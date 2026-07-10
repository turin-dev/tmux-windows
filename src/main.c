/* main.c — tmuxw entry point and command dispatch (Phase 3).
 *
 * Running `tmux`/`tmuxw` connects to a background server for a session, starting
 * one if needed, and attaches as a thin client. The server owns all panes and
 * survives detach (Ctrl-B d); reattaching resumes the session.
 *
 *   tmux                                  attach to (or start) the default session
 *   tmux [-f file] new [-s name] [-c dir] [-x w -y h] [-d] [cmd]
 *                                         start a session (alias: new-session);
 *                                         -d starts it without attaching, -f
 *                                         loads an alternate config file
 *   tmux attach [-t name] [-d]             attach to an existing session (alias: a);
 *                                         -d kicks any client already attached to it
 *   tmux has-session [-t name]            exit 0 if the session is running (alias: has)
 *   tmux kill-session [-t name] [-a]      stop one session's server, or (-a) every
 *                                         other one
 *   tmux <cmd> [-t name] [args...]         run any other command (send-keys,
 *                                         new-window, rename-session, ...)
 *                                         against a session, attached or not
 *   tmux -V | --version                   print the version
 *   tmux --standalone [cmd]               run a session in one process (no server; debug)
 *   tmux --server <pipe> [--cwd dir] [--size WxH] [cmd]
 *                                         internal: the background server process
 *   tmux --selftest [cmd]                 headless: ConPTY output straight to stdout
 *   tmux --selftest-render [cmd]          headless: ConPTY -> libvterm -> dump grid
 *   tmux --selftest-split                 headless: two panes + compositor
 *   tmux --selftest-ipc                   headless: full server/client round trip
 *   tmux --selftest-cmdipc                headless: one-off commands vs. a detached session
 *   tmux --selftest-attachd               headless: attach -d kicks an attached client
 *   tmux --selftest-switch                headless: switch-client sends MSG_SWITCH
 *   tmux --selftest-link                  headless: link-window shares one window across two indices
 *   tmux --selftest-confirm               headless: confirm-before, capture-pane, etc.
 *
 * The binary is installed under both names: `tmux` (familiar) and `tmuxw`.
 */
#include "platform/conpty.h"
#include "platform/winterm.h"
#include "platform/ipc.h"
#include "emu/screen.h"
#include "model/pane.h"
#include "layout.h"
#include "render.h"
#include "session.h"
#include "server.h"
#include "client.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_BUF   8192
#define CMD_MAX  32768

/* -L <name> / -S <path>: an independent namespace of sessions, set once at
 * startup from a global flag (tmux's -L/-S). Windows named pipes have no
 * real notion of a "socket path" the way -S implies on Unix, so both are
 * treated the same way here: a token folded into every pipe name this
 * invocation computes, so `-L work` and the default namespace never see
 * each other's sessions even if they happen to share a session name. */
static wchar_t g_namespace[256] = L"";

/* Resolve `session` (NULL/empty -> "default") against g_namespace into a
 * single token suitable for ipc_pipe_name/ipc_cmd_pipe_name. */
static void ns_session_name(const wchar_t *session, wchar_t *out, size_t cap)
{
    const wchar_t *base = (session && session[0]) ? session : L"default";
    if (g_namespace[0])
        _snwprintf_s(out, cap, _TRUNCATE, L"%s~%s", g_namespace, base);
    else
        wcscpy_s(out, cap, base);
}

/* ----- small helpers -------------------------------------------------------- */

static void join_cmdline(wchar_t *out, int argc, wchar_t **argv, int start)
{
    int i;
    out[0] = L'\0';
    for (i = start; i < argc; i++) {
        if (i > start) wcscat_s(out, CMD_MAX, L" ");
        wcscat_s(out, CMD_MAX, argv[i]);
    }
}

/* pwsh (PowerShell 7+) > powershell > cmd. */
static const wchar_t *default_shell(void)
{
    wchar_t path[MAX_PATH];
    if (SearchPathW(NULL, L"pwsh.exe", NULL, MAX_PATH, path, NULL) > 0)
        return L"pwsh.exe";
    if (SearchPathW(NULL, L"powershell.exe", NULL, MAX_PATH, path, NULL) > 0)
        return L"powershell.exe";
    return L"cmd.exe";
}

/* Prevent our (possibly inheritable) standard handles from leaking into a
 * pseudo console's conhost, which would misroute a child shell's stdout. */
static void disinherit_std_handles(void)
{
    static const DWORD ids[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    int i;
    for (i = 0; i < 3; i++) {
        HANDLE h = GetStdHandle(ids[i]);
        if (h && h != INVALID_HANDLE_VALUE)
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    }
}

/* Run `exe args` hidden, wait up to 10s, and return its exit code (or a
 * Win32 error code if it could not even be started). */
static int run_hidden(const wchar_t *exe, const wchar_t *args)
{
    wchar_t cmd[CMD_MAX + 256];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;

    _snwprintf_s(cmd, CMD_MAX + 256, _TRUNCATE, L"%s %s", exe, args);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi))
        return (int)GetLastError();

    WaitForSingleObject(pi.hProcess, 10000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* Escape `in` for embedding inside an already-quoted schtasks /tr value, per
 * the scheme documented for schtasks: each literal '"' becomes '\"'. */
static void escape_quotes(const wchar_t *in, wchar_t *out, size_t out_cap)
{
    size_t i, j = 0, n = wcslen(in);
    for (i = 0; i < n && j + 2 < out_cap; i++) {
        if (in[i] == L'"') {
            out[j++] = L'\\';
            out[j++] = L'"';
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = L'\0';
}

/* Launch `cmd` via the Task Scheduler service so the resulting process is not
 * a member of any job the caller belongs to. The Schedule service (running
 * independently as SYSTEM) creates the target process itself, so it is never
 * a descendant of our process tree at all -- unlike CREATE_BREAKAWAY_FROM_JOB,
 * this works even when the enclosing job denies breakaway, which is exactly
 * how Win32-OpenSSH's per-session job is configured. Returns 0 if the task
 * was created and run (the target process is presumed launched); nonzero on
 * failure, in which case the caller should fall back to a direct spawn. */
static int spawn_via_scheduled_task(const wchar_t *cmd)
{
    wchar_t taskname[64];
    wchar_t escaped[CMD_MAX + 16];
    wchar_t args[CMD_MAX + 256];
    wchar_t sttime[8];
    SYSTEMTIME st;
    unsigned total, hh, mm;

    _snwprintf_s(taskname, 64, _TRUNCATE, L"tmuxw-srv-%lu-%lu",
                 (unsigned long)GetCurrentProcessId(), GetTickCount());

    GetLocalTime(&st);
    total = (unsigned)st.wHour * 60u + (unsigned)st.wMinute + 1u; /* avoid a start time already past */
    hh = (total / 60u) % 24u;
    mm = total % 60u;
    _snwprintf_s(sttime, 8, _TRUNCATE, L"%02u:%02u", hh, mm);

    escape_quotes(cmd, escaped, CMD_MAX + 16);
    _snwprintf_s(args, CMD_MAX + 256, _TRUNCATE,
                L"/create /tn \"%s\" /tr \"%s\" /sc once /st %s /f",
                taskname, escaped, sttime);
    if (run_hidden(L"schtasks.exe", args) != 0)
        return -1;

    _snwprintf_s(args, CMD_MAX + 256, _TRUNCATE, L"/run /tn \"%s\"", taskname);
    if (run_hidden(L"schtasks.exe", args) != 0) {
        _snwprintf_s(args, CMD_MAX + 256, _TRUNCATE, L"/delete /tn \"%s\" /f", taskname);
        run_hidden(L"schtasks.exe", args);
        return -1;
    }

    Sleep(300);   /* give the Schedule service a moment to launch the target */
    _snwprintf_s(args, CMD_MAX + 256, _TRUNCATE, L"/delete /tn \"%s\" /f", taskname);
    run_hidden(L"schtasks.exe", args);   /* the task definition, not the process it launched */
    return 0;
}

/* Launch the background server process for `pipename`. On success, if
 * out_process is non-NULL, receives the server process handle (caller closes);
 * otherwise the handle is closed here. Returns 0 on success.
 *
 * SSH servers on Windows (OpenSSH's sshd in particular) commonly put a login
 * session's whole process tree into a Job Object with
 * JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so that everything spawned during the
 * session is torn down when it ends. Without countermeasures, that would take
 * our detached server down the moment the SSH connection drops -- exactly the
 * scenario a detachable session exists to survive. We try, in order:
 *   1. A plain spawn when we're not in a job at all (the common local case).
 *   2. CREATE_BREAKAWAY_FROM_JOB, in case the job allows it.
 *   3. Task Scheduler (see spawn_via_scheduled_task): Win32-OpenSSH's session
 *      job does NOT set JOB_OBJECT_LIMIT_BREAKAWAY_OK, so (2) reliably fails
 *      for the exact scenario we care about (an interactive SSH session) --
 *      this is the fallback that actually survives disconnect there.
 *   4. A plain in-job spawn, so a session still starts even if every escape
 *      attempt failed (better than refusing to run at all).
 * Because (3) launches the process via the Schedule service rather than as
 * our own child, no process handle is available for it; *out_process is set
 * to NULL in that case. */
static int spawn_server_ex(const wchar_t *pipename, const wchar_t *shell,
                           const wchar_t *cwd, int cols, int rows, const wchar_t *cfg,
                           HANDLE *out_process)
{
    wchar_t exe[MAX_PATH];
    wchar_t cmd[CMD_MAX];
    wchar_t extra[2 * MAX_PATH + 128];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD flags = CREATE_NO_WINDOW | DETACHED_PROCESS;
    BOOL in_job = FALSE;
    BOOL ok;

    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0)
        return (int)GetLastError();

    extra[0] = L'\0';
    if (cwd && cwd[0]) {
        wchar_t part[MAX_PATH + 16];
        _snwprintf_s(part, MAX_PATH + 16, _TRUNCATE, L"--cwd \"%s\" ", cwd);
        wcscat_s(extra, 2 * MAX_PATH + 128, part);
    }
    if (cols > 0 && rows > 0) {
        wchar_t part[64];
        _snwprintf_s(part, 64, _TRUNCATE, L"--size %dx%d ", cols, rows);
        wcscat_s(extra, 2 * MAX_PATH + 128, part);
    }
    if (cfg && cfg[0]) {
        wchar_t part[MAX_PATH + 16];
        _snwprintf_s(part, MAX_PATH + 16, _TRUNCATE, L"--cfg \"%s\" ", cfg);
        wcscat_s(extra, 2 * MAX_PATH + 128, part);
    }
    _snwprintf_s(cmd, CMD_MAX, _TRUNCATE, L"\"%s\" --server %s %s%s", exe, pipename, extra, shell);

    if (!IsProcessInJob(GetCurrentProcess(), NULL, &in_job))
        in_job = FALSE;

    if (in_job)
        flags |= CREATE_BREAKAWAY_FROM_JOB;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);

    if (!ok && in_job) {
        /* Breakaway refused: escape the job entirely via Task Scheduler. */
        if (spawn_via_scheduled_task(cmd) == 0) {
            if (out_process) *out_process = NULL;
            return 0;
        }
        /* Scheduler unavailable too: settle for a session that works locally
         * even if it won't survive this job being torn down. */
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        ZeroMemory(&pi, sizeof(pi));
        ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);
    }
    if (!ok)
        return (int)GetLastError();

    CloseHandle(pi.hThread);
    if (out_process)
        *out_process = pi.hProcess;
    else
        CloseHandle(pi.hProcess);
    return 0;
}

/* Forward declaration: defined later alongside the other cmd-pipe helpers,
 * but attach_session (attach -d) needs it too. */
static int send_one_cmd(const wchar_t *cmdpipename, const char *cmdline);

/* Attach to session `name` (NULL/empty -> "default") as a thin client. If no
 * server is running: when start_if_missing is set, spawn one running `shell`
 * (starting in `cwd`, at `cols` x `rows` if given, loading `cfg` as its
 * config instead of the default ~/.tmuxw.conf when non-NULL -- tmux's -f);
 * otherwise fail (the `attach` command must not create sessions). If
 * `start_detached` is set (new-session -d), the server is started/left
 * running but this returns without attaching a client, mirroring tmux's
 * `-d`. If `detach_others` is set (attach -d) and another client is already
 * attached, that client is kicked -- the same as running detach-client
 * against the session -- before we attach in its place. */
static int attach_session(const wchar_t *name, const wchar_t *shell, int start_if_missing,
                          const wchar_t *cwd, int cols, int rows, int start_detached,
                          int detach_others, const wchar_t *cfg)
{
    wchar_t pipename[512], nsname[512];
    HANDLE pipe;
    int busy = 0;

    ns_session_name(name, nsname, 512);
    ipc_pipe_name(pipename, 512, nsname);

    pipe = ipc_client_connect_ex(pipename, 0, &busy);
    if (pipe == INVALID_HANDLE_VALUE && busy && detach_others) {
        wchar_t cmdpipename[512];
        ipc_cmd_pipe_name(cmdpipename, 512, nsname);
        if (send_one_cmd(cmdpipename, "detach-client") == 0)
            pipe = ipc_client_connect_ex(pipename, 3000, &busy);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        int rc;
        if (!start_if_missing) {
            if (busy)
                fprintf(stderr, "tmux: session '%ls' already has a client attached\n",
                        (name && name[0]) ? name : L"default");
            else
                fprintf(stderr, "tmux: no server running for session '%ls'\n",
                        (name && name[0]) ? name : L"default");
            return 1;
        }
        rc = spawn_server_ex(pipename, shell, cwd, cols, rows, cfg, NULL);
        if (rc != 0) {
            fprintf(stderr, "tmux: failed to start server (error %d)\n", rc);
            return 1;
        }
        if (start_detached) {
            /* Wait for the pipe to come up so a following `tmux ls` reliably
             * sees it, then return without attaching a client. */
            pipe = ipc_client_connect(pipename, 5000);
            if (pipe != INVALID_HANDLE_VALUE)
                CloseHandle(pipe);
            return 0;
        }
        pipe = ipc_client_connect(pipename, 5000);
        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "tmux: could not connect to server\n");
            return 1;
        }
    } else if (start_detached) {
        CloseHandle(pipe);   /* session already running; -d is a no-op here */
        return 0;
    }
    return run_client(pipe, g_namespace);
}

/* Parsed arguments for a client subcommand (new-session, attach, kill-session,
 * has-session, ...): a `-s`/`-t` target, `-c` start directory, `-x`/`-y`
 * initial size, `-d` (start detached / detach other clients, depending on the
 * command), `-a` (command-specific "all"), `-n` (initial window name --
 * parsed but not yet applied), and any remaining tokens joined as the command
 * to run. All string fields are always NUL-terminated. */
typedef struct {
    wchar_t name[256];
    wchar_t cwd[MAX_PATH];
    wchar_t winname[64];
    wchar_t shell[CMD_MAX];
    int     has_shell;
    int     cols, rows;   /* 0 = unset */
    int     detach;       /* -d */
    int     all;          /* -a */
} target_args_t;

static void parse_target_args(int argc, wchar_t **argv, int start, target_args_t *a)
{
    int i;
    ZeroMemory(a, sizeof(*a));
    for (i = start; i < argc; i++) {
        const wchar_t *arg = argv[i];
        if ((wcscmp(arg, L"-s") == 0 || wcscmp(arg, L"-t") == 0) && i + 1 < argc) {
            wcscpy_s(a->name, 256, argv[++i]);
        } else if (wcscmp(arg, L"-c") == 0 && i + 1 < argc) {
            wcscpy_s(a->cwd, MAX_PATH, argv[++i]);
        } else if (wcscmp(arg, L"-n") == 0 && i + 1 < argc) {
            wcscpy_s(a->winname, 64, argv[++i]);
        } else if (wcscmp(arg, L"-x") == 0 && i + 1 < argc) {
            a->cols = _wtoi(argv[++i]);
        } else if (wcscmp(arg, L"-y") == 0 && i + 1 < argc) {
            a->rows = _wtoi(argv[++i]);
        } else if (wcscmp(arg, L"-d") == 0) {
            a->detach = 1;
        } else if (wcscmp(arg, L"-a") == 0) {
            a->all = 1;
        } else if (arg[0] == L'-' && arg[1] != L'\0') {
            /* Unrecognized flag: ignore for now (kept forward-compatible). */
        } else {
            if (a->has_shell) wcscat_s(a->shell, CMD_MAX, L" ");
            wcscat_s(a->shell, CMD_MAX, arg);
            a->has_shell = 1;
        }
    }
}

/* ----- session listing / killing (client side) ------------------------------ */

#define SESS_MAX   64
#define SESS_NAME  64

/* Filter+strip a raw (possibly "<namespace>~<name>") pipe-derived session
 * name for the currently active -L/-S namespace (g_namespace). Returns 1 if
 * it belongs to the current namespace (with `out` set to its display name),
 * 0 if it belongs to some other namespace. */
static int ns_filter(const char *raw, char *out, size_t outcap)
{
    char nsutf8[256];
    size_t nslen;
    WideCharToMultiByte(CP_UTF8, 0, g_namespace, -1, nsutf8, sizeof(nsutf8), NULL, NULL);
    nslen = strlen(nsutf8);
    if (nslen == 0) {
        if (strchr(raw, '~') != NULL)
            return 0;
        strncpy_s(out, outcap, raw, _TRUNCATE);
        return 1;
    }
    if (strncmp(raw, nsutf8, nslen) == 0 && raw[nslen] == '~') {
        strncpy_s(out, outcap, raw + nslen + 1, _TRUNCATE);
        return 1;
    }
    return 0;
}

static int list_sessions(void)
{
    char names[SESS_MAX * SESS_NAME];
    int n = ipc_list_sessions(names, SESS_MAX, SESS_NAME), i, shown = 0;
    for (i = 0; i < n; i++) {
        char disp[SESS_NAME];
        if (ns_filter(names + (size_t)i * SESS_NAME, disp, sizeof(disp))) {
            printf("%s\n", disp);
            shown++;
        }
    }
    if (shown == 0)
        printf("no sessions\n");
    return 0;
}

/* Connect to a session's server and ask it to exit. Returns 0 if the request
 * was delivered, 1 if no such session is running (or, if out_busy is
 * non-NULL, it could exist but already has a client attached -- with only
 * one pipe instance per session we can't get in to signal it right now). */
static int kill_one_ex(const wchar_t *name, int *out_busy)
{
    wchar_t pipename[512], nsname[512];
    HANDLE pipe;
    ns_session_name(name, nsname, 512);
    ipc_pipe_name(pipename, 512, nsname);
    pipe = ipc_client_connect_ex(pipename, 0, out_busy);
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;
    ipc_write_frame(pipe, MSG_KILL, NULL, 0);
    Sleep(100);                 /* let the server act before we drop the pipe */
    CloseHandle(pipe);
    return 0;
}

static int kill_one(const wchar_t *name)
{
    return kill_one_ex(name, NULL);
}

static int kill_session_cmd(const wchar_t *name)
{
    int busy = 0;
    if (kill_one_ex(name, &busy) != 0) {
        if (busy)
            fprintf(stderr, "tmux: session '%ls' has a client attached; detach it first\n",
                    (name && name[0]) ? name : L"default");
        else
            fprintf(stderr, "tmux: no such session\n");
        return 1;
    }
    return 0;
}

/* kill-session -a -t <name>: kill every session except `name`. */
static int kill_session_others_cmd(const wchar_t *name)
{
    char names[SESS_MAX * SESS_NAME];
    int n = ipc_list_sessions(names, SESS_MAX, SESS_NAME), i, killed = 0;
    const wchar_t *keep = (name && name[0]) ? name : L"default";
    for (i = 0; i < n; i++) {
        char disp[SESS_NAME];
        wchar_t w[SESS_NAME * 2];
        if (!ns_filter(names + (size_t)i * SESS_NAME, disp, sizeof(disp)))
            continue;   /* a different -L/-S namespace; leave it alone */
        MultiByteToWideChar(CP_UTF8, 0, disp, -1, w, SESS_NAME * 2);
        if (wcscmp(w, keep) == 0)
            continue;
        if (kill_one(w) == 0)
            killed++;
    }
    printf("killed %d session(s)\n", killed);
    return 0;
}

/* has-session -t <name>: exit 0 if a server for that session is running,
 * whether or not we can actually connect to it right now (a session with a
 * client already attached is still very much "there"), else print an error
 * and exit 1, matching tmux. */
static int has_session_cmd(const wchar_t *name)
{
    wchar_t pipename[512], nsname[512];
    HANDLE pipe;
    int busy = 0;
    ns_session_name(name, nsname, 512);
    ipc_pipe_name(pipename, 512, nsname);
    pipe = ipc_client_connect_ex(pipename, 0, &busy);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (busy)
            return 0;
        fprintf(stderr, "tmux: can't find session '%ls'\n", (name && name[0]) ? name : L"default");
        return 1;
    }
    CloseHandle(pipe);
    return 0;
}

static int kill_server_cmd(void)
{
    char names[SESS_MAX * SESS_NAME];
    int n = ipc_list_sessions(names, SESS_MAX, SESS_NAME), i, killed = 0;
    for (i = 0; i < n; i++) {
        char disp[SESS_NAME];
        wchar_t w[SESS_NAME * 2];
        if (!ns_filter(names + (size_t)i * SESS_NAME, disp, sizeof(disp)))
            continue;   /* a different -L/-S namespace; leave it alone */
        MultiByteToWideChar(CP_UTF8, 0, disp, -1, w, SESS_NAME * 2);
        if (kill_one(w) == 0)
            killed++;
    }
    printf("killed %d session(s)\n", killed);
    return 0;
}

/* ----- headless self-tests -------------------------------------------------- */

typedef struct collect_ctx {
    conpty_t         *pty;
    strbuf_t         *raw;
    CRITICAL_SECTION *lock;
} collect_ctx_t;

static DWORD WINAPI collect_reader(LPVOID arg)
{
    collect_ctx_t *c = (collect_ctx_t *)arg;
    char buf[IO_BUF];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(c->pty->output_read, buf, sizeof(buf), &n, NULL) || n == 0)
            break;
        EnterCriticalSection(c->lock);
        strbuf_append(c->raw, buf, n);
        LeaveCriticalSection(c->lock);
    }
    return 0;
}

static int collect_output(const wchar_t *cmdline, strbuf_t *raw)
{
    conpty_t pty;
    collect_ctx_t ctx;
    CRITICAL_SECTION lock;
    HANDLE h;
    int rc = conpty_spawn(&pty, cmdline, 80, 25, NULL, NULL);
    if (rc != 0)
        return rc;

    InitializeCriticalSection(&lock);
    ctx.pty = &pty;
    ctx.raw = raw;
    ctx.lock = &lock;
    h = CreateThread(NULL, 0, collect_reader, &ctx, 0, NULL);

    WaitForSingleObject(pty.process, 5000);
    Sleep(150);
    conpty_close(&pty);
    if (h) { WaitForSingleObject(h, 2000); CloseHandle(h); }
    DeleteCriticalSection(&lock);
    return 0;
}

/* First non-empty grid row of a screen as ASCII, into `buf`. */
static void screen_first_line(screen_t *s, char *buf, int buflen)
{
    int rows = screen_rows(s), cols = screen_cols(s), row, col;
    buf[0] = '\0';
    for (row = 0; row < rows; row++) {
        int n = 0;
        for (col = 0; col < cols && n < buflen - 1; col++) {
            VTermScreenCell cell;
            if (!screen_get_cell(s, row, col, &cell) || cell.width == 0)
                continue;
            buf[n++] = (cell.chars[0] && cell.chars[0] < 128) ? (char)cell.chars[0] : ' ';
        }
        while (n > 0 && buf[n - 1] == ' ') n--;
        buf[n] = '\0';
        if (n > 0)
            return;
    }
}

/* Does any grid row contain `needle`? */
static int screen_contains(screen_t *s, const char *needle)
{
    int rows = screen_rows(s), cols = screen_cols(s), row, col;
    char line[512];
    for (row = 0; row < rows; row++) {
        int n = 0;
        for (col = 0; col < cols && n < (int)sizeof(line) - 1; col++) {
            VTermScreenCell cell;
            if (!screen_get_cell(s, row, col, &cell) || cell.width == 0)
                continue;
            line[n++] = (cell.chars[0] && cell.chars[0] < 128) ? (char)cell.chars[0] : ' ';
        }
        line[n] = '\0';
        if (strstr(line, needle))
            return 1;
    }
    return 0;
}

static int run_selftest(int argc, wchar_t **argv)
{
    wchar_t cmdline[CMD_MAX];
    strbuf_t raw;
    DWORD written = 0;

    if (argc > 2) join_cmdline(cmdline, argc, argv, 2);
    else          wcscpy_s(cmdline, CMD_MAX, L"cmd.exe /c echo tmuxw-selftest-ok");

    strbuf_init(&raw);
    if (collect_output(cmdline, &raw) != 0) {
        fprintf(stderr, "selftest: conpty_spawn failed\n");
        strbuf_free(&raw);
        return 1;
    }
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), raw.data, (DWORD)raw.len, &written, NULL);
    strbuf_free(&raw);
    return 0;
}

static int run_selftest_render(int argc, wchar_t **argv)
{
    wchar_t cmdline[CMD_MAX];
    strbuf_t raw;
    screen_t *screen;
    int row, col, rows, cols;
    char line[512];

    if (argc > 2) join_cmdline(cmdline, argc, argv, 2);
    else          wcscpy_s(cmdline, CMD_MAX, L"cmd.exe /c echo tmuxw-render-ok");

    strbuf_init(&raw);
    if (collect_output(cmdline, &raw) != 0) {
        fprintf(stderr, "selftest-render: conpty_spawn failed\n");
        strbuf_free(&raw);
        return 1;
    }

    screen = screen_new(80, 25);
    screen_write(screen, raw.data, raw.len);

    rows = screen_rows(screen);
    cols = screen_cols(screen);
    for (row = 0; row < rows; row++) {
        int n = 0;
        for (col = 0; col < cols && n < (int)sizeof(line) - 1; col++) {
            VTermScreenCell cell;
            if (!screen_get_cell(screen, row, col, &cell) || cell.width == 0)
                continue;
            line[n++] = (cell.chars[0] && cell.chars[0] < 128) ? (char)cell.chars[0] : ' ';
        }
        while (n > 0 && line[n - 1] == ' ') n--;
        line[n] = '\0';
        if (n > 0)
            printf("%2d| %s\n", row, line);
    }
    fflush(stdout);

    screen_free(screen);
    strbuf_free(&raw);
    return 0;
}

/* Two real panes side by side, composited — proves pane + layout + compositor. */
static int run_selftest_split(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    pane_t *a = pane_create(1, L"cmd.exe /c echo PANE_A_TEXT", 40, 6, wake, NULL, NULL);
    pane_t *b = pane_create(2, L"cmd.exe /c echo PANE_B_TEXT", 40, 6, wake, NULL, NULL);
    layout_node_t *root;
    strbuf_t frame;
    char la[128], lb[128];
    int ok = 1;

    if (a == NULL || b == NULL) {
        fprintf(stderr, "selftest-split: pane_create failed\n");
        if (a) pane_close(a);
        if (b) pane_close(b);
        if (wake) CloseHandle(wake);
        return 1;
    }

    root = layout_leaf(a);
    layout_split(&root, root, LN_SPLIT_V, b);
    layout_apply(root, 0, 0, 40, 6);

    Sleep(600);
    pane_pump(a);
    pane_pump(b);

    printf("pane A: x=%d y=%d cols=%d rows=%d\n", a->x, a->y, a->cols, a->rows);
    printf("pane B: x=%d y=%d cols=%d rows=%d\n", b->x, b->y, b->cols, b->rows);

    screen_first_line(a->screen, la, sizeof(la));
    screen_first_line(b->screen, lb, sizeof(lb));
    printf("pane A text: [%s]\n", la);
    printf("pane B text: [%s]\n", lb);

    strbuf_init(&frame);
    render_clear(&frame);
    layout_draw_borders(root, &frame);
    screen_mark_all_dirty(a->screen);
    screen_mark_all_dirty(b->screen);
    render_pane(&frame, a);
    render_pane(&frame, b);
    strbuf_putc(&frame, '\0');

    if (!(a->x == 0 && b->x == 21 && a->cols == 20 && b->cols == 19)) {
        printf("FAIL: unexpected geometry\n"); ok = 0;
    }
    if (strstr(la, "PANE_A_TEXT") == NULL) { printf("FAIL: pane A text missing\n"); ok = 0; }
    if (strstr(lb, "PANE_B_TEXT") == NULL) { printf("FAIL: pane B text missing\n"); ok = 0; }
    if (strstr(frame.data, "\x1b[1;22H") == NULL) { printf("FAIL: pane B not composited at offset\n"); ok = 0; }
    if (strstr(frame.data, "\xe2\x94\x82") == NULL) { printf("FAIL: divider missing\n"); ok = 0; }

    printf("%s\n", ok ? "SPLIT SELFTEST PASSED" : "SPLIT SELFTEST FAILED");

    strbuf_free(&frame);
    layout_free(root, 1);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* Full server/client round trip: start a server, connect, type a command,
 * collect the composited output, and verify the echoed text appears. */
typedef struct ipctest {
    HANDLE            pipe;
    CRITICAL_SECTION  lock;
    strbuf_t          out;
    volatile LONG     stopped;
} ipctest_t;

static DWORD WINAPI ipctest_reader(LPVOID arg)
{
    ipctest_t *t = (ipctest_t *)arg;
    unsigned char type;
    strbuf_t p;
    strbuf_init(&p);
    for (;;) {
        if (ipc_read_frame(t->pipe, &type, &p) != 0)
            break;
        if (type == MSG_OUTPUT) {
            EnterCriticalSection(&t->lock);
            strbuf_append(&t->out, p.data, p.len);
            LeaveCriticalSection(&t->lock);
        } else if (type == MSG_EXIT || type == MSG_DETACH) {
            break;
        }
    }
    strbuf_free(&p);
    InterlockedExchange(&t->stopped, 1);
    return 0;
}

static int run_selftest_ipc(void)
{
    wchar_t pipename[512];
    ipctest_t t;
    HANDLE reader, server = NULL;
    screen_t *screen;
    unsigned char sz[4];
    const char *typed = "echo IPC_HELLO_MARKER\r";
    int ok = 1;

    ipc_pipe_name(pipename, 512, L"selftest");

    if (spawn_server_ex(pipename, L"cmd.exe", NULL, 0, 0, NULL, &server) != 0) {
        printf("FAIL: could not spawn server\n");
        return 1;
    }

    memset(&t, 0, sizeof(t));
    InitializeCriticalSection(&t.lock);
    strbuf_init(&t.out);
    t.pipe = ipc_client_connect(pipename, 5000);
    if (t.pipe == INVALID_HANDLE_VALUE) {
        printf("FAIL: could not connect to server\n");
        if (server) { TerminateProcess(server, 1); CloseHandle(server); }
        DeleteCriticalSection(&t.lock);
        return 1;
    }

    reader = CreateThread(NULL, 0, ipctest_reader, &t, 0, NULL);

    ipc_pack_size(sz, 80, 25);
    ipc_write_frame(t.pipe, MSG_ATTACH, sz, 4);
    Sleep(500);                                     /* shell startup */
    ipc_write_frame(t.pipe, MSG_INPUT, typed, (uint32_t)strlen(typed));
    Sleep(800);                                     /* run + render */

    /* Parse the accumulated composited output. */
    screen = screen_new(80, 25);
    EnterCriticalSection(&t.lock);
    printf("collected %zu output bytes\n", t.out.len);
    screen_write(screen, t.out.data, t.out.len);
    LeaveCriticalSection(&t.lock);

    if (!screen_contains(screen, "IPC_HELLO_MARKER")) {
        printf("FAIL: echoed text not found in composited output\n");
        ok = 0;
    }
    printf("%s\n", ok ? "IPC SELFTEST PASSED" : "IPC SELFTEST FAILED");

    /* Ask the server to exit over the pipe -- this works even when `server`
     * is NULL (e.g. it escaped a restrictive job via Task Scheduler, so we
     * never got a process handle for it; see spawn_server_ex). Then unblock
     * the reader (parked in ReadFile) and clean up. */
    ipc_write_frame(t.pipe, MSG_KILL, NULL, 0);
    Sleep(150);
    CancelIoEx(t.pipe, NULL);
    CloseHandle(t.pipe);
    if (reader) { WaitForSingleObject(reader, 1500); CloseHandle(reader); }
    if (server) {
        if (WaitForSingleObject(server, 1000) != WAIT_OBJECT_0)
            TerminateProcess(server, 0);
        CloseHandle(server);
    }
    screen_free(screen);
    strbuf_free(&t.out);
    DeleteCriticalSection(&t.lock);
    return ok ? 0 : 1;
}

/* Send one command line to a session's cmd pipe and wait for the reply. A
 * command that produces text (list-windows, list-panes) has it printed to
 * stdout. Returns 0 on success. */
static int send_one_cmd(const wchar_t *cmdpipename, const char *cmdline)
{
    HANDLE pipe = ipc_client_connect(cmdpipename, 3000);
    unsigned char type;
    strbuf_t reply;
    int ok;

    if (pipe == INVALID_HANDLE_VALUE)
        return 1;
    ipc_write_frame(pipe, MSG_CMD, cmdline, (uint32_t)strlen(cmdline));
    strbuf_init(&reply);
    ok = (ipc_read_frame(pipe, &type, &reply) == 0 &&
          (type == MSG_CMD_OK || type == MSG_CMD_TEXT));
    if (ok && type == MSG_CMD_TEXT && reply.len > 0)
        fwrite(reply.data, 1, reply.len, stdout);
    strbuf_free(&reply);
    CloseHandle(pipe);
    return ok ? 0 : 1;
}

/* Like send_one_cmd, but for a test that needs to inspect a MSG_CMD_TEXT
 * reply (list-windows, list-panes) rather than just its success/failure. */
static int send_cmd_capture(const wchar_t *cmdpipename, const char *cmdline,
                            char *out, size_t outcap)
{
    HANDLE pipe = ipc_client_connect(cmdpipename, 3000);
    unsigned char type;
    strbuf_t reply;
    int ok;

    out[0] = '\0';
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;
    ipc_write_frame(pipe, MSG_CMD, cmdline, (uint32_t)strlen(cmdline));
    strbuf_init(&reply);
    ok = (ipc_read_frame(pipe, &type, &reply) == 0 &&
          (type == MSG_CMD_OK || type == MSG_CMD_TEXT));
    if (ok && type == MSG_CMD_TEXT) {
        size_t n = reply.len < outcap - 1 ? reply.len : outcap - 1;
        memcpy(out, reply.data, n);
        out[n] = '\0';
    }
    strbuf_free(&reply);
    CloseHandle(pipe);
    return ok ? 0 : 1;
}

/* Commands sent over the cmd pipe reach a session whether or not a client is
 * attached: send-keys/rename-session-style commands while fully detached,
 * then again after attaching, and confirm both took effect. Also covers
 * list-windows/list-panes, which reply with their output as text (MSG_CMD_TEXT)
 * instead of a bare ack. */
static int run_selftest_cmdipc(void)
{
    wchar_t pipename[512], cmdpipename[512];
    HANDLE server = NULL, pipe;
    unsigned char sz[4];
    strbuf_t frame;
    int ok = 1;

    ipc_pipe_name(pipename, 512, L"cmdiptest");
    ipc_cmd_pipe_name(cmdpipename, 512, L"cmdiptest");

    if (spawn_server_ex(pipename, L"cmd.exe", NULL, 0, 0, NULL, &server) != 0) {
        printf("FAIL: could not spawn server\n");
        return 1;
    }
    Sleep(300);   /* let the session (and its cmd-pipe acceptor thread) come up */

    /* While nobody is attached: rename the session and open a second window. */
    if (send_one_cmd(cmdpipename, "rename-session CMDIPCTEST") != 0) {
        printf("FAIL: detached rename-session command not acked\n");
        ok = 0;
    }
    if (send_one_cmd(cmdpipename, "new-window") != 0) {
        printf("FAIL: detached new-window command not acked\n");
        ok = 0;
    }

    /* list-windows/list-panes reply with their output as text, even while
     * fully detached. */
    {
        char text[2048];
        if (send_cmd_capture(cmdpipename, "list-windows", text, sizeof(text)) != 0) {
            printf("FAIL: detached list-windows command not acked\n");
            ok = 0;
        } else if (strstr(text, "0:") == NULL || strstr(text, "1:") == NULL) {
            printf("FAIL: list-windows text missing expected windows: [%s]\n", text);
            ok = 0;
        }
        if (send_cmd_capture(cmdpipename, "list-panes", text, sizeof(text)) != 0) {
            printf("FAIL: detached list-panes command not acked\n");
            ok = 0;
        } else if (strstr(text, "0:") == NULL) {
            printf("FAIL: list-panes text missing expected pane: [%s]\n", text);
            ok = 0;
        }
    }

    /* Now attach and confirm both detached commands actually took effect. */
    pipe = ipc_client_connect(pipename, 3000);
    if (pipe == INVALID_HANDLE_VALUE) {
        printf("FAIL: could not attach after detached commands\n");
        if (server) { TerminateProcess(server, 1); CloseHandle(server); }
        return 1;
    }
    ipc_pack_size(sz, 80, 25);
    ipc_write_frame(pipe, MSG_ATTACH, sz, 4);
    strbuf_init(&frame);
    {
        unsigned char type;
        strbuf_t payload;
        strbuf_init(&payload);
        Sleep(300);
        /* Drain whatever frames arrived so far without blocking forever. */
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) || avail == 0)
                break;
            if (ipc_read_frame(pipe, &type, &payload) != 0)
                break;
            if (type == MSG_OUTPUT)
                strbuf_append(&frame, payload.data, payload.len);
        }
        strbuf_free(&payload);
    }
    strbuf_putc(&frame, '\0');

    if (strstr(frame.data, "CMDIPCTEST") == NULL) {
        printf("FAIL: rename-session sent while detached did not apply\n");
        ok = 0;
    }
    if (strstr(frame.data, "1:cmd*") == NULL) {
        printf("FAIL: new-window sent while detached did not apply\n");
        ok = 0;
    }

    /* And once attached, a cmd-pipe command should still land (drained by
     * serve_client's loop rather than the outer accept loop). */
    if (send_one_cmd(cmdpipename, "rename-window ATTACHEDCMD") != 0) {
        printf("FAIL: attached rename-window command not acked\n");
        ok = 0;
    }
    Sleep(300);
    strbuf_clear(&frame);
    {
        unsigned char type;
        strbuf_t payload;
        strbuf_init(&payload);
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) || avail == 0)
                break;
            if (ipc_read_frame(pipe, &type, &payload) != 0)
                break;
            if (type == MSG_OUTPUT)
                strbuf_append(&frame, payload.data, payload.len);
        }
        strbuf_free(&payload);
    }
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "ATTACHEDCMD") == NULL) {
        printf("FAIL: rename-window sent while attached did not apply\n");
        ok = 0;
    }

    printf("%s\n", ok ? "CMDIPC SELFTEST PASSED" : "CMDIPC SELFTEST FAILED");

    /* Ask the server to exit over the pipe -- this works even when `server`
     * is NULL (e.g. it escaped a restrictive job via Task Scheduler, so we
     * never got a process handle for it; see spawn_server_ex). */
    ipc_write_frame(pipe, MSG_KILL, NULL, 0);
    Sleep(150);
    CancelIoEx(pipe, NULL);
    CloseHandle(pipe);
    strbuf_free(&frame);
    if (server) {
        if (WaitForSingleObject(server, 1000) != WAIT_OBJECT_0)
            TerminateProcess(server, 0);
        CloseHandle(server);
    }
    return ok ? 0 : 1;
}

typedef struct attachd_client {
    HANDLE         pipe;
    volatile LONG  got_detach;
} attachd_client_t;

static DWORD WINAPI attachd_reader(LPVOID arg)
{
    attachd_client_t *c = (attachd_client_t *)arg;
    unsigned char type;
    strbuf_t p;
    strbuf_init(&p);
    for (;;) {
        if (ipc_read_frame(c->pipe, &type, &p) != 0)
            break;
        if (type == MSG_DETACH || type == MSG_EXIT) {
            if (type == MSG_DETACH)
                InterlockedExchange(&c->got_detach, 1);
            break;
        }
    }
    strbuf_free(&p);
    return 0;
}

/* attach -d kicks whichever client is already attached: client A attaches
 * first (so the pipe is busy, matching a real interactive session), then a
 * simulated `attach -d` sends detach-client over the cmd pipe and takes A's
 * place -- verifying both that A actually receives MSG_DETACH and that a new
 * client can then attach where A couldn't a moment ago. */
static int run_selftest_attachd(void)
{
    wchar_t pipename[512], cmdpipename[512];
    HANDLE server = NULL;
    attachd_client_t a;
    HANDLE reader, pipeB = INVALID_HANDLE_VALUE;
    unsigned char sz[4];
    int ok = 1;

    ipc_pipe_name(pipename, 512, L"attachdtest");
    ipc_cmd_pipe_name(cmdpipename, 512, L"attachdtest");

    if (spawn_server_ex(pipename, L"cmd.exe", NULL, 0, 0, NULL, &server) != 0) {
        printf("FAIL: could not spawn server\n");
        return 1;
    }
    Sleep(300);

    memset(&a, 0, sizeof(a));
    a.pipe = ipc_client_connect(pipename, 3000);
    if (a.pipe == INVALID_HANDLE_VALUE) {
        printf("FAIL: client A could not attach\n");
        if (server) { TerminateProcess(server, 1); CloseHandle(server); }
        return 1;
    }
    ipc_pack_size(sz, 80, 25);
    ipc_write_frame(a.pipe, MSG_ATTACH, sz, 4);
    reader = CreateThread(NULL, 0, attachd_reader, &a, 0, NULL);
    Sleep(300);   /* let A settle in as the attached client */

    {
        HANDLE probe;
        int busy = 0;
        probe = ipc_client_connect_ex(pipename, 0, &busy);
        if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
        if (!busy) { printf("FAIL: expected the pipe busy with client A attached\n"); ok = 0; }
    }

    if (send_one_cmd(cmdpipename, "detach-client") != 0) {
        printf("FAIL: detach-client command not acked\n");
        ok = 0;
    }
    pipeB = ipc_client_connect(pipename, 3000);
    if (pipeB == INVALID_HANDLE_VALUE) {
        printf("FAIL: client B could not attach after detach-client\n");
        ok = 0;
    } else {
        ipc_write_frame(pipeB, MSG_ATTACH, sz, 4);
    }

    if (reader) WaitForSingleObject(reader, 2000);
    if (!InterlockedCompareExchange(&a.got_detach, 0, 0)) {
        printf("FAIL: client A never received MSG_DETACH\n");
        ok = 0;
    }

    printf("%s\n", ok ? "ATTACHD SELFTEST PASSED" : "ATTACHD SELFTEST FAILED");

    CloseHandle(a.pipe);
    if (reader) CloseHandle(reader);
    if (pipeB != INVALID_HANDLE_VALUE) {
        ipc_write_frame(pipeB, MSG_KILL, NULL, 0);
        Sleep(150);
        CancelIoEx(pipeB, NULL);
        CloseHandle(pipeB);
    }
    if (server) {
        if (WaitForSingleObject(server, 1000) != WAIT_OBJECT_0)
            TerminateProcess(server, 0);
        CloseHandle(server);
    }
    return ok ? 0 : 1;
}

/* Drive a session directly (no ConPTY output needed): create windows via the
 * prefix key and verify the status bar reflects them. */
static int run_selftest_windows(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);

    /* Ctrl-B c twice -> three windows; the last created is current. */
    session_input(s, "\x02" "c", 2);
    session_input(s, "\x02" "c", 2);
    Sleep(300);
    session_pump(s);

    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "0:cmd") == NULL)  { printf("FAIL: window 0 missing\n"); ok = 0; }
    if (strstr(frame.data, "1:cmd") == NULL)  { printf("FAIL: window 1 missing\n"); ok = 0; }
    if (strstr(frame.data, "2:cmd*") == NULL) { printf("FAIL: window 2 not current\n"); ok = 0; }

    /* Ctrl-B 0 -> switch to window 0. */
    session_input(s, "\x02" "0", 2);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "0:cmd*") == NULL) { printf("FAIL: switch to window 0 failed\n"); ok = 0; }

    printf("%s\n", ok ? "WINDOWS SELFTEST PASSED" : "WINDOWS SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* Enter copy mode through the session and confirm the viewport renders. */
static int run_selftest_copymode(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);

    Sleep(300);
    session_pump(s);

    /* Ctrl-B [ enters copy mode. */
    session_input(s, "\x02" "[", 2);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "[COPY") == NULL) { printf("FAIL: copy indicator missing\n"); ok = 0; }

    /* 'q' exits; the indicator should disappear. */
    session_input(s, "q", 1);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "[COPY") != NULL) { printf("FAIL: copy mode did not exit\n"); ok = 0; }

    printf("%s\n", ok ? "COPYMODE SELFTEST PASSED" : "COPYMODE SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* Exercise the command system: commands, a custom binding, and set prefix. */
static int run_selftest_cmd(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);

    session_run(s, "new-window");             /* window 1 (current) */
    session_run(s, "rename-window MYWIN");    /* rename window 1 */
    session_run(s, "split-window -h");        /* window 1 -> 2 panes */
    session_run(s, "bind z new-window");      /* custom binding */
    session_input(s, "\x02" "z", 2);          /* Ctrl-B z -> window 2 */
    session_run(s, "set prefix C-a");         /* change prefix to Ctrl-A */
    session_input(s, "\x01" "c", 2);          /* Ctrl-A c -> window 3 */

    /* Config-file sourcing: write a small file and source it. */
    {
        char path[MAX_PATH];
        DWORD tn = GetTempPathA(sizeof(path), path);
        if (tn > 0 && tn < sizeof(path) - 20) {
            char cmdline[MAX_PATH + 32];
            FILE *f = NULL;
            strcat_s(path, sizeof(path), "tmuxw_test.conf");
            if (fopen_s(&f, path, "w") == 0 && f) {
                fprintf(f, "# test config\nrename-window FROMCONF\n");
                fclose(f);
                _snprintf_s(cmdline, sizeof(cmdline), _TRUNCATE, "source-file %s", path);
                session_run(s, cmdline);
            }
        }
    }

    Sleep(250);
    session_pump(s);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    if (strstr(frame.data, "1:MYWIN") == NULL)   { printf("FAIL: rename-window not shown\n"); ok = 0; }
    if (strstr(frame.data, "2:cmd") == NULL)     { printf("FAIL: custom binding didn't create window\n"); ok = 0; }
    if (strstr(frame.data, "FROMCONF") == NULL)  { printf("FAIL: source-file config not applied\n"); ok = 0; }

    printf("%s\n", ok ? "CMD SELFTEST PASSED" : "CMD SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* Drive the mouse path: enable mouse (DECSET emitted), a wheel-up SGR report
 * enters copy mode, and disabling mouse emits DECRST. */
static int run_selftest_mouse(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(300);
    session_pump(s);

    session_run(s, "set mouse on");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "\x1b[?1006h") == NULL) { printf("FAIL: mouse enable not emitted\n"); ok = 0; }

    /* Wheel up (button 64) over the pane should enter copy mode. */
    session_input(s, "\x1b[<64;10;10M", 12);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "[COPY") == NULL) { printf("FAIL: wheel-up did not enter copy mode\n"); ok = 0; }

    session_input(s, "q", 1);            /* leave copy mode */
    session_run(s, "set mouse off");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "\x1b[?1006l") == NULL) { printf("FAIL: mouse disable not emitted\n"); ok = 0; }

    printf("%s\n", ok ? "MOUSE SELFTEST PASSED" : "MOUSE SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* break-pane moves the active pane of a two-pane window into a new window. */
static int run_selftest_break(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(250);
    session_pump(s);

    session_run(s, "split-window -h");   /* window 0 now has two panes */
    session_run(s, "break-pane");         /* active pane -> new window 1 */
    Sleep(150);
    session_pump(s);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    if (strstr(frame.data, "0:cmd") == NULL)  { printf("FAIL: original window missing\n"); ok = 0; }
    if (strstr(frame.data, "1:cmd*") == NULL) { printf("FAIL: broken-out window not current\n"); ok = 0; }

    /* Join window 0's pane back into the current window; window 0 disappears. */
    session_run(s, "join-pane -s 0");
    Sleep(150);
    session_pump(s);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "1:cmd") != NULL)  { printf("FAIL: join-pane left a second window\n"); ok = 0; }
    if (strstr(frame.data, "0:cmd*") == NULL) { printf("FAIL: joined window not current\n"); ok = 0; }

    printf("%s\n", ok ? "BREAK SELFTEST PASSED" : "BREAK SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* kill-pane -a closes every pane but the active one (dividers disappear). */
static int run_selftest_killall(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) { printf("FAIL: session_create\n"); if (wake) CloseHandle(wake); return 1; }
    strbuf_init(&frame);
    Sleep(250);
    session_pump(s);

    session_run(s, "split-window -h ; split-window -h");   /* 3 panes */
    session_render(s, &frame); strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "\xe2\x94\x82") == NULL) { printf("FAIL: expected dividers before kill\n"); ok = 0; }

    session_run(s, "kill-pane -a");
    Sleep(150); session_pump(s);
    session_render(s, &frame); strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "\xe2\x94\x82") != NULL) { printf("FAIL: dividers remain after kill-pane -a\n"); ok = 0; }

    printf("%s\n", ok ? "KILLALL SELFTEST PASSED" : "KILLALL SELFTEST FAILED");
    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* move-window relocates the current window to a new position. */
static int run_selftest_move(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) { printf("FAIL: session_create\n"); if (wake) CloseHandle(wake); return 1; }
    strbuf_init(&frame);
    Sleep(200);
    session_pump(s);

    session_run(s, "rename-window W0");
    session_run(s, "new-window ; rename-window W1");
    session_run(s, "new-window ; rename-window W2");   /* current = W2 at index 2 */
    session_run(s, "move-window -t 0");                 /* W2 -> index 0 */
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    if (strstr(frame.data, "0:W2*") == NULL) { printf("FAIL: window not moved to index 0\n"); ok = 0; }
    if (strstr(frame.data, "1:W0") == NULL)  { printf("FAIL: other windows not shifted\n"); ok = 0; }

    printf("%s\n", ok ? "MOVE SELFTEST PASSED" : "MOVE SELFTEST FAILED");
    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* choose-window: navigate the picker with k and select window 0 with Enter. */
static int run_selftest_choose(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(250);
    session_pump(s);

    session_run(s, "new-window");        /* windows 0,1,2 (current 2) */
    session_run(s, "new-window");
    session_run(s, "choose-window");
    session_input(s, "k", 1);            /* sel 2 -> 1 */
    session_input(s, "k", 1);            /* sel 1 -> 0 */
    session_input(s, "\r", 1);           /* select window 0 */
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    if (strstr(frame.data, "0:cmd*") == NULL) { printf("FAIL: choose-window did not select window 0\n"); ok = 0; }

    printf("%s\n", ok ? "CHOOSE SELFTEST PASSED" : "CHOOSE SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* display-message shows text on the status row; run-shell/if-shell run cmd.exe. */
static int run_selftest_shell(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(250);
    session_pump(s);

    session_run(s, "display-message HELLO_MSG");
    session_render(s, &frame); strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "HELLO_MSG") == NULL) { printf("FAIL: display-message not shown\n"); ok = 0; }

    session_run(s, "run-shell \"echo RUNSHELL_OK\"");
    session_render(s, &frame); strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "RUNSHELL_OK") == NULL) { printf("FAIL: run-shell output not shown\n"); ok = 0; }

    session_run(s, "if-shell \"exit 0\" \"display-message IFYES\"");
    session_render(s, &frame); strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "IFYES") == NULL) { printf("FAIL: if-shell then-branch not run\n"); ok = 0; }

    session_run(s, "if-shell \"exit 1\" \"display-message NOPE\" \"display-message IFNO\"");
    session_render(s, &frame); strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "IFNO") == NULL) { printf("FAIL: if-shell else-branch not run\n"); ok = 0; }

    printf("%s\n", ok ? "SHELL SELFTEST PASSED" : "SHELL SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* A -r binding repeats without the prefix during a short window. */
static int run_selftest_repeat(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(250);
    session_pump(s);

    session_run(s, "bind -r a new-window");
    session_input(s, "\x02" "a", 2);   /* Ctrl-B a -> window 1, opens repeat */
    session_input(s, "a", 1);           /* repeat (no prefix) -> window 2 */
    Sleep(150);
    session_pump(s);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    /* Windows 0, 1, 2 exist with 2 current; a fourth (3:cmd) must not. */
    if (strstr(frame.data, "2:cmd*") == NULL) { printf("FAIL: -r repeat did not create window 2\n"); ok = 0; }
    if (strstr(frame.data, "3:cmd") != NULL)  { printf("FAIL: unexpected extra window\n"); ok = 0; }

    printf("%s\n", ok ? "REPEAT SELFTEST PASSED" : "REPEAT SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* clock-mode overlays a big digital clock (lit blocks) over the active pane. */
static int run_selftest_clock(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1, blocks = 0;
    const char *p;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(300);
    session_pump(s);

    session_run(s, "clock-mode");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    /* The clock draws many lit blocks: "\x1b[7m \x1b[0m". */
    for (p = frame.data; (p = strstr(p, "\x1b[7m \x1b[0m")) != NULL; p += 9)
        blocks++;
    if (blocks < 5) { printf("FAIL: clock did not render (blocks=%d)\n", blocks); ok = 0; }

    /* A key dismisses clock mode. */
    session_input(s, " ", 1);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    printf("%s\n", ok ? "CLOCK SELFTEST PASSED" : "CLOCK SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* display-panes overlays each pane's number until a key dismisses it. */
static int run_selftest_display(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(300);
    session_pump(s);

    session_run(s, "split-window -h");   /* panes 0 and 1 */
    session_run(s, "display-panes");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, " 0 ") == NULL) { printf("FAIL: pane 0 number not shown\n"); ok = 0; }
    if (strstr(frame.data, " 1 ") == NULL) { printf("FAIL: pane 1 number not shown\n"); ok = 0; }

    printf("%s\n", ok ? "DISPLAY SELFTEST PASSED" : "DISPLAY SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* send-keys types into the active pane; a ';' separates chained commands. */
static int run_selftest_sendkeys(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(400);
    session_pump(s);

    /* Command sequence: create a window and rename it in one line. */
    session_run(s, "new-window ; rename-window SEQWIN");
    /* Type a command into the new window and run it. */
    session_run(s, "send-keys \"echo SENDKEYS_OK\" Enter");
    Sleep(800);
    session_pump(s);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');

    if (strstr(frame.data, "SEQWIN") == NULL)       { printf("FAIL: ; command sequence did not rename\n"); ok = 0; }
    if (strstr(frame.data, "SENDKEYS_OK") == NULL)  { printf("FAIL: send-keys output missing\n"); ok = 0; }

    printf("%s\n", ok ? "SENDKEYS SELFTEST PASSED" : "SENDKEYS SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* base-index shifts the window numbers shown and used by select-window -t. */
static int run_selftest_options(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(250);
    session_pump(s);

    session_run(s, "set base-index 1");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "1:cmd") == NULL)  { printf("FAIL: base-index label missing\n"); ok = 0; }
    if (strstr(frame.data, "0:cmd") != NULL)  { printf("FAIL: window still labelled 0\n"); ok = 0; }

    session_run(s, "new-window");        /* second window -> displayed as 2 */
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "2:cmd*") == NULL) { printf("FAIL: second window not labelled 2\n"); ok = 0; }

    session_run(s, "select-window -t 1"); /* base-relative -> first window */
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "1:cmd*") == NULL) { printf("FAIL: select-window -t 1 did not pick the first\n"); ok = 0; }

    /* status-left / status-right format expansion (#S, literal text). */
    session_run(s, "rename-session SESSX");
    session_run(s, "set status-left \"<#S>\"");
    session_run(s, "set status-right RIGHTZ");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "<SESSX>") == NULL) { printf("FAIL: status-left #S not expanded\n"); ok = 0; }
    if (strstr(frame.data, "RIGHTZ") == NULL)  { printf("FAIL: status-right text missing\n"); ok = 0; }

    printf("%s\n", ok ? "OPTIONS SELFTEST PASSED" : "OPTIONS SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* link-window inserts a second, live reference to the same window_t (via
 * refcounting) at another index: renaming it via one index must be visible
 * through the other, and unlink-window on one index must leave the other
 * fully alive (not a dangling/freed window). */
static int run_selftest_link(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(250);
    session_pump(s);

    session_run(s, "new-window");            /* windows: 0:cmd, 1:cmd(cur) */
    session_run(s, "link-window -s 0 -t 2");  /* window 0 now ALSO at index 2 */
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "0:cmd") == NULL || strstr(frame.data, "2:cmd") == NULL) {
        printf("FAIL: link-window did not create a second reference\n");
        ok = 0;
    }
    if (strstr(frame.data, "1:cmd*") == NULL) {
        printf("FAIL: link-window shifted the current window's index\n");
        ok = 0;
    }

    /* Rename via index 2; index 0 (the same underlying window) must show it too. */
    session_run(s, "select-window -t 2");
    session_run(s, "rename-window SHARED");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "0:SHARED") == NULL || strstr(frame.data, "2:SHARED*") == NULL) {
        printf("FAIL: rename via the linked index wasn't reflected at the original index\n");
        ok = 0;
    }

    /* Unlink index 2: index 0's window must survive untouched (no crash, no
     * garbage -- proves window_free's refcount didn't over-free). */
    session_run(s, "unlink-window -t 2");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "0:SHARED") == NULL) {
        printf("FAIL: original index lost the window after unlinking the other\n");
        ok = 0;
    }
    if (strstr(frame.data, "2:") != NULL) {
        printf("FAIL: unlinked index still present\n");
        ok = 0;
    }

    /* And the surviving window must still be a live, working pane. */
    session_run(s, "select-window -t 0");
    session_input(s, "echo LINKALIVE\r", (size_t)strlen("echo LINKALIVE\r"));
    Sleep(600);
    session_pump(s);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "LINKALIVE") == NULL) {
        printf("FAIL: surviving linked window's pane is not actually alive\n");
        ok = 0;
    }

    printf("%s\n", ok ? "LINK SELFTEST PASSED" : "LINK SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* switch-client asks the attached client to reconnect to a different
 * session's pipe. This exercises the server-side half of that protocol
 * (session.c's switch_pending state -> server.c sending MSG_SWITCH): attach
 * a raw client to session A, run `switch-client -t B` against A over its cmd
 * pipe, and confirm the attached connection receives MSG_SWITCH naming B
 * instead of further output. (client.c's reconnect-in-place loop that
 * consumes MSG_SWITCH is exercised by every other selftest that attaches at
 * all, just never told to switch -- it's the same run_one_attach() path.) */
static int run_selftest_switch(void)
{
    wchar_t pipenameA[512], cmdpipenameA[512], pipenameB[512];
    HANDLE serverA = NULL, serverB = NULL, pipeA;
    unsigned char sz[4], type;
    strbuf_t payload;
    int ok = 1;

    ipc_pipe_name(pipenameA, 512, L"switchtestA");
    ipc_cmd_pipe_name(cmdpipenameA, 512, L"switchtestA");
    ipc_pipe_name(pipenameB, 512, L"switchtestB");

    if (spawn_server_ex(pipenameA, L"cmd.exe", NULL, 0, 0, NULL, &serverA) != 0 ||
        spawn_server_ex(pipenameB, L"cmd.exe", NULL, 0, 0, NULL, &serverB) != 0) {
        printf("FAIL: could not spawn servers\n");
        if (serverA) { TerminateProcess(serverA, 1); CloseHandle(serverA); }
        if (serverB) { TerminateProcess(serverB, 1); CloseHandle(serverB); }
        return 1;
    }
    Sleep(300);

    pipeA = ipc_client_connect(pipenameA, 3000);
    if (pipeA == INVALID_HANDLE_VALUE) {
        printf("FAIL: could not attach to session A\n");
        ok = 0;
    } else {
        ipc_pack_size(sz, 80, 25);
        ipc_write_frame(pipeA, MSG_ATTACH, sz, 4);

        if (send_one_cmd(cmdpipenameA, "switch-client -t switchtestB") != 0) {
            printf("FAIL: switch-client command not acked\n");
            ok = 0;
        }

        strbuf_init(&payload);
        ok = ok && 1;
        {
            int got_switch = 0;
            DWORD deadline = GetTickCount() + 3000;
            while (GetTickCount() < deadline) {
                if (ipc_read_frame(pipeA, &type, &payload) != 0)
                    break;
                if (type == MSG_SWITCH) {
                    got_switch = 1;
                    if (payload.len == 0 || strncmp(payload.data, "switchtestB", payload.len) != 0) {
                        printf("FAIL: MSG_SWITCH target mismatch\n");
                        ok = 0;
                    }
                    break;
                }
                /* MSG_OUTPUT frames from the shell starting up: keep reading. */
            }
            if (!got_switch) { printf("FAIL: never received MSG_SWITCH\n"); ok = 0; }
        }
        strbuf_free(&payload);
        CancelIoEx(pipeA, NULL);
        CloseHandle(pipeA);
    }

    printf("%s\n", ok ? "SWITCH SELFTEST PASSED" : "SWITCH SELFTEST FAILED");

    kill_one(L"switchtestA");
    kill_one(L"switchtestB");
    if (serverA) {
        if (WaitForSingleObject(serverA, 1000) != WAIT_OBJECT_0)
            TerminateProcess(serverA, 0);
        CloseHandle(serverA);
    }
    if (serverB) {
        if (WaitForSingleObject(serverB, 1000) != WAIT_OBJECT_0)
            TerminateProcess(serverB, 0);
        CloseHandle(serverB);
    }
    return ok ? 0 : 1;
}

/* confirm-before shows a message and gates a command behind y/n: 'n' (or
 * anything but y/Y) cancels, y/Y runs it. Also exercises capture-pane,
 * clear-history, and previous-layout, which don't have a dedicated selftest
 * elsewhere. */
static int run_selftest_confirm(void)
{
    HANDLE wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    session_t *s = session_create(L"cmd.exe", 80, 25, wake);
    strbuf_t frame;
    int ok = 1;

    if (s == NULL) {
        printf("FAIL: session_create\n");
        if (wake) CloseHandle(wake);
        return 1;
    }
    strbuf_init(&frame);
    Sleep(300);
    session_pump(s);

    session_run(s, "confirm-before -p \"rename? (y/n)\" \"rename-window NOPE\"");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "rename?") == NULL) { printf("FAIL: confirm-before message not shown\n"); ok = 0; }

    session_input(s, "n", 1);        /* cancel */
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "0:NOPE") != NULL) { printf("FAIL: 'n' should have cancelled\n"); ok = 0; }

    session_run(s, "confirm-before -p \"rename? (y/n)\" \"rename-window CONFIRMED\"");
    session_input(s, "y", 1);        /* confirm */
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "0:CONFIRMED") == NULL) { printf("FAIL: 'y' should have run the command\n"); ok = 0; }

    /* capture-pane / clear-history / previous-layout smoke: none of these
     * should crash. */
    session_run(s, "capture-pane");
    session_run(s, "clear-history");
    session_run(s, "split-window -h");
    session_run(s, "previous-layout");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "\xe2\x94\x82") == NULL) { printf("FAIL: previous-layout broke the split\n"); ok = 0; }

    /* choose-buffer: opens a picker over the paste-buffer stack; Enter pastes
     * the highlighted one into the active pane. */
    session_run(s, "set-buffer -b choosetest CHOOSEBUF_PAYLOAD");
    session_run(s, "choose-buffer");
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "choosetest") == NULL) { printf("FAIL: choose-buffer picker not shown\n"); ok = 0; }

    session_input(s, "\r", 1);       /* select it: pastes into the active pane */
    Sleep(400);
    session_pump(s);
    session_render(s, &frame);
    strbuf_putc(&frame, '\0');
    if (strstr(frame.data, "CHOOSEBUF_PAYLOAD") == NULL) {
        printf("FAIL: choose-buffer Enter did not paste the selected buffer\n");
        ok = 0;
    }

    printf("%s\n", ok ? "CONFIRM SELFTEST PASSED" : "CONFIRM SELFTEST FAILED");

    strbuf_free(&frame);
    session_free(s);
    if (wake) CloseHandle(wake);
    return ok ? 0 : 1;
}

/* ----- dispatch ------------------------------------------------------------- */

int wmain(int argc, wchar_t **argv)
{
    wchar_t shell[CMD_MAX];

    disinherit_std_handles();

    if (argc > 1 && wcscmp(argv[1], L"--selftest-render") == 0)
        return run_selftest_render(argc, argv);
    if (argc > 1 && wcscmp(argv[1], L"--selftest-split") == 0)
        return run_selftest_split();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-ipc") == 0)
        return run_selftest_ipc();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-cmdipc") == 0)
        return run_selftest_cmdipc();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-attachd") == 0)
        return run_selftest_attachd();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-switch") == 0)
        return run_selftest_switch();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-link") == 0)
        return run_selftest_link();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-windows") == 0)
        return run_selftest_windows();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-copymode") == 0)
        return run_selftest_copymode();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-cmd") == 0)
        return run_selftest_cmd();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-mouse") == 0)
        return run_selftest_mouse();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-break") == 0)
        return run_selftest_break();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-options") == 0)
        return run_selftest_options();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-confirm") == 0)
        return run_selftest_confirm();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-sendkeys") == 0)
        return run_selftest_sendkeys();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-display") == 0)
        return run_selftest_display();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-clock") == 0)
        return run_selftest_clock();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-repeat") == 0)
        return run_selftest_repeat();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-shell") == 0)
        return run_selftest_shell();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-choose") == 0)
        return run_selftest_choose();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-move") == 0)
        return run_selftest_move();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-killall") == 0)
        return run_selftest_killall();
    if (argc > 1 && wcscmp(argv[1], L"--selftest") == 0)
        return run_selftest(argc, argv);

    if (argc > 1 && wcscmp(argv[1], L"--server") == 0) {
        const wchar_t *pipename = (argc > 2) ? argv[2] : L"";
        const wchar_t *cwd = NULL;
        char cfgfile[MAX_PATH];
        int scols = 0, srows = 0, idx = 3;

        cfgfile[0] = '\0';
        while (idx < argc) {
            if (wcscmp(argv[idx], L"--cwd") == 0 && idx + 1 < argc) {
                cwd = argv[idx + 1];
                idx += 2;
            } else if (wcscmp(argv[idx], L"--size") == 0 && idx + 1 < argc) {
                swscanf_s(argv[idx + 1], L"%dx%d", &scols, &srows);
                idx += 2;
            } else if (wcscmp(argv[idx], L"--cfg") == 0 && idx + 1 < argc) {
                WideCharToMultiByte(CP_UTF8, 0, argv[idx + 1], -1, cfgfile, sizeof(cfgfile), NULL, NULL);
                idx += 2;
            } else {
                break;
            }
        }
        if (idx < argc) join_cmdline(shell, argc, argv, idx);
        else            wcscpy_s(shell, CMD_MAX, default_shell());
        return run_server(pipename, shell, cwd, scols, srows, cfgfile[0] ? cfgfile : NULL);
    }

    if (argc > 1 && wcscmp(argv[1], L"--standalone") == 0) {
        if (argc > 2) join_cmdline(shell, argc, argv, 2);
        else          wcscpy_s(shell, CMD_MAX, default_shell());
        return run_standalone(shell, NULL);
    }

    if (argc > 1 && (wcscmp(argv[1], L"-V") == 0 || wcscmp(argv[1], L"--version") == 0)) {
        printf("tmuxw " TMUXW_VERSION "\n");
        return 0;
    }

    /* No arguments: attach to (or start) the default session. */
    if (argc == 1)
        return attach_session(L"default", default_shell(), 1, NULL, 0, 0, 0, 0, NULL);

    /* Global flags recognized as a prefix before the subcommand, in any
     * order/combination, e.g. `tmux -L work -f myconf.conf new -s work`:
     *   -f <file>  an alternate config file, used when this invocation
     *              starts a new server (tmux's -f)
     *   -L <name> / -S <path>
     *              an independent namespace of sessions (tmux's -L/-S;
     *              see g_namespace/ns_session_name for how Windows named
     *              pipes stand in for tmux's separate server sockets)
     *   -2 / -8 / -u / -q
     *              accepted for script/shebang compatibility and otherwise
     *              no-ops here: color depth (-2/-8) and encoding (-u) are
     *              whatever the terminal's own VT support is, since tmuxw
     *              passes escape sequences straight through rather than
     *              reinterpreting them; -q (quiet startup messages) has no
     *              startup messages to suppress in the first place */
    {
        int subidx = 1;
        const wchar_t *cfgfile = NULL;

        while (subidx < argc) {
            if (wcscmp(argv[subidx], L"-f") == 0 && subidx + 1 < argc) {
                cfgfile = argv[subidx + 1];
                subidx += 2;
            } else if ((wcscmp(argv[subidx], L"-L") == 0 || wcscmp(argv[subidx], L"-S") == 0) &&
                      subidx + 1 < argc) {
                wcscpy_s(g_namespace, 256, argv[subidx + 1]);
                subidx += 2;
            } else if (wcscmp(argv[subidx], L"-2") == 0 || wcscmp(argv[subidx], L"-8") == 0 ||
                      wcscmp(argv[subidx], L"-u") == 0 || wcscmp(argv[subidx], L"-q") == 0) {
                subidx += 1;
            } else {
                break;
            }
        }

        /* A tmux-style client subcommand: new / new-session / attach / a. */
        if (argv[subidx][0] != L'-') {
            const wchar_t *sub = argv[subidx];
            target_args_t a;

            parse_target_args(argc, argv, subidx + 1, &a);

            if (wcscmp(sub, L"new") == 0 || wcscmp(sub, L"new-session") == 0)
                return attach_session(a.name, a.has_shell ? a.shell : default_shell(), 1,
                                      a.cwd[0] ? a.cwd : NULL, a.cols, a.rows, a.detach, 0,
                                      cfgfile);

            if (wcscmp(sub, L"attach") == 0 || wcscmp(sub, L"attach-session") == 0 ||
                wcscmp(sub, L"a") == 0)
                return attach_session(a.name, default_shell(), 0, NULL, 0, 0, 0, a.detach, NULL);

        if (wcscmp(sub, L"ls") == 0 || wcscmp(sub, L"list-sessions") == 0)
            return list_sessions();

        if (wcscmp(sub, L"has-session") == 0 || wcscmp(sub, L"has") == 0)
            return has_session_cmd(a.name);

        if (wcscmp(sub, L"kill-session") == 0)
            return a.all ? kill_session_others_cmd(a.name) : kill_session_cmd(a.name);

        if (wcscmp(sub, L"kill-server") == 0)
            return kill_server_cmd();

        /* wait-for [-t session] [-S|-L|-U] <channel>: -S/-L/-U signal (see
         * cmd_wait_for for why tmux's lock semantics aren't distinguished
         * here) and are otherwise a normal fire-and-forget forward. A bare
         * wait blocks until signalled -- but the *server* never blocks (see
         * the wait-for section in session.c), so the CLI polls here instead,
         * matching tmux's actual blocking behavior from the caller's point
         * of view without freezing the session for anyone attached to it. */
        if (wcscmp(sub, L"wait-for") == 0) {
            wchar_t name[256] = L"", cmdpipename[512], nsname[512];
            char channel[64] = "", cmdline[128], reply[64];
            int i, is_signal = 0, start = subidx + 1;

            if (argc > subidx + 2 && wcscmp(argv[subidx + 1], L"-t") == 0) {
                wcscpy_s(name, 256, argv[subidx + 2]);
                start = subidx + 3;
            }
            for (i = start; i < argc; i++) {
                if (wcscmp(argv[i], L"-S") == 0 || wcscmp(argv[i], L"-L") == 0 ||
                    wcscmp(argv[i], L"-U") == 0)
                    is_signal = 1;
                else
                    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, channel, sizeof(channel), NULL, NULL);
            }

            ns_session_name(name, nsname, 512);
            ipc_cmd_pipe_name(cmdpipename, 512, nsname);

            if (is_signal) {
                _snprintf_s(cmdline, sizeof(cmdline), _TRUNCATE, "wait-for -S %s", channel);
                if (send_one_cmd(cmdpipename, cmdline) != 0) {
                    fprintf(stderr, "tmux: no server running for session '%ls'\n",
                            name[0] ? name : L"default");
                    return 1;
                }
                return 0;
            }
            _snprintf_s(cmdline, sizeof(cmdline), _TRUNCATE, "wait-for %s", channel);
            for (;;) {
                if (send_cmd_capture(cmdpipename, cmdline, reply, sizeof(reply)) != 0) {
                    fprintf(stderr, "tmux: no server running for session '%ls'\n",
                            name[0] ? name : L"default");
                    return 1;
                }
                if (strncmp(reply, "OK", 2) == 0)
                    return 0;
                Sleep(200);
            }
        }

        /* Anything else is forwarded verbatim to a session's server as an
         * in-session command (send-keys, rename-session, new-window, ...),
         * whether or not a client is attached to it -- mirroring how real
         * tmux runs one-off commands from a plain shell, e.g.
         * `tmux send-keys -t work 'make' Enter`. A `-t <session>` pair
         * immediately after the subcommand name picks the session (default
         * otherwise); it is intentionally recognized *only* in that
         * position, since several in-session commands (select-window -t,
         * select-pane -t, swap-window -t, ...) use -t for their own
         * window/pane-local targeting and must receive it unchanged. */
        {
            wchar_t name[256] = L"";
            wchar_t cmdpipename[512], nsname[512];
            char cmdline[CMD_MAX];
            char subn[64];
            int start = subidx + 1, i;

            if (argc > subidx + 2 && wcscmp(argv[subidx + 1], L"-t") == 0) {
                wcscpy_s(name, 256, argv[subidx + 2]);
                start = subidx + 3;
            }

            WideCharToMultiByte(CP_UTF8, 0, sub, -1, subn, sizeof(subn), NULL, NULL);
            strcpy_s(cmdline, CMD_MAX, subn);
            for (i = start; i < argc; i++) {
                char narrow[1024];
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, narrow, sizeof(narrow), NULL, NULL);
                strcat_s(cmdline, CMD_MAX, " ");
                if (strpbrk(narrow, " \t") != NULL && narrow[0] != '"') {
                    strcat_s(cmdline, CMD_MAX, "\"");
                    strcat_s(cmdline, CMD_MAX, narrow);
                    strcat_s(cmdline, CMD_MAX, "\"");
                } else {
                    strcat_s(cmdline, CMD_MAX, narrow);
                }
            }

            ns_session_name(name, nsname, 512);
            ipc_cmd_pipe_name(cmdpipename, 512, nsname);
            if (send_one_cmd(cmdpipename, cmdline) != 0) {
                fprintf(stderr, "tmux: no server running for session '%ls'\n",
                        name[0] ? name : L"default");
                return 1;
            }
            return 0;
        }
        }

        fprintf(stderr, "tmux: unknown option: %ls\n", argv[subidx]);
        return 1;
    }
}

/* main.c — tmuxw entry point and command dispatch (Phase 3).
 *
 * Running `tmux`/`tmuxw` connects to a background server for a session, starting
 * one if needed, and attaches as a thin client. The server owns all panes and
 * survives detach (Ctrl-B d); reattaching resumes the session.
 *
 *   tmux                         attach to (or start) the default session
 *   tmux new [-s name] [cmd]     start a session (alias: new-session) and attach
 *   tmux attach [-t name]        attach to an existing session (alias: a)
 *   tmux --standalone [cmd]      run a session in one process (no server; debug)
 *   tmux --server <pipe> [cmd]   internal: the background server process
 *   tmux --selftest [cmd]        headless: ConPTY output straight to stdout
 *   tmux --selftest-render [cmd] headless: ConPTY -> libvterm -> dump grid
 *   tmux --selftest-split        headless: two panes + compositor
 *   tmux --selftest-ipc          headless: full server/client round trip
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

/* Launch the background server process for `pipename`. On success, if
 * out_process is non-NULL, receives the server process handle (caller closes);
 * otherwise the handle is closed here. Returns 0 on success. */
static int spawn_server_ex(const wchar_t *pipename, const wchar_t *shell, HANDLE *out_process)
{
    wchar_t exe[MAX_PATH];
    wchar_t cmd[CMD_MAX];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0)
        return (int)GetLastError();

    _snwprintf_s(cmd, CMD_MAX, _TRUNCATE, L"\"%s\" --server %s %s", exe, pipename, shell);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi))
        return (int)GetLastError();

    CloseHandle(pi.hThread);
    if (out_process)
        *out_process = pi.hProcess;
    else
        CloseHandle(pi.hProcess);
    return 0;
}

static int spawn_server(const wchar_t *pipename, const wchar_t *shell)
{
    return spawn_server_ex(pipename, shell, NULL);
}

/* Attach to session `name` (NULL/empty -> "default") as a thin client. If no
 * server is running: when start_if_missing is set, spawn one running `shell`;
 * otherwise fail (the `attach` command must not create sessions). */
static int attach_session(const wchar_t *name, const wchar_t *shell, int start_if_missing)
{
    wchar_t pipename[512];
    HANDLE pipe;

    ipc_pipe_name(pipename, 512, (name && name[0]) ? name : L"default");

    pipe = ipc_client_connect(pipename, 0);
    if (pipe == INVALID_HANDLE_VALUE) {
        int rc;
        if (!start_if_missing) {
            fprintf(stderr, "tmux: no server running for session '%ls'\n",
                    (name && name[0]) ? name : L"default");
            return 1;
        }
        rc = spawn_server(pipename, shell);
        if (rc != 0) {
            fprintf(stderr, "tmux: failed to start server (error %d)\n", rc);
            return 1;
        }
        pipe = ipc_client_connect(pipename, 5000);
        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "tmux: could not connect to server\n");
            return 1;
        }
    }
    return run_client(pipe);
}

/* Parse client-subcommand arguments starting at argv[start]: a `-s`/`-t name`
 * target and any remaining tokens joined as the command to run. `name` and
 * `shell` are always NUL-terminated; *has_shell reports whether a command was
 * given. */
static void parse_target_and_shell(int argc, wchar_t **argv, int start,
                                   wchar_t *name, size_t name_cap,
                                   wchar_t *shell, size_t shell_cap,
                                   int *has_shell)
{
    int i;
    name[0] = L'\0';
    shell[0] = L'\0';
    *has_shell = 0;
    for (i = start; i < argc; i++) {
        if ((wcscmp(argv[i], L"-s") == 0 || wcscmp(argv[i], L"-t") == 0) && i + 1 < argc) {
            wcscpy_s(name, name_cap, argv[++i]);
        } else if (argv[i][0] == L'-' && argv[i][1] != L'\0') {
            /* Unrecognized flag: ignore for now (kept forward-compatible). */
        } else {
            if (*has_shell) wcscat_s(shell, shell_cap, L" ");
            wcscat_s(shell, shell_cap, argv[i]);
            *has_shell = 1;
        }
    }
}

/* ----- session listing / killing (client side) ------------------------------ */

#define SESS_MAX   64
#define SESS_NAME  64

static int list_sessions(void)
{
    char names[SESS_MAX * SESS_NAME];
    int n = ipc_list_sessions(names, SESS_MAX, SESS_NAME), i;
    if (n == 0) {
        printf("no sessions\n");
        return 0;
    }
    for (i = 0; i < n; i++)
        printf("%s\n", names + (size_t)i * SESS_NAME);
    return 0;
}

/* Connect to a session's server and ask it to exit. Returns 0 if the request
 * was delivered, 1 if no such session is running. */
static int kill_one(const wchar_t *name)
{
    wchar_t pipename[512];
    HANDLE pipe;
    ipc_pipe_name(pipename, 512, (name && name[0]) ? name : L"default");
    pipe = ipc_client_connect(pipename, 0);
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;
    ipc_write_frame(pipe, MSG_KILL, NULL, 0);
    Sleep(100);                 /* let the server act before we drop the pipe */
    CloseHandle(pipe);
    return 0;
}

static int kill_session_cmd(const wchar_t *name)
{
    if (kill_one(name) != 0) {
        fprintf(stderr, "tmux: no such session\n");
        return 1;
    }
    return 0;
}

static int kill_server_cmd(void)
{
    char names[SESS_MAX * SESS_NAME];
    int n = ipc_list_sessions(names, SESS_MAX, SESS_NAME), i, killed = 0;
    for (i = 0; i < n; i++) {
        wchar_t w[SESS_NAME * 2];
        MultiByteToWideChar(CP_UTF8, 0, names + (size_t)i * SESS_NAME, -1, w, SESS_NAME * 2);
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
    int rc = conpty_spawn(&pty, cmdline, 80, 25);
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
    pane_t *a = pane_create(1, L"cmd.exe /c echo PANE_A_TEXT", 40, 6, wake);
    pane_t *b = pane_create(2, L"cmd.exe /c echo PANE_B_TEXT", 40, 6, wake);
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

    if (spawn_server_ex(pipename, L"cmd.exe", &server) != 0) {
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

    /* Unblock the reader (parked in ReadFile), then shut the server down. */
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

    printf("%s\n", ok ? "BREAK SELFTEST PASSED" : "BREAK SELFTEST FAILED");

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

    printf("%s\n", ok ? "OPTIONS SELFTEST PASSED" : "OPTIONS SELFTEST FAILED");

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
    if (argc > 1 && wcscmp(argv[1], L"--selftest-sendkeys") == 0)
        return run_selftest_sendkeys();
    if (argc > 1 && wcscmp(argv[1], L"--selftest-display") == 0)
        return run_selftest_display();
    if (argc > 1 && wcscmp(argv[1], L"--selftest") == 0)
        return run_selftest(argc, argv);

    if (argc > 1 && wcscmp(argv[1], L"--server") == 0) {
        const wchar_t *pipename = (argc > 2) ? argv[2] : L"";
        if (argc > 3) join_cmdline(shell, argc, argv, 3);
        else          wcscpy_s(shell, CMD_MAX, default_shell());
        return run_server(pipename, shell);
    }

    if (argc > 1 && wcscmp(argv[1], L"--standalone") == 0) {
        if (argc > 2) join_cmdline(shell, argc, argv, 2);
        else          wcscpy_s(shell, CMD_MAX, default_shell());
        return run_standalone(shell);
    }

    /* No arguments: attach to (or start) the default session. */
    if (argc == 1)
        return attach_session(L"default", default_shell(), 1);

    /* A tmux-style client subcommand: new / new-session / attach / a. */
    if (argv[1][0] != L'-') {
        const wchar_t *sub = argv[1];
        wchar_t name[256];
        int has_shell;

        parse_target_and_shell(argc, argv, 2, name, 256, shell, CMD_MAX, &has_shell);

        if (wcscmp(sub, L"new") == 0 || wcscmp(sub, L"new-session") == 0)
            return attach_session(name, has_shell ? shell : default_shell(), 1);

        if (wcscmp(sub, L"attach") == 0 || wcscmp(sub, L"attach-session") == 0 ||
            wcscmp(sub, L"a") == 0)
            return attach_session(name, default_shell(), 0);

        if (wcscmp(sub, L"ls") == 0 || wcscmp(sub, L"list-sessions") == 0)
            return list_sessions();

        if (wcscmp(sub, L"kill-session") == 0)
            return kill_session_cmd(name);

        if (wcscmp(sub, L"kill-server") == 0)
            return kill_server_cmd();

        fprintf(stderr, "tmux: unknown command: %ls\n", sub);
        fprintf(stderr,
                "usage: tmux [new|new-session|attach|attach-session|a|ls|"
                "kill-session|kill-server] [-s|-t name] [command]\n");
        return 1;
    }

    fprintf(stderr, "tmux: unknown option: %ls\n", argv[1]);
    return 1;
}

/* pane.c — see pane.h. */
#include "model/pane.h"

#include <stdio.h>
#include <stdlib.h>

#define PANE_IO_BUF 8192

/* Length in wchar_t (including the final terminating NUL) of a
 * GetEnvironmentStringsW-style block: a run of NUL-terminated "NAME=VALUE"
 * strings, itself terminated by an extra NUL. */
static size_t env_block_wlen(const wchar_t *b)
{
    const wchar_t *p = b;
    while (*p) p += wcslen(p) + 1;
    return (size_t)(p - b) + 1;
}

static wchar_t *env_block_dup(const wchar_t *b)
{
    wchar_t *copy;
    size_t n;
    if (b == NULL)
        return NULL;
    n = env_block_wlen(b);
    copy = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (copy)
        memcpy(copy, b, n * sizeof(wchar_t));
    return copy;
}

/* Drain the pseudo console into the pending buffer until it is closed. Reading
 * blocks until ClosePseudoConsole releases conhost's write end (see conpty.c),
 * so this must live on its own thread; pane_close() triggers the EOF. */
static DWORD WINAPI pane_reader(LPVOID arg)
{
    pane_t *p = (pane_t *)arg;
    char buf[PANE_IO_BUF];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(p->pty.output_read, buf, sizeof(buf), &n, NULL) || n == 0)
            break;
        EnterCriticalSection(&p->lock);
        strbuf_append(&p->pending, buf, n);
        if (p->pipe_write) {
            DWORD written = 0;
            if (!WriteFile(p->pipe_write, buf, n, &written, NULL)) {
                /* The piped process went away; stop targeting it. */
                CloseHandle(p->pipe_write);
                p->pipe_write = NULL;
                if (p->pipe_proc) { CloseHandle(p->pipe_proc); p->pipe_proc = NULL; }
            }
        }
        LeaveCriticalSection(&p->lock);
        if (p->wake)
            SetEvent(p->wake);
    }
    return 0;
}

pane_t *pane_create(int id, const wchar_t *cmdline, int cols, int rows, HANDLE wake,
                     const wchar_t *cwd, const wchar_t *envblock)
{
    pane_t *p;
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 25;

    p = (pane_t *)calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;

    p->id = id;
    p->cols = cols;
    p->rows = rows;
    p->wake = wake;
    InitializeCriticalSection(&p->lock);
    strbuf_init(&p->pending);
    wcsncpy_s(p->cmdline, 1024, cmdline, _TRUNCATE);
    if (cwd) wcsncpy_s(p->cwd, MAX_PATH, cwd, _TRUNCATE);
    else     p->cwd[0] = L'\0';
    p->envblock = env_block_dup(envblock);

    p->screen = screen_new(cols, rows);
    if (p->screen == NULL)
        goto fail;

    if (conpty_spawn(&p->pty, cmdline, (short)cols, (short)rows, cwd, p->envblock) != 0)
        goto fail;
    p->has_conpty = 1;

    p->reader = CreateThread(NULL, 0, pane_reader, p, 0, NULL);
    if (p->reader == NULL)
        goto fail;

    return p;

fail:
    pane_close(p);
    return NULL;
}

void pane_close(pane_t *p)
{
    if (p == NULL)
        return;
    pane_pipe_stop(p);
    /* Closing the pseudo console makes the reader's ReadFile hit EOF. */
    if (p->has_conpty)
        conpty_close(&p->pty);
    if (p->reader) {
        WaitForSingleObject(p->reader, 1000);
        CloseHandle(p->reader);
    }
    if (p->screen)
        screen_free(p->screen);
    strbuf_free(&p->pending);
    free(p->envblock);
    DeleteCriticalSection(&p->lock);
    free(p);
}

int pane_respawn(pane_t *p)
{
    if (p->has_conpty) {
        conpty_close(&p->pty);
        p->has_conpty = 0;
    }
    if (p->reader) {
        WaitForSingleObject(p->reader, 1000);
        CloseHandle(p->reader);
        p->reader = NULL;
    }
    EnterCriticalSection(&p->lock);
    strbuf_clear(&p->pending);
    LeaveCriticalSection(&p->lock);
    if (p->screen) {
        screen_clear_history(p->screen);
        screen_mark_all_dirty(p->screen);
    }

    ZeroMemory(&p->pty, sizeof(p->pty));
    if (conpty_spawn(&p->pty, p->cmdline, (short)p->cols, (short)p->rows,
                     p->cwd[0] ? p->cwd : NULL, p->envblock) != 0)
        return -1;
    p->has_conpty = 1;

    p->reader = CreateThread(NULL, 0, pane_reader, p, 0, NULL);
    if (p->reader == NULL) {
        conpty_close(&p->pty);
        p->has_conpty = 0;
        return -1;
    }
    return 0;
}

int pane_pipe_start(pane_t *p, const char *shell_cmdline)
{
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char full[1024];

    pane_pipe_stop(p);   /* replace any existing target */

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);   /* we keep the write end */

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    ZeroMemory(&pi, sizeof(pi));

    _snprintf_s(full, sizeof(full), _TRUNCATE, "cmd.exe /c %s", shell_cmdline);
    if (!CreateProcessA(NULL, full, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(rd);
    CloseHandle(pi.hThread);

    EnterCriticalSection(&p->lock);
    p->pipe_write = wr;
    p->pipe_proc = pi.hProcess;
    LeaveCriticalSection(&p->lock);
    return 0;
}

void pane_pipe_stop(pane_t *p)
{
    HANDLE w, proc;
    EnterCriticalSection(&p->lock);
    w = p->pipe_write;
    proc = p->pipe_proc;
    p->pipe_write = NULL;
    p->pipe_proc = NULL;
    LeaveCriticalSection(&p->lock);
    if (w)
        CloseHandle(w);        /* child sees EOF on its stdin */
    if (proc) {
        WaitForSingleObject(proc, 200);
        CloseHandle(proc);
    }
}

int pane_pipe_active(const pane_t *p)
{
    return p->pipe_write != NULL;
}

void pane_apply_geometry(pane_t *p)
{
    if (p->cols < 1) p->cols = 1;
    if (p->rows < 1) p->rows = 1;
    if (p->screen)
        screen_resize(p->screen, p->cols, p->rows);
    if (p->has_conpty)
        conpty_resize(&p->pty, (short)p->cols, (short)p->rows);
}

size_t pane_pump(pane_t *p)
{
    strbuf_t chunk;
    size_t got;

    EnterCriticalSection(&p->lock);
    if (p->pending.len == 0) {
        LeaveCriticalSection(&p->lock);
        return 0;
    }
    /* Hand the buffer off and parse outside the lock so the reader isn't
     * blocked while libvterm works. */
    chunk = p->pending;
    strbuf_init(&p->pending);
    LeaveCriticalSection(&p->lock);

    got = chunk.len;
    if (p->screen)
        screen_write(p->screen, chunk.data, chunk.len);
    strbuf_free(&chunk);
    return got;
}

void pane_write_input(pane_t *p, const char *bytes, size_t n)
{
    DWORD written = 0;
    if (p->has_conpty && n > 0)
        WriteFile(p->pty.input_write, bytes, (DWORD)n, &written, NULL);
}

int pane_child_exited(pane_t *p)
{
    if (!p->has_conpty)
        return 0;
    return WaitForSingleObject(p->pty.process, 0) == WAIT_OBJECT_0;
}

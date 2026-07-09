/* pane.c — see pane.h. */
#include "model/pane.h"

#include <stdlib.h>

#define PANE_IO_BUF 8192

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
        LeaveCriticalSection(&p->lock);
        if (p->wake)
            SetEvent(p->wake);
    }
    return 0;
}

pane_t *pane_create(int id, const wchar_t *cmdline, int cols, int rows, HANDLE wake,
                     const wchar_t *cwd)
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

    p->screen = screen_new(cols, rows);
    if (p->screen == NULL)
        goto fail;

    if (conpty_spawn(&p->pty, cmdline, (short)cols, (short)rows, cwd) != 0)
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
    DeleteCriticalSection(&p->lock);
    free(p);
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

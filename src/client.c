/* client.c — see client.h. */
#include "client.h"

#include "platform/ipc.h"
#include "platform/winterm.h"
#include "util/strbuf.h"

#include <stdio.h>

#define CLI_IO_BUF 8192

typedef struct client {
    HANDLE        pipe;
    winterm_t    *term;
    volatile LONG quit;
} client_t;

/* Reader thread: server output frames -> host terminal. */
static DWORD WINAPI client_reader(LPVOID arg)
{
    client_t *c = (client_t *)arg;
    unsigned char type;
    strbuf_t payload;
    strbuf_init(&payload);

    for (;;) {
        if (ipc_read_frame(c->pipe, &type, &payload) != 0)
            break;
        if (type == MSG_OUTPUT) {
            DWORD w = 0;
            WriteFile(c->term->out, payload.data, (DWORD)payload.len, &w, NULL);
        } else if (type == MSG_DETACH || type == MSG_EXIT) {
            break;
        }
    }

    strbuf_free(&payload);
    InterlockedExchange(&c->quit, 1);
    return 0;
}

/* Input thread: host keystrokes -> server. */
static DWORD WINAPI client_input(LPVOID arg)
{
    client_t *c = (client_t *)arg;
    char buf[CLI_IO_BUF];
    DWORD n = 0;
    for (;;) {
        if (InterlockedCompareExchange(&c->quit, 0, 0))
            break;
        if (!ReadFile(c->term->in, buf, sizeof(buf), &n, NULL) || n == 0)
            break;
        if (ipc_write_frame(c->pipe, MSG_INPUT, buf, n) != 0)
            break;
    }
    return 0;
}

int run_client(HANDLE pipe)
{
    winterm_t term;
    client_t c;
    HANDLE h_reader, h_input;
    unsigned char sz[4];
    short cols = 80, rows = 25, pc, pr;

    if (winterm_enable(&term) != 0) {
        fprintf(stderr, "tmuxw: failed to set up console\n");
        CloseHandle(pipe);
        return 1;
    }
    winterm_size(&term, &cols, &rows);
    pc = cols; pr = rows;

    c.pipe = pipe;
    c.term = &term;
    c.quit = 0;

    /* Announce our size, then start relaying. */
    ipc_pack_size(sz, cols, rows);
    if (ipc_write_frame(pipe, MSG_ATTACH, sz, 4) != 0) {
        winterm_restore(&term);
        CloseHandle(pipe);
        return 1;
    }

    h_reader = CreateThread(NULL, 0, client_reader, &c, 0, NULL);
    h_input = CreateThread(NULL, 0, client_input, &c, 0, NULL);

    /* Main loop: watch for resize and for the reader signalling quit. */
    while (!InterlockedCompareExchange(&c.quit, 0, 0)) {
        short cc, rr;
        Sleep(50);
        winterm_size(&term, &cc, &rr);
        if (cc != pc || rr != pr) {
            pc = cc; pr = rr;
            ipc_pack_size(sz, cc, rr);
            if (ipc_write_frame(pipe, MSG_RESIZE, sz, 4) != 0)
                break;
        }
    }

    /* Tear down: closing the pipe unblocks the reader; cancel the input read. */
    CloseHandle(pipe);
    CancelIoEx(term.in, NULL);
    if (h_reader) { WaitForSingleObject(h_reader, 1000); CloseHandle(h_reader); }
    if (h_input)  { WaitForSingleObject(h_input, 1000);  CloseHandle(h_input); }

    /* Restore the terminal for the shell we return to. */
    {
        strbuf_t frame;
        strbuf_init(&frame);
        /* Disable mouse reporting, then reset attributes and clear. */
        strbuf_append(&frame, "\x1b[?1000l\x1b[?1002l\x1b[?1006l", 24);
        strbuf_append(&frame, "\x1b[0m\x1b[2J\x1b[H", 10);
        WriteFile(term.out, frame.data, (DWORD)frame.len, NULL, NULL);
        strbuf_free(&frame);
    }
    winterm_restore(&term);
    return 0;
}

/* server.c — see server.h. */
#include "server.h"

#include "session.h"
#include "render.h"
#include "platform/ipc.h"
#include "platform/winterm.h"
#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRV_IO_BUF 8192

/* Optional debug trace to the file named by %TMUXW_SRVLOG% (server has no
 * console). No-op when the variable is unset. */
static void srvlog(const char *msg)
{
    char path[512];
    DWORD n = GetEnvironmentVariableA("TMUXW_SRVLOG", path, sizeof(path));
    if (n == 0 || n >= sizeof(path))
        return;
    {
        FILE *f = NULL;
        if (fopen_s(&f, path, "a") == 0 && f) {
            fprintf(f, "%s\n", msg);
            fclose(f);
        }
    }
}

/* ----- one-shot command connections ------------------------------------------
 *
 * A CLI invocation like `tmux send-keys -t work ...` needs to run a command
 * against a session's server whether or not anything is interactively
 * attached to it right now. Those connections land on a *separate* pipe (see
 * ipc_cmd_pipe_name) so they never contend with the single-instance
 * interactive-attach pipe. A dedicated thread accepts them and hands the
 * command line off to this queue; only the main loop thread (which already
 * owns `session_t` exclusively) ever calls session_run(), so the queue is the
 * only cross-thread synchronization needed. */

/* One queued command plus a place for the main loop to leave its text result
 * (if any -- see session_run_capture) and an event to signal when done. The
 * acceptor thread that created a node owns it end to end: it pushes the
 * node, waits on `done`, then reads `result` and frees the node itself. The
 * main-loop thread (cmdq_drain) only ever fills `result` and signals `done`
 * -- it never frees a node, so there's no free/use race between the two
 * threads as long as the acceptor thread doesn't touch the node again before
 * `done` is signaled. */
typedef struct cmdq_node {
    char              *cmd;
    strbuf_t           result;
    HANDLE             done;
    struct cmdq_node  *next;
} cmdq_node_t;

typedef struct {
    cmdq_node_t      *head, *tail;
    CRITICAL_SECTION  lock;
} cmdq_t;

static void cmdq_init(cmdq_t *q)
{
    q->head = q->tail = NULL;
    InitializeCriticalSection(&q->lock);
}

static void cmdq_free(cmdq_t *q)
{
    cmdq_node_t *n = q->head;
    while (n) {
        cmdq_node_t *next = n->next;
        free(n->cmd);
        strbuf_free(&n->result);
        if (n->done) CloseHandle(n->done);
        free(n);
        n = next;
    }
    DeleteCriticalSection(&q->lock);
}

/* Returns the pushed node (still owned by the caller) or NULL on allocation
 * failure. */
static cmdq_node_t *cmdq_push(cmdq_t *q, const char *cmd)
{
    cmdq_node_t *n = (cmdq_node_t *)calloc(1, sizeof(*n));
    if (n == NULL)
        return NULL;
    n->cmd = _strdup(cmd);
    strbuf_init(&n->result);
    n->done = CreateEvent(NULL, TRUE, FALSE, NULL);
    n->next = NULL;
    EnterCriticalSection(&q->lock);
    if (q->tail) q->tail->next = n; else q->head = n;
    q->tail = n;
    LeaveCriticalSection(&q->lock);
    return n;
}

/* NULL when the queue is empty. The main loop (only) pops; it must call
 * cmdq_finish() on whatever it pops, exactly once, to release it back to its
 * owning acceptor thread. */
static cmdq_node_t *cmdq_pop(cmdq_t *q)
{
    cmdq_node_t *n;
    EnterCriticalSection(&q->lock);
    n = q->head;
    if (n) {
        q->head = n->next;
        if (q->head == NULL) q->tail = NULL;
    }
    LeaveCriticalSection(&q->lock);
    return n;
}

/* Run every queued command against `sess`, capturing any text result, and
 * hand each node back to its waiting acceptor thread. Main-loop thread only. */
static void cmdq_drain(cmdq_t *q, session_t *sess)
{
    cmdq_node_t *n;
    while ((n = cmdq_pop(q)) != NULL) {
        session_run_capture(sess, n->cmd, &n->result);
        SetEvent(n->done);   /* the acceptor thread now owns *n again */
    }
}

typedef struct {
    wchar_t          pipename[512];
    cmdq_t           *q;
    HANDLE            wake;      /* nudge the main loop to drain promptly */
    volatile LONG     stop;
} cmd_acceptor_ctx_t;

static DWORD WINAPI cmd_acceptor_thread(LPVOID arg)
{
    cmd_acceptor_ctx_t *c = (cmd_acceptor_ctx_t *)arg;
    for (;;) {
        HANDLE pipe;
        unsigned char type;
        strbuf_t payload;

        if (InterlockedCompareExchange(&c->stop, 0, 0))
            break;

        pipe = ipc_server_listen(c->pipename);
        if (pipe == INVALID_HANDLE_VALUE) {
            if (InterlockedCompareExchange(&c->stop, 0, 0))
                break;
            Sleep(50);
            continue;
        }
        if (InterlockedCompareExchange(&c->stop, 0, 0)) {
            CloseHandle(pipe);
            break;
        }

        strbuf_init(&payload);
        if (ipc_read_frame(pipe, &type, &payload) == 0 && type == MSG_CMD && payload.len > 0) {
            char *cmd = (char *)malloc(payload.len + 1);
            if (cmd) {
                cmdq_node_t *n;
                memcpy(cmd, payload.data, payload.len);
                cmd[payload.len] = '\0';
                n = cmdq_push(c->q, cmd);
                free(cmd);
                if (c->wake) SetEvent(c->wake);
                /* The main loop drains this queue at least every ~50ms (see
                 * cmdq_drain call sites in run_server/serve_client), so this
                 * wait is bounded in every normal case; it only blocks
                 * indefinitely if the server itself is stuck, in which case
                 * the whole process (and this thread with it) is about to
                 * go away anyway. Waiting forever avoids any timeout-driven
                 * free/use race with the main loop over *n. */
                if (n != NULL && n->done != NULL &&
                    WaitForSingleObject(n->done, INFINITE) == WAIT_OBJECT_0) {
                    if (n->result.len > 0)
                        ipc_write_frame(pipe, MSG_CMD_TEXT, n->result.data, (uint32_t)n->result.len);
                    else
                        ipc_write_frame(pipe, MSG_CMD_OK, NULL, 0);
                    free(n->cmd);
                    strbuf_free(&n->result);
                    CloseHandle(n->done);
                    free(n);
                }
            }
        }
        strbuf_free(&payload);

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

/* ----- one attached client -------------------------------------------------- */

typedef struct srv_client {
    HANDLE            pipe;
    HANDLE            wake;
    CRITICAL_SECTION  lock;
    strbuf_t          in_buf;    /* accumulated MSG_INPUT bytes */
    int               new_size;  /* a resize/attach is pending */
    int               cols, rows;
    volatile LONG     disconnected;
    volatile LONG     kill;      /* client asked the server to exit */
} srv_client_t;

/* Reader thread: decode frames from the client into input / resize state. */
static DWORD WINAPI srv_reader(LPVOID arg)
{
    srv_client_t *c = (srv_client_t *)arg;
    unsigned char type;
    strbuf_t payload;
    strbuf_init(&payload);

    for (;;) {
        if (ipc_read_frame(c->pipe, &type, &payload) != 0)
            break;
        EnterCriticalSection(&c->lock);
        if (type == MSG_INPUT) {
            strbuf_append(&c->in_buf, payload.data, payload.len);
        } else if (type == MSG_ATTACH || type == MSG_RESIZE) {
            int cc, rr;
            ipc_unpack_size((const unsigned char *)payload.data, payload.len, &cc, &rr);
            c->cols = cc;
            c->rows = rr;
            c->new_size = 1;
        } else if (type == MSG_KILL) {
            InterlockedExchange(&c->kill, 1);
        }
        LeaveCriticalSection(&c->lock);
        SetEvent(c->wake);
    }

    strbuf_free(&payload);
    InterlockedExchange(&c->disconnected, 1);
    SetEvent(c->wake);
    return 0;
}

/* Serve one connected client until it detaches, disconnects, or the session
 * ends. Returns 1 if the session is still alive afterward (client left), 0 if
 * the session ended. */
static int serve_client(session_t *sess, HANDLE pipe, HANDLE wake, strbuf_t *frame, cmdq_t *cmdq)
{
    srv_client_t c;
    strbuf_t local;
    HANDLE reader;
    int session_ended = 0;

    memset(&c, 0, sizeof(c));
    c.pipe = pipe;
    c.wake = wake;
    InitializeCriticalSection(&c.lock);
    strbuf_init(&c.in_buf);
    strbuf_init(&local);

    reader = CreateThread(NULL, 0, srv_reader, &c, 0, NULL);

    /* Repaint everything for the newly attached client. */
    session_force_redraw(sess);

    for (;;) {
        int resized = 0;

        WaitForSingleObject(wake, 50);

        EnterCriticalSection(&c.lock);
        if (c.new_size) {
            resized = 1;
            c.new_size = 0;
        }
        if (c.in_buf.len) {
            strbuf_clear(&local);
            strbuf_append(&local, c.in_buf.data, c.in_buf.len);
            strbuf_clear(&c.in_buf);
        }
        LeaveCriticalSection(&c.lock);

        if (resized) {
            session_resize(sess, c.cols, c.rows);
            session_force_redraw(sess);
        }
        if (local.len) {
            session_input(sess, local.data, local.len);
            strbuf_clear(&local);
        }

        cmdq_drain(cmdq, sess);
        session_tick(sess);

        if (InterlockedCompareExchange(&c.kill, 0, 0)) {
            session_ended = 1;                 /* kill-session/kill-server */
            ipc_write_frame(pipe, MSG_EXIT, NULL, 0);
            break;
        }

        if (session_take_detach(sess)) {
            ipc_write_frame(pipe, MSG_DETACH, NULL, 0);
            break;
        }

        session_pump(sess);
        if (!session_alive(sess)) {
            session_ended = 1;
            ipc_write_frame(pipe, MSG_EXIT, NULL, 0);
            break;
        }

        session_render(sess, frame);
        if (frame->len) {
            if (ipc_write_frame(pipe, MSG_OUTPUT, frame->data, (uint32_t)frame->len) != 0)
                break;   /* client gone */
        }

        if (InterlockedCompareExchange(&c.disconnected, 0, 0))
            break;
    }

    /* Unblock the reader (it may be parked in ReadFile) and reap it. */
    CancelIoEx(pipe, NULL);
    if (reader) {
        WaitForSingleObject(reader, 1000);
        CloseHandle(reader);
    }
    strbuf_free(&c.in_buf);
    strbuf_free(&local);
    DeleteCriticalSection(&c.lock);
    return session_ended ? 0 : 1;
}

int run_server(const wchar_t *pipename, const wchar_t *shell, const wchar_t *cwd,
               int cols, int rows, const char *cfgpath)
{
    HANDLE wake;
    session_t *sess;
    strbuf_t frame;
    cmdq_t cmdq;
    cmd_acceptor_ctx_t cmdctx;
    HANDLE cmd_thread;

    srvlog("run_server: start");
    wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    sess = session_create_in(shell, cols > 0 ? cols : 80, rows > 0 ? rows : 25, wake, cwd);
    if (sess == NULL) {
        srvlog("run_server: session_create FAILED");
        if (wake) CloseHandle(wake);
        return 1;
    }
    session_load_config_from(sess, cfgpath);
    srvlog("run_server: session created");
    strbuf_init(&frame);

    cmdq_init(&cmdq);
    memset(&cmdctx, 0, sizeof(cmdctx));
    /* ipc_cmd_pipe_name derives from a *session name*, but here we only have
     * the already-built interactive pipe name; append the same "-cmd" suffix
     * directly rather than re-deriving from a session name. */
    _snwprintf_s(cmdctx.pipename, 512, _TRUNCATE, L"%s-cmd", pipename);
    cmdctx.q = &cmdq;
    cmdctx.wake = wake;
    cmd_thread = CreateThread(NULL, 0, cmd_acceptor_thread, &cmdctx, 0, NULL);

    while (session_alive(sess)) {
        ipc_accept_t acc;
        HANDLE pipe;
        int rc = 0, still_alive;

        srvlog("run_server: listening");
        pipe = ipc_server_begin_listen(pipename, &acc);
        if (pipe == INVALID_HANDLE_VALUE) {
            srvlog("run_server: listen FAILED");
            break;
        }

        /* Detached: poll for the next client while still servicing queued
         * one-shot commands and pumping pane output, instead of blocking. */
        for (;;) {
            rc = ipc_server_accept_poll(pipe, &acc, 50);
            if (rc != 0)
                break;
            cmdq_drain(&cmdq, sess);
            session_tick(sess);
            session_pump(sess);
            if (!session_alive(sess))
                break;
        }
        ipc_server_accept_cancel(pipe, &acc);

        if (!session_alive(sess)) {
            CloseHandle(pipe);
            break;
        }
        if (rc != 1) {
            srvlog("run_server: accept error, retrying");
            CloseHandle(pipe);
            continue;
        }

        srvlog("run_server: client connected");
        still_alive = serve_client(sess, pipe, wake, &frame, &cmdq);
        srvlog(still_alive ? "run_server: client left (alive)" : "run_server: session ended");
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (!still_alive)
            break;
        /* Detached: loop back and wait for the next client. Panes keep running;
         * their output accumulates until reattach. */
    }

    /* One last drain so a command pushed right as the loop above was ending
     * still gets a proper reply (its acceptor thread signaled) instead of
     * being torn down mid-wait by cmdq_free below. */
    cmdq_drain(&cmdq, sess);

    InterlockedExchange(&cmdctx.stop, 1);
    if (cmd_thread) {
        /* Unblock a pending accept in the acceptor thread. */
        HANDLE dummy = ipc_client_connect(cmdctx.pipename, 0);
        if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);
        WaitForSingleObject(cmd_thread, 1000);
        CloseHandle(cmd_thread);
    }
    cmdq_free(&cmdq);

    session_free(sess);
    strbuf_free(&frame);
    if (wake) CloseHandle(wake);
    return 0;
}

/* ----- standalone (no client/server split) ---------------------------------- */

typedef struct standalone_input {
    winterm_t        *term;
    CRITICAL_SECTION *lock;
    strbuf_t         *buf;
    HANDLE            wake;
} standalone_input_t;

static DWORD WINAPI standalone_reader(LPVOID arg)
{
    standalone_input_t *c = (standalone_input_t *)arg;
    char buf[SRV_IO_BUF];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(c->term->in, buf, sizeof(buf), &n, NULL) || n == 0)
            break;
        EnterCriticalSection(c->lock);
        strbuf_append(c->buf, buf, n);
        LeaveCriticalSection(c->lock);
        SetEvent(c->wake);
    }
    return 0;
}

int run_standalone(const wchar_t *shell, const wchar_t *cwd)
{
    winterm_t term;
    HANDLE wake, h_input;
    session_t *sess;
    standalone_input_t ictx;
    CRITICAL_SECTION in_lock;
    strbuf_t in_buf, in_local, frame;
    short cols = 80, rows = 25, pc, pr;

    if (winterm_enable(&term) != 0) {
        fprintf(stderr, "tmuxw: failed to set up console\n");
        return 1;
    }
    winterm_size(&term, &cols, &rows);
    pc = cols; pr = rows;

    wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    sess = session_create_in(shell, cols, rows, wake, cwd);
    if (sess == NULL) {
        if (wake) CloseHandle(wake);
        winterm_restore(&term);
        fprintf(stderr, "tmuxw: failed to spawn pseudo console\n");
        return 1;
    }
    session_load_config(sess);

    InitializeCriticalSection(&in_lock);
    strbuf_init(&in_buf);
    strbuf_init(&in_local);
    strbuf_init(&frame);

    ictx.term = &term;
    ictx.lock = &in_lock;
    ictx.buf = &in_buf;
    ictx.wake = wake;
    h_input = CreateThread(NULL, 0, standalone_reader, &ictx, 0, NULL);

    while (session_alive(sess)) {
        short c, r;
        WaitForSingleObject(wake, 50);

        winterm_size(&term, &c, &r);
        if (c != pc || r != pr) {
            pc = c; pr = r;
            session_resize(sess, c, r);
        }

        EnterCriticalSection(&in_lock);
        if (in_buf.len) {
            strbuf_clear(&in_local);
            strbuf_append(&in_local, in_buf.data, in_buf.len);
            strbuf_clear(&in_buf);
        }
        LeaveCriticalSection(&in_lock);
        if (in_local.len) {
            session_input(sess, in_local.data, in_local.len);
            strbuf_clear(&in_local);
        }

        session_tick(sess);
        session_take_detach(sess);   /* no server to detach from; ignore */
        session_pump(sess);
        session_render(sess, &frame);
        if (frame.len)
            WriteFile(term.out, frame.data, (DWORD)frame.len, NULL, NULL);
    }

    session_free(sess);
    if (h_input) CloseHandle(h_input);
    strbuf_free(&in_buf);
    strbuf_free(&in_local);
    strbuf_free(&frame);
    DeleteCriticalSection(&in_lock);
    if (wake) CloseHandle(wake);

    /* Leave the host terminal clean for the shell we return to. */
    strbuf_init(&frame);
    render_clear(&frame);
    WriteFile(term.out, frame.data, (DWORD)frame.len, NULL, NULL);
    strbuf_free(&frame);

    winterm_restore(&term);
    return 0;
}

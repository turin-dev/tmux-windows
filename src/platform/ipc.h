/* ipc.h — client/server transport over a Windows named pipe.
 *
 * Messages are length-framed on a byte stream: [u8 type][u32 le length][bytes].
 * The client is a thin relay; the server owns all session state. Message types:
 *
 *   Client -> Server                Server -> Client
 *   ----------------                ----------------
 *   MSG_ATTACH  {u16 cols,u16 rows} MSG_OUTPUT {vt bytes to display}
 *   MSG_INPUT   {raw key bytes}     MSG_DETACH {} (client should exit, server stays)
 *   MSG_RESIZE  {u16 cols,u16 rows} MSG_EXIT   {} (server ending)
 *   MSG_CMD     {utf-8 command}     MSG_CMD_OK   {} (command ran, no output)
 *                                   MSG_CMD_TEXT {utf-8 text} (command's output,
 *                                                e.g. list-windows/list-panes)
 *
 * MSG_CMD travels over a *separate* pipe (see ipc_cmd_pipe_name) from
 * MSG_ATTACH/INPUT/RESIZE: it lets a one-shot CLI invocation (e.g.
 * `tmux send-keys -t work ...`) run a command against a session's server
 * without contending with (or requiring) an interactively attached client on
 * the main pipe, which only ever holds one connection at a time.
 */
#ifndef TMUXW_PLATFORM_IPC_H
#define TMUXW_PLATFORM_IPC_H

#include <windows.h>
#include <stdint.h>

#include "util/strbuf.h"

enum {
    MSG_ATTACH  = 1,
    MSG_INPUT   = 2,
    MSG_RESIZE  = 3,
    MSG_KILL    = 4,   /* client -> server: end the session/server now */
    MSG_CMD     = 5,   /* client -> server: run one command line (see above) */
    MSG_OUTPUT  = 10,
    MSG_DETACH  = 11,
    MSG_EXIT    = 12,
    MSG_CMD_OK  = 13,  /* server -> client: MSG_CMD ran, no text output */
    MSG_CMD_TEXT = 14  /* server -> client: MSG_CMD ran, payload is its output */
};

/* Build the pipe name for a session (e.g. "default") into `out`. */
void ipc_pipe_name(wchar_t *out, size_t cap, const wchar_t *session);

/* Build the one-shot command pipe name for a session -- distinct from the
 * interactive-attach pipe (see the MSG_CMD note above). */
void ipc_cmd_pipe_name(wchar_t *out, size_t cap, const wchar_t *session);

/* List running tmuxw sessions for the current user by enumerating named pipes.
 * Writes up to `max` NUL-terminated session names (UTF-8) into `out`, each up to
 * `namecap` bytes. Returns the count found. */
int ipc_list_sessions(char *out, int max, int namecap);

/* Write one frame. Returns 0 on success, non-zero on I/O error. */
int ipc_write_frame(HANDLE h, unsigned char type, const void *payload, uint32_t len);

/* Read one frame into `payload` (cleared first). Sets *type. Returns 0 on
 * success, <0 on EOF/error. */
int ipc_read_frame(HANDLE h, unsigned char *type, strbuf_t *payload);

/* Pack/unpack a (cols, rows) pair as two little-endian u16 (4 bytes). */
void ipc_pack_size(unsigned char buf[4], int cols, int rows);
void ipc_unpack_size(const unsigned char *buf, size_t len, int *cols, int *rows);

/* Server: create a pipe instance and block until a client connects. Returns the
 * connected pipe handle, or INVALID_HANDLE_VALUE on failure. */
HANDLE ipc_server_listen(const wchar_t *pipename);

/* Non-blocking accept, for a caller that needs to do other work (drain a
 * command queue, pump panes) while waiting for a client. Usage:
 *
 *   ipc_accept_t acc;
 *   HANDLE pipe = ipc_server_begin_listen(pipename, &acc);
 *   for (;;) {
 *       int rc = ipc_server_accept_poll(pipe, &acc, 50);   // 50ms slices
 *       if (rc != 0) break;
 *       ...do other work...
 *   }
 *   ipc_server_accept_cancel(pipe, &acc);
 *   if (rc == 1) ...pipe is now a connected client...
 *   else         CloseHandle(pipe);   // rc == -1: accept failed
 */
typedef struct {
    OVERLAPPED ov;
    HANDLE     ev;
    int        immediate;
} ipc_accept_t;

HANDLE ipc_server_begin_listen(const wchar_t *pipename, ipc_accept_t *acc);
/* 1 = connected, 0 = still pending (call again), -1 = error. */
int    ipc_server_accept_poll(HANDLE pipe, ipc_accept_t *acc, DWORD timeout_ms);
void   ipc_server_accept_cancel(HANDLE pipe, ipc_accept_t *acc);

/* Client: connect to the server pipe, retrying up to timeout_ms. Returns the
 * pipe handle, or INVALID_HANDLE_VALUE on failure. */
HANDLE ipc_client_connect(const wchar_t *pipename, int timeout_ms);

/* Like ipc_client_connect, but if non-NULL, *out_busy is set to 1 when the
 * pipe exists but is already occupied by another client (as opposed to not
 * existing at all) -- lets callers tell "no such session" apart from
 * "session exists, already has a client attached". */
HANDLE ipc_client_connect_ex(const wchar_t *pipename, int timeout_ms, int *out_busy);

/* Create a connected overlapped pipe pair in this process (for tests). Returns
 * 0 on success with both handles set; the caller closes them. */
int ipc_make_pair(HANDLE *server, HANDLE *client);

#endif /* TMUXW_PLATFORM_IPC_H */

/* client.h — the tmuxw client: a thin relay between the host terminal and a
 * server over a connected named pipe.
 */
#ifndef TMUXW_CLIENT_H
#define TMUXW_CLIENT_H

#include <windows.h>

/* Attach to a server over `pipe`: forward host keystrokes and window size to
 * the server, write server output frames to the terminal. Returns when the
 * server sends detach/exit or the pipe breaks. Takes ownership of `pipe`.
 *
 * If the server requests switch-client (MSG_SWITCH), this reconnects in
 * place to the named target session's pipe (within the same -L/-S
 * namespace, `ns` -- NULL/empty for the default namespace) and keeps
 * running, rather than returning; the caller never sees the switch happen. */
int run_client(HANDLE pipe, const wchar_t *ns);

#endif /* TMUXW_CLIENT_H */

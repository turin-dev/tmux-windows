/* server.h — the tmuxw server: owns the session, accepts clients over a named
 * pipe, and (for debugging) a single-process standalone driver.
 */
#ifndef TMUXW_SERVER_H
#define TMUXW_SERVER_H

#include <windows.h>

/* Background server: create the pipe, own a session running `shell` (starting
 * in `cwd`, NULL/empty for the caller's current directory, at `cols` x `rows`,
 * <= 0 for the built-in default), load `cfgpath` (NULL/empty for the default
 * ~/.tmuxw.conf, tmux's -f) as its config, and serve attaching clients until
 * the session's last pane exits. Returns 0 on clean shutdown. Has no console
 * of its own. */
int run_server(const wchar_t *pipename, const wchar_t *shell, const wchar_t *cwd,
               int cols, int rows, const char *cfgpath);

/* Single-process driver: run a session directly against the host console (no
 * client/server split). Handy for debugging the TUI. */
int run_standalone(const wchar_t *shell, const wchar_t *cwd);

#endif /* TMUXW_SERVER_H */

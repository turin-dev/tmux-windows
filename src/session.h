/* session.h — a window of panes and the prefix-key command state, independent
 * of any transport.
 *
 * The session owns the layout tree, panes, active-pane selection, and the
 * prefix state machine. It consumes raw host input via session_input() and
 * produces host-terminal output via session_render(); the caller wires those to
 * a real console (standalone) or a client pipe (server). This lets the exact
 * same logic run in-process or behind the client/server split.
 */
#ifndef TMUXW_SESSION_H
#define TMUXW_SESSION_H

#include <windows.h>
#include <stddef.h>

#include "util/strbuf.h"

typedef struct session session_t;

/* Create a window with one pane running `shell` at cols x rows. `wake` is
 * signalled by pane output (the caller waits on it). `shell` must outlive the
 * session. Returns NULL on failure. */
session_t *session_create(const wchar_t *shell, int cols, int rows, HANDLE wake);

/* Like session_create, but the initial pane starts in `cwd` (NULL/empty
 * inherits the caller's current directory). */
session_t *session_create_in(const wchar_t *shell, int cols, int rows, HANDLE wake,
                              const wchar_t *cwd);
void       session_free(session_t *s);

/* Feed host input bytes through the prefix state machine, routing ordinary
 * bytes to the active pane. */
void       session_input(session_t *s, const char *bytes, size_t n);

/* Change the window size and re-tile. */
void       session_resize(session_t *s, int cols, int rows);

/* Drain pane output into screens and reap any exited children. */
void       session_pump(session_t *s);

/* Refresh time-based UI (the status-bar clock). Call once per driver loop. */
void       session_tick(session_t *s);

/* Fill `frame` with the VT bytes to display, if anything changed since the last
 * render. `frame` is left empty when there is nothing to send. */
void       session_render(session_t *s, strbuf_t *frame);

/* Force a full repaint on the next render (e.g. after a client (re)attaches). */
void       session_force_redraw(session_t *s);

/* Record whether an interactive client is currently attached (server.c calls
 * this around serve_client); used by list-clients. */
void       session_set_attached(session_t *s, int on);

/* True while at least one pane remains. */
int        session_alive(const session_t *s);

/* Consume a pending detach request (Ctrl-B d). Returns 1 once per request. */
int        session_take_detach(session_t *s);

/* Run a single command line (e.g. "split-window -h"). */
void       session_run(session_t *s, const char *cmdline);

/* Like session_run, but if `cmdline` is (or chains) a command that produces
 * text -- list-windows/lsw, list-panes/lsp -- that text is appended to
 * `out`. `out` may be NULL to behave exactly like session_run. Ordinary
 * (non-listing) commands leave `out` untouched. */
void       session_run_capture(session_t *s, const char *cmdline, strbuf_t *out);

/* Load and run the user's config (~/.tmuxw.conf) if present. */
void       session_load_config(session_t *s);

/* Like session_load_config, but loads `override_path` instead when non-NULL/
 * non-empty (tmux -f <file>). */
void       session_load_config_from(session_t *s, const char *override_path);

#endif /* TMUXW_SESSION_H */

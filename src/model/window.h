/* window.h — a window: one full-screen tab, holding a layout tree of panes.
 *
 * A session owns several windows (tabs) and shows one at a time. All the pane
 * operations (split, select, kill) and the per-window compositing live here;
 * the session layers window switching and the status bar on top.
 */
#ifndef TMUXW_MODEL_WINDOW_H
#define TMUXW_MODEL_WINDOW_H

#include <windows.h>
#include <stddef.h>

#include "layout.h"
#include "model/pane.h"
#include "copymode.h"
#include "util/strbuf.h"

typedef struct window {
    layout_node_t *root;
    pane_t        *active;
    pane_t        *last_active;    /* previously active pane (for last-pane) */
    int            next_pane_id;
    int            cols, rows;     /* area available to panes (excludes status) */
    int            zoomed;         /* active pane temporarily fills the window */
    int            layout;         /* last-applied LAYOUT_* preset (for cycling) */
    layout_node_t *drag;           /* divider being dragged with the mouse, or NULL */
    char           name[64];       /* display name (derived from the shell) */
    int            refcount;       /* live references, see window_link/window_free;
                                    * link-window inserts a second reference to the
                                    * SAME window_t at another index instead of
                                    * copying it, so both indices show one shared,
                                    * live window exactly like tmux's window objects */
} window_t;

/* Create a window with one pane running `shell`, starting in `cwd` (NULL/empty
 * inherits the caller's current directory) with environment `envblock`
 * (NULL inherits the caller's environment; see conpty_spawn for its format).
 * Returns NULL on failure. */
window_t *window_create(const wchar_t *shell, int cols, int rows, HANDLE wake,
                        const wchar_t *cwd, const wchar_t *envblock);
/* Drop one reference; frees the window once its refcount reaches zero (see
 * window_link). Safe to call on NULL. */
void      window_free(window_t *w);
/* Take a second reference to `w` for link-window: returns `w` with its
 * refcount incremented, so inserting the result at another index makes both
 * indices show the exact same live window (shared panes/state), and
 * unlinking (window_free) either one leaves the other unaffected. */
window_t *window_link(window_t *w);

/* Wrap an already-running pane in a fresh window (used by break-pane). Takes
 * ownership of `p`. Returns NULL on failure (the caller then owns `p`). */
window_t *window_create_with_pane(pane_t *p, int cols, int rows, const char *name);

/* Detach the active pane from this window without closing it, returning the
 * pane; the window re-tiles around the rest. Returns NULL if it is the only
 * pane (nothing to break out). */
pane_t   *window_extract_active(window_t *w);

/* Like window_extract_active but also removes the last pane (the window becomes
 * empty, root == NULL). Used by join-pane. */
pane_t   *window_detach_active(window_t *w);

/* Insert an already-running pane by splitting the active pane (`type`); the
 * inserted pane becomes active. If the window is empty, `p` becomes its sole
 * pane. Returns 1 on success (else the caller still owns `p`). */
int       window_insert_pane(window_t *w, pane_t *p, int type);

/* Re-tile panes into a cols x rows area. */
void      window_apply(window_t *w, int cols, int rows);

/* Split the active pane (LN_SPLIT_V / LN_SPLIT_H); the new pane becomes
 * active. `envblock` as in window_create. */
void      window_split(window_t *w, int type, const wchar_t *shell, HANDLE wake,
                       const wchar_t *envblock);

void      window_select_next_pane(window_t *w);
void      window_select_dir(window_t *w, int dir);

/* Resize the active pane by `amount` cells toward `dir`. No-op while zoomed. */
void      window_resize_active(window_t *w, int dir, int amount);

/* Re-tile the panes into a named preset (LAYOUT_*); unzooms first. */
void      window_set_layout(window_t *w, int preset);
/* Cycle to the next / previous preset. */
void      window_next_layout(window_t *w);
void      window_previous_layout(window_t *w);

/* Toggle zoom of the active pane (fills the window, hiding the others). */
void      window_toggle_zoom(window_t *w);
int       window_is_zoomed(const window_t *w);

/* The pane at cell (x, y), or NULL. */
pane_t   *window_pane_at(window_t *w, int x, int y);
/* Make `p` active if it belongs to this window. */
void      window_select_pane(window_t *w, pane_t *p);
/* Make the pane at traversal index `n` active. */
void      window_select_index(window_t *w, int n);

/* Mouse gestures: press either starts a divider drag (returns 1) or selects the
 * pane under the cursor; drag moves an in-progress divider; release ends it. */
int       window_mouse_press(window_t *w, int x, int y);
void      window_mouse_drag(window_t *w, int x, int y);
void      window_mouse_release(window_t *w);

/* Rotate panes through the layout positions (downward = toward the next slot). */
void      window_rotate(window_t *w, int downward);
/* Swap the active pane with its neighbor (next=1) or previous (next=0). */
void      window_swap_active(window_t *w, int next);
/* Re-select the previously active pane, if it still exists. */
void      window_select_last(window_t *w);

/* The active pane (NULL if the window is empty). */
pane_t   *window_active(window_t *w);

/* Kill the active pane. Returns 1 if the window is now empty. */
int       window_kill_active(window_t *w);

/* Restart the active pane's child process in place (respawn-pane). Returns 0
 * on success, -1 if there is no active pane or the respawn itself failed. */
int       window_respawn_active(window_t *w);
/* Restart every pane's child process in the window (respawn-window). */
void      window_respawn_all(window_t *w);

/* Close every pane except the active one (kill-pane -a). */
void      window_kill_others(window_t *w);

/* Forward input bytes to the active pane's child. */
void      window_write_active(window_t *w, const char *bytes, size_t n);

/* Parse pane output and reap exited children. Returns bytes parsed (>0 means
 * the window changed). Removes dead panes; the window may become empty. If
 * `out_died` is non-NULL, it's set to 1 when at least one pane was reaped
 * (for the pane-died hook), 0 otherwise. */
size_t    window_pump_ex(window_t *w, int *out_died);
size_t    window_pump(window_t *w);
int       window_empty(const window_t *w);

/* Composite the window's panes + dividers into `frame`. When full_redraw is
 * set, every pane is repainted and dividers redrawn. If `cm` is non-NULL and
 * active on the window's active pane, that pane is drawn in copy mode. */
void      window_render(strbuf_t *frame, window_t *w, int full_redraw,
                        const copymode_t *cm);

/* Overlay each pane's number (starting at `base`) centred in reverse video,
 * for display-panes. */
void      window_display_panes(strbuf_t *frame, window_t *w, int base);

#endif /* TMUXW_MODEL_WINDOW_H */

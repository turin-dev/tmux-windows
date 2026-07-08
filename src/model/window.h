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
    char           name[64];       /* display name (derived from the shell) */
} window_t;

/* Create a window with one pane running `shell`. Returns NULL on failure. */
window_t *window_create(const wchar_t *shell, int cols, int rows, HANDLE wake);
void      window_free(window_t *w);

/* Re-tile panes into a cols x rows area. */
void      window_apply(window_t *w, int cols, int rows);

/* Split the active pane (LN_SPLIT_V / LN_SPLIT_H); the new pane becomes active. */
void      window_split(window_t *w, int type, const wchar_t *shell, HANDLE wake);

void      window_select_next_pane(window_t *w);
void      window_select_dir(window_t *w, int dir);

/* Resize the active pane by `amount` cells toward `dir`. No-op while zoomed. */
void      window_resize_active(window_t *w, int dir, int amount);

/* Re-tile the panes into a named preset (LAYOUT_*); unzooms first. */
void      window_set_layout(window_t *w, int preset);
/* Cycle to the next preset. */
void      window_next_layout(window_t *w);

/* Toggle zoom of the active pane (fills the window, hiding the others). */
void      window_toggle_zoom(window_t *w);
int       window_is_zoomed(const window_t *w);

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

/* Forward input bytes to the active pane's child. */
void      window_write_active(window_t *w, const char *bytes, size_t n);

/* Parse pane output and reap exited children. Returns bytes parsed (>0 means
 * the window changed). Removes dead panes; the window may become empty. */
size_t    window_pump(window_t *w);
int       window_empty(const window_t *w);

/* Composite the window's panes + dividers into `frame`. When full_redraw is
 * set, every pane is repainted and dividers redrawn. If `cm` is non-NULL and
 * active on the window's active pane, that pane is drawn in copy mode. */
void      window_render(strbuf_t *frame, window_t *w, int full_redraw,
                        const copymode_t *cm);

#endif /* TMUXW_MODEL_WINDOW_H */

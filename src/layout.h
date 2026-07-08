/* layout.h — a binary tree that tiles panes across the window.
 *
 * A node is either a leaf (one pane) or a split. A vertical split (LN_SPLIT_V)
 * places its children side by side with a one-column divider between them; a
 * horizontal split (LN_SPLIT_H) stacks them with a one-row divider. layout_apply
 * walks the tree assigning each pane its geometry (and resizing it); the
 * renderer draws the dividers.
 */
#ifndef TMUXW_LAYOUT_H
#define TMUXW_LAYOUT_H

#include "model/pane.h"
#include "util/strbuf.h"

#define TMUXW_MAX_PANES 256

enum {
    LN_LEAF,
    LN_SPLIT_V,   /* vertical divider; children left (a) | right (b) */
    LN_SPLIT_H    /* horizontal divider; children top (a) / bottom (b) */
};

/* Directions for directional pane selection. */
enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

/* Named tmux layout presets (for select-layout / next-layout). */
enum {
    LAYOUT_EVEN_H,   /* even-horizontal: panes in a single row */
    LAYOUT_EVEN_V,   /* even-vertical:   panes in a single column */
    LAYOUT_MAIN_H,   /* main-horizontal: big pane on top, rest in a row */
    LAYOUT_MAIN_V,   /* main-vertical:   big pane on left, rest in a column */
    LAYOUT_TILED,    /* tiled:           roughly square grid */
    LAYOUT_COUNT
};

typedef struct layout_node {
    int                  type;
    struct layout_node  *parent;
    pane_t              *pane;        /* leaf only */
    struct layout_node  *a, *b;       /* split only */
    double               ratio;       /* fraction of space given to `a` */
    int                  dx, dy, dlen; /* divider geometry (split; set by apply) */
    int                  alloc_x, alloc_y;        /* origin assigned by apply */
    int                  alloc_cols, alloc_rows;  /* area assigned by apply */
} layout_node_t;

/* Create a leaf node wrapping `pane`. */
layout_node_t *layout_leaf(pane_t *pane);

/* Free the whole tree. If close_panes is nonzero, pane_close() each leaf pane. */
void layout_free(layout_node_t *node, int close_panes);

/* Split `leaf` into a `type` split: the existing pane becomes child a and
 * `new_pane` becomes child b. Updates *root if the tree root changed. Returns
 * the new split node. */
layout_node_t *layout_split(layout_node_t **root, layout_node_t *leaf,
                            int type, pane_t *new_pane);

/* Remove `leaf` and collapse its parent (the sibling takes the parent's slot).
 * Updates *root (may become NULL if the last leaf is removed). Does not free the
 * pane — the caller owns pane_close(). */
void layout_remove(layout_node_t **root, layout_node_t *leaf);

/* Assign geometry to every pane in the tree within the given rectangle. */
void layout_apply(layout_node_t *node, int x, int y, int cols, int rows);

/* Tree queries. */
layout_node_t *layout_first_leaf(layout_node_t *node);
layout_node_t *layout_next_leaf(layout_node_t *root, layout_node_t *cur);
layout_node_t *layout_find(layout_node_t *node, const pane_t *pane);
int            layout_count(const layout_node_t *node);

/* Collect leaf panes into `out` (up to max) in traversal order. Returns count. */
int            layout_collect(layout_node_t *node, pane_t **out, int max);

/* The pane nearest to `cur` in direction `dir`, or NULL if none. */
pane_t *layout_pane_in_dir(layout_node_t *root, const pane_t *cur, int dir);

/* Move the divider adjacent to `active` in direction `dir` by `amount` cells,
 * growing the active pane that way. Returns 1 if the layout changed (caller
 * then re-applies geometry). */
int layout_resize(layout_node_t *root, const pane_t *active, int dir, int amount);

/* Rebuild `*root`, arranging its existing panes into `preset` (one of the
 * LAYOUT_* values). Panes are reused (never closed), so any held pane pointer —
 * including the active pane — stays valid. Returns 1 on success. */
int layout_set_preset(layout_node_t **root, int preset);

/* Map between a preset id and its tmux name ("tiled", "even-horizontal", ...).
 * layout_preset_from_name returns -1 if the name is unknown. */
const char *layout_preset_name(int preset);
int         layout_preset_from_name(const char *name);

/* Rotate which pane occupies each leaf: downward moves every pane to the next
 * leaf position (the last wraps to the first); upward is the reverse. The tree
 * shape is unchanged. Caller re-applies geometry. */
void layout_rotate(layout_node_t *root, int downward);

/* Swap the pane at `active`'s leaf with its neighbor in traversal order (next=1
 * for the following pane, 0 for the preceding; wraps). Returns 1 if swapped. */
int layout_swap(layout_node_t *root, const pane_t *active, int next);

/* Mouse hit-testing (coordinates are 0-based cells; requires a prior apply). */

/* The pane whose rectangle contains (x, y), or NULL. */
pane_t *layout_pane_at(layout_node_t *root, int x, int y);

/* If (x, y) lands on a split's divider, return that split (with *vertical set to
 * 1 for a vertical divider, 0 for horizontal); otherwise NULL. */
layout_node_t *layout_divider_at(layout_node_t *root, int x, int y, int *vertical);

/* Move `split`'s divider to pass through (x, y), adjusting its ratio. Returns 1
 * if the ratio changed. */
int layout_set_divider(layout_node_t *split, int x, int y);

/* Draw all split dividers as box-drawing lines into `out`. */
void layout_draw_borders(const layout_node_t *node, strbuf_t *out);

#endif /* TMUXW_LAYOUT_H */

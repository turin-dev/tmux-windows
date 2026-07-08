/* layout.c — see layout.h. */
#include "layout.h"

#include <stdlib.h>

layout_node_t *layout_leaf(pane_t *pane)
{
    layout_node_t *n = (layout_node_t *)calloc(1, sizeof(*n));
    if (n == NULL)
        return NULL;
    n->type = LN_LEAF;
    n->pane = pane;
    n->ratio = 0.5;
    return n;
}

void layout_free(layout_node_t *node, int close_panes)
{
    if (node == NULL)
        return;
    if (node->type == LN_LEAF) {
        if (close_panes && node->pane)
            pane_close(node->pane);
    } else {
        layout_free(node->a, close_panes);
        layout_free(node->b, close_panes);
    }
    free(node);
}

layout_node_t *layout_split(layout_node_t **root, layout_node_t *leaf,
                            int type, pane_t *new_pane)
{
    layout_node_t *split = (layout_node_t *)calloc(1, sizeof(*split));
    layout_node_t *newleaf = layout_leaf(new_pane);
    layout_node_t *oldparent = leaf->parent;

    if (split == NULL || newleaf == NULL) {
        free(split);
        free(newleaf);
        return NULL;
    }

    split->type = type;
    split->ratio = 0.5;
    split->a = leaf;
    split->b = newleaf;
    split->parent = oldparent;
    leaf->parent = split;
    newleaf->parent = split;

    if (oldparent == NULL)
        *root = split;
    else if (oldparent->a == leaf)
        oldparent->a = split;
    else
        oldparent->b = split;

    return split;
}

void layout_remove(layout_node_t **root, layout_node_t *leaf)
{
    layout_node_t *parent = leaf->parent;
    layout_node_t *sibling, *grand;

    if (parent == NULL) {
        *root = NULL;
        free(leaf);
        return;
    }

    sibling = (parent->a == leaf) ? parent->b : parent->a;
    grand = parent->parent;
    sibling->parent = grand;

    if (grand == NULL)
        *root = sibling;
    else if (grand->a == parent)
        grand->a = sibling;
    else
        grand->b = sibling;

    free(leaf);
    free(parent);
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void layout_apply(layout_node_t *node, int x, int y, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    if (node->type == LN_LEAF) {
        node->pane->x = x;
        node->pane->y = y;
        node->pane->cols = cols;
        node->pane->rows = rows;
        pane_apply_geometry(node->pane);
        return;
    }

    if (node->type == LN_SPLIT_V) {
        int usable = cols - 1;           /* one column for the divider */
        int aw, bw;
        if (usable < 2) usable = 2;
        aw = clampi((int)(node->ratio * usable + 0.5), 1, usable - 1);
        bw = usable - aw;
        node->dx = x + aw;
        node->dy = y;
        node->dlen = rows;
        layout_apply(node->a, x, y, aw, rows);
        layout_apply(node->b, x + aw + 1, y, bw, rows);
    } else { /* LN_SPLIT_H */
        int usable = rows - 1;           /* one row for the divider */
        int ah, bh;
        if (usable < 2) usable = 2;
        ah = clampi((int)(node->ratio * usable + 0.5), 1, usable - 1);
        bh = usable - ah;
        node->dx = x;
        node->dy = y + ah;
        node->dlen = cols;
        layout_apply(node->a, x, y, cols, ah);
        layout_apply(node->b, x, y + ah + 1, cols, bh);
    }
}

layout_node_t *layout_first_leaf(layout_node_t *node)
{
    while (node && node->type != LN_LEAF)
        node = node->a;
    return node;
}

/* Leftmost leaf of a subtree. */
static layout_node_t *leftmost(layout_node_t *n)
{
    return layout_first_leaf(n);
}

layout_node_t *layout_next_leaf(layout_node_t *root, layout_node_t *cur)
{
    /* In-order-ish traversal over leaves: go up until `cur` is a left child
     * with a right sibling, then descend into that sibling; wrap to the first. */
    layout_node_t *child = cur;
    layout_node_t *parent = cur->parent;
    while (parent) {
        if (parent->a == child)
            return leftmost(parent->b);
        child = parent;
        parent = parent->parent;
    }
    return leftmost(root); /* wrap around */
}

layout_node_t *layout_find(layout_node_t *node, const pane_t *pane)
{
    layout_node_t *r;
    if (node == NULL)
        return NULL;
    if (node->type == LN_LEAF)
        return node->pane == pane ? node : NULL;
    r = layout_find(node->a, pane);
    return r ? r : layout_find(node->b, pane);
}

int layout_count(const layout_node_t *node)
{
    if (node == NULL)
        return 0;
    if (node->type == LN_LEAF)
        return 1;
    return layout_count(node->a) + layout_count(node->b);
}

/* Collect leaf panes into `out` (up to max). Returns count. */
static int collect_panes(layout_node_t *node, pane_t **out, int max, int n)
{
    if (node == NULL || n >= max)
        return n;
    if (node->type == LN_LEAF) {
        out[n++] = node->pane;
        return n;
    }
    n = collect_panes(node->a, out, max, n);
    n = collect_panes(node->b, out, max, n);
    return n;
}

int layout_collect(layout_node_t *node, pane_t **out, int max)
{
    return collect_panes(node, out, max, 0);
}

pane_t *layout_pane_in_dir(layout_node_t *root, const pane_t *cur, int dir)
{
    pane_t *panes[256];
    int count = collect_panes(root, panes, 256, 0);
    int i;
    pane_t *best = NULL;
    long best_score = 0;

    /* Center of the current pane. */
    int cx = cur->x + cur->cols / 2;
    int cy = cur->y + cur->rows / 2;

    for (i = 0; i < count; i++) {
        pane_t *p = panes[i];
        int px, py;
        long primary, secondary, score;
        if (p == cur)
            continue;
        px = p->x + p->cols / 2;
        py = p->y + p->rows / 2;

        /* Require the candidate to be on the correct side. */
        switch (dir) {
            case DIR_UP:    if (!(py < cy)) continue; primary = cy - py; secondary = labs(px - cx); break;
            case DIR_DOWN:  if (!(py > cy)) continue; primary = py - cy; secondary = labs(px - cx); break;
            case DIR_LEFT:  if (!(px < cx)) continue; primary = cx - px; secondary = labs(py - cy); break;
            case DIR_RIGHT: if (!(px > cx)) continue; primary = px - cx; secondary = labs(py - cy); break;
            default: continue;
        }
        /* Prefer the closest in the primary axis, then the least off-axis. */
        score = primary * 1000 + secondary;
        if (best == NULL || score < best_score) {
            best = p;
            best_score = score;
        }
    }
    return best;
}

/* U+2502 BOX DRAWINGS LIGHT VERTICAL, U+2500 LIGHT HORIZONTAL. */
static const char VBAR[] = "\xe2\x94\x82";
static const char HBAR[] = "\xe2\x94\x80";

void layout_draw_borders(const layout_node_t *node, strbuf_t *out)
{
    if (node == NULL || node->type == LN_LEAF)
        return;

    if (node->type == LN_SPLIT_V) {
        int r;
        for (r = 0; r < node->dlen; r++) {
            strbuf_printf(out, "\x1b[%d;%dH", node->dy + r + 1, node->dx + 1);
            strbuf_append(out, VBAR, 3);
        }
    } else {
        int c;
        strbuf_printf(out, "\x1b[%d;%dH", node->dy + 1, node->dx + 1);
        for (c = 0; c < node->dlen; c++)
            strbuf_append(out, HBAR, 3);
    }
    layout_draw_borders(node->a, out);
    layout_draw_borders(node->b, out);
}

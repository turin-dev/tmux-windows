/* layout.c — see layout.h. */
#include "layout.h"

#include <stdlib.h>
#include <string.h>

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

    node->alloc_cols = cols;
    node->alloc_rows = rows;

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

int layout_resize(layout_node_t *root, const pane_t *active, int dir, int amount)
{
    layout_node_t *leaf = layout_find(root, active);
    layout_node_t *child, *p, *sp = NULL;
    int wantType, wantSideA, sign;
    double total, r, lo, hi;

    if (leaf == NULL || amount == 0)
        return 0;

    switch (dir) {
        case DIR_RIGHT: wantType = LN_SPLIT_V; wantSideA = 1; sign =  1; break;
        case DIR_LEFT:  wantType = LN_SPLIT_V; wantSideA = 0; sign = -1; break;
        case DIR_DOWN:  wantType = LN_SPLIT_H; wantSideA = 1; sign =  1; break;
        case DIR_UP:    wantType = LN_SPLIT_H; wantSideA = 0; sign = -1; break;
        default: return 0;
    }

    /* Walk up to the nearest ancestor split of the right axis whose divider sits
     * on the requested side of the active pane. */
    child = leaf;
    p = leaf->parent;
    while (p) {
        if (p->type == wantType && ((p->a == child) ? 1 : 0) == wantSideA) {
            sp = p;
            break;
        }
        child = p;
        p = p->parent;
    }
    if (sp == NULL)
        return 0;

    total = (double)((wantType == LN_SPLIT_V ? sp->alloc_cols : sp->alloc_rows) - 1);
    if (total < 2.0)
        return 0;

    r = sp->ratio + sign * (double)amount / total;
    lo = 1.0 / total;
    hi = (total - 1.0) / total;
    if (r < lo) r = lo;
    if (r > hi) r = hi;
    if (r == sp->ratio)
        return 0;
    sp->ratio = r;
    return 1;
}

/* Build a balanced left-leaning chain of `type` splits over panes[0..n-1] so
 * each pane receives an equal share. Returns the subtree root, or NULL. */
static layout_node_t *build_even(pane_t **panes, int n, int type)
{
    layout_node_t *node;
    int i;
    if (n <= 0)
        return NULL;
    node = layout_leaf(panes[n - 1]);
    if (node == NULL)
        return NULL;
    for (i = n - 2; i >= 0; i--) {
        layout_node_t *sp = (layout_node_t *)calloc(1, sizeof(*sp));
        layout_node_t *lf = layout_leaf(panes[i]);
        if (sp == NULL || lf == NULL) { free(sp); free(lf); layout_free(node, 0); return NULL; }
        sp->type = type;
        sp->ratio = 1.0 / (double)(n - i);   /* pane i gets 1/(remaining) */
        sp->a = lf;
        sp->b = node;
        lf->parent = sp;
        node->parent = sp;
        node = sp;
    }
    return node;
}

/* Combine subtree nodes into a balanced chain of `type` splits (equal shares). */
static layout_node_t *combine_even(layout_node_t **nodes, int n, int type)
{
    layout_node_t *node;
    int i;
    if (n <= 0)
        return NULL;
    node = nodes[n - 1];
    for (i = n - 2; i >= 0; i--) {
        layout_node_t *sp = (layout_node_t *)calloc(1, sizeof(*sp));
        if (sp == NULL) return NULL;
        sp->type = type;
        sp->ratio = 1.0 / (double)(n - i);
        sp->a = nodes[i];
        sp->b = node;
        nodes[i]->parent = sp;
        node->parent = sp;
        node = sp;
    }
    return node;
}

static layout_node_t *build_main(pane_t **panes, int n, int mainSplit, int restType)
{
    layout_node_t *split, *rest, *mainleaf;
    if (n == 1)
        return layout_leaf(panes[0]);
    mainleaf = layout_leaf(panes[0]);
    rest = build_even(panes + 1, n - 1, restType);
    split = (layout_node_t *)calloc(1, sizeof(*split));
    if (split == NULL || mainleaf == NULL || rest == NULL) {
        free(split); layout_free(mainleaf, 0); layout_free(rest, 0);
        return NULL;
    }
    split->type = mainSplit;
    split->ratio = 0.5;
    split->a = mainleaf;
    split->b = rest;
    mainleaf->parent = split;
    rest->parent = split;
    return split;
}

static layout_node_t *build_tiled(pane_t **panes, int n)
{
    layout_node_t *rows[TMUXW_MAX_PANES];
    int cols = 1, nrows, r, made = 0;
    if (n <= 0)
        return NULL;
    while (cols * cols < n)
        cols++;
    nrows = (n + cols - 1) / cols;
    for (r = 0; r < nrows; r++) {
        int start = r * cols;
        int len = n - start;
        if (len > cols) len = cols;
        rows[made] = build_even(panes + start, len, LN_SPLIT_V);
        if (rows[made] == NULL) {
            int k;
            for (k = 0; k < made; k++) layout_free(rows[k], 0);
            return NULL;
        }
        made++;
    }
    return combine_even(rows, made, LN_SPLIT_H);
}

int layout_set_preset(layout_node_t **root, int preset)
{
    pane_t *panes[TMUXW_MAX_PANES];
    int n;
    layout_node_t *built = NULL;

    if (root == NULL || *root == NULL)
        return 0;
    n = layout_collect(*root, panes, TMUXW_MAX_PANES);
    if (n <= 0)
        return 0;

    switch (preset) {
        case LAYOUT_EVEN_H: built = build_even(panes, n, LN_SPLIT_V); break;
        case LAYOUT_EVEN_V: built = build_even(panes, n, LN_SPLIT_H); break;
        case LAYOUT_MAIN_H: built = build_main(panes, n, LN_SPLIT_H, LN_SPLIT_V); break;
        case LAYOUT_MAIN_V: built = build_main(panes, n, LN_SPLIT_V, LN_SPLIT_H); break;
        case LAYOUT_TILED:  built = build_tiled(panes, n); break;
        default: return 0;
    }
    if (built == NULL)
        return 0;

    layout_free(*root, 0);   /* free nodes only; panes are reused in `built` */
    built->parent = NULL;
    *root = built;
    return 1;
}

/* Collect leaf nodes (not panes) in traversal order. Returns count. */
static int collect_leaf_nodes(layout_node_t *node, layout_node_t **out, int max, int n)
{
    if (node == NULL || n >= max)
        return n;
    if (node->type == LN_LEAF) {
        out[n++] = node;
        return n;
    }
    n = collect_leaf_nodes(node->a, out, max, n);
    n = collect_leaf_nodes(node->b, out, max, n);
    return n;
}

void layout_rotate(layout_node_t *root, int downward)
{
    layout_node_t *lv[TMUXW_MAX_PANES];
    int n = collect_leaf_nodes(root, lv, TMUXW_MAX_PANES, 0), i;
    if (n < 2)
        return;
    if (downward) {
        pane_t *last = lv[n - 1]->pane;
        for (i = n - 1; i > 0; i--) lv[i]->pane = lv[i - 1]->pane;
        lv[0]->pane = last;
    } else {
        pane_t *first = lv[0]->pane;
        for (i = 0; i < n - 1; i++) lv[i]->pane = lv[i + 1]->pane;
        lv[n - 1]->pane = first;
    }
}

int layout_swap(layout_node_t *root, const pane_t *active, int next)
{
    layout_node_t *lv[TMUXW_MAX_PANES];
    int n = collect_leaf_nodes(root, lv, TMUXW_MAX_PANES, 0), i, idx = -1, t;
    pane_t *tmp;
    if (n < 2)
        return 0;
    for (i = 0; i < n; i++)
        if (lv[i]->pane == active) { idx = i; break; }
    if (idx < 0)
        return 0;
    t = next ? (idx + 1) % n : (idx - 1 + n) % n;
    tmp = lv[idx]->pane;
    lv[idx]->pane = lv[t]->pane;
    lv[t]->pane = tmp;
    return 1;
}

static const char *const PRESET_NAMES[LAYOUT_COUNT] = {
    "even-horizontal", "even-vertical", "main-horizontal", "main-vertical", "tiled"
};

const char *layout_preset_name(int preset)
{
    if (preset < 0 || preset >= LAYOUT_COUNT)
        return "";
    return PRESET_NAMES[preset];
}

int layout_preset_from_name(const char *name)
{
    int i;
    if (name == NULL)
        return -1;
    for (i = 0; i < LAYOUT_COUNT; i++)
        if (strcmp(name, PRESET_NAMES[i]) == 0)
            return i;
    return -1;
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

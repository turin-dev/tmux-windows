/* test_layout.c — headless tests for the pane layout tree.
 *
 * Panes are constructed directly (no ConPTY) so geometry, traversal, splitting,
 * removal, and directional selection can be checked without a real terminal.
 */
#include "layout.h"
#include "model/pane.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }   \
    else         { printf("ok:   %s\n", (msg)); }               \
} while (0)

static pane_t *make_pane(int id)
{
    pane_t *p = (pane_t *)calloc(1, sizeof(*p));
    p->id = id;
    InitializeCriticalSection(&p->lock);
    strbuf_init(&p->pending);
    p->screen = screen_new(1, 1);
    p->has_conpty = 0;
    return p;
}

static void free_pane(pane_t *p)
{
    if (!p) return;
    if (p->screen) screen_free(p->screen);
    strbuf_free(&p->pending);
    DeleteCriticalSection(&p->lock);
    free(p);
}

static void test_single_leaf(void)
{
    pane_t *p = make_pane(1);
    layout_node_t *root = layout_leaf(p);

    layout_apply(root, 0, 0, 80, 24);
    CHECK(p->x == 0 && p->y == 0 && p->cols == 80 && p->rows == 24,
          "single leaf fills window");
    CHECK(layout_count(root) == 1, "count == 1");
    CHECK(layout_first_leaf(root) == root, "first leaf is root");

    layout_free(root, 0);
    free_pane(p);
}

static void test_vsplit(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2);
    layout_node_t *root = layout_leaf(a);
    layout_node_t *leaf_a = root;
    layout_node_t *split;

    split = layout_split(&root, leaf_a, LN_SPLIT_V, b);
    CHECK(root == split, "root is the split after vsplit");
    CHECK(layout_count(root) == 2, "count == 2 after vsplit");

    layout_apply(root, 0, 0, 80, 24);
    /* usable = 79, aw = 40, bw = 39, divider at col 40. */
    CHECK(a->cols == 40 && a->x == 0, "left pane width/x");
    CHECK(b->cols == 39 && b->x == 41, "right pane width/x");
    CHECK(a->rows == 24 && b->rows == 24, "both full height");
    CHECK(split->dx == 40 && split->dlen == 24, "vertical divider position");
    CHECK(a->cols + 1 + b->cols == 80, "widths + divider == total");

    layout_free(root, 0);
    free_pane(a); free_pane(b);
}

static void test_hsplit(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2);
    layout_node_t *root = layout_leaf(a);
    layout_node_t *split = layout_split(&root, root, LN_SPLIT_H, b);

    layout_apply(root, 0, 0, 80, 24);
    /* usable = 23, ah = 12, bh = 11, divider at row 12. */
    CHECK(a->rows == 12 && a->y == 0, "top pane height/y");
    CHECK(b->rows == 11 && b->y == 13, "bottom pane height/y");
    CHECK(a->cols == 80 && b->cols == 80, "both full width");
    CHECK(split->dy == 12 && split->dlen == 80, "horizontal divider position");

    layout_free(root, 0);
    free_pane(a); free_pane(b);
}

static void test_traversal(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2), *c = make_pane(3);
    layout_node_t *root = layout_leaf(a);
    layout_node_t *la = root, *lb, *lc;

    lb = layout_split(&root, la, LN_SPLIT_V, b)->b; /* a | b */
    /* Split b again horizontally: a | (b / c) */
    lc = layout_split(&root, lb, LN_SPLIT_H, c)->b;
    (void)lc;

    CHECK(layout_count(root) == 3, "count == 3");

    {
        layout_node_t *n = layout_first_leaf(root);
        pane_t *seen[3];
        int i;
        for (i = 0; i < 3; i++) {
            seen[i] = n->pane;
            n = layout_next_leaf(root, n);
        }
        CHECK(seen[0] == a, "traversal[0] == a");
        CHECK(n->pane == seen[0], "traversal wraps to start");
        CHECK((seen[0] != seen[1]) && (seen[1] != seen[2]) && (seen[0] != seen[2]),
              "traversal visits distinct panes");
    }

    layout_free(root, 0);
    free_pane(a); free_pane(b); free_pane(c);
}

static void test_directional(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2);
    layout_node_t *root = layout_leaf(a);
    layout_split(&root, root, LN_SPLIT_V, b);   /* a | b */
    layout_apply(root, 0, 0, 80, 24);

    CHECK(layout_pane_in_dir(root, a, DIR_RIGHT) == b, "right of left pane is b");
    CHECK(layout_pane_in_dir(root, b, DIR_LEFT) == a, "left of right pane is a");
    CHECK(layout_pane_in_dir(root, a, DIR_LEFT) == NULL, "nothing left of a");
    CHECK(layout_pane_in_dir(root, a, DIR_UP) == NULL, "nothing above a");

    layout_free(root, 0);
    free_pane(a); free_pane(b);
}

static void test_remove(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2);
    layout_node_t *root = layout_leaf(a);
    layout_node_t *leaf_b = layout_split(&root, root, LN_SPLIT_V, b)->b;

    layout_remove(&root, leaf_b);   /* remove b; a should take over */
    CHECK(layout_count(root) == 1, "count == 1 after remove");
    CHECK(root->type == LN_LEAF && root->pane == a, "a is sole root leaf");

    layout_apply(root, 0, 0, 80, 24);
    CHECK(a->cols == 80 && a->rows == 24, "a fills window after remove");

    /* Removing the last leaf empties the tree. */
    {
        layout_node_t *leaf_a = root;
        layout_remove(&root, leaf_a);
        CHECK(root == NULL, "tree empty after removing last leaf");
    }

    free_pane(a); free_pane(b);
}

static void test_resize(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2);
    layout_node_t *root = layout_leaf(a);
    layout_split(&root, root, LN_SPLIT_V, b);   /* a | b */
    layout_apply(root, 0, 0, 80, 24);
    CHECK(a->cols == 40, "resize: initial left width 40");

    /* Grow the left pane rightward by 5 cells. */
    CHECK(layout_resize(root, a, DIR_RIGHT, 5) == 1, "resize -R reports change");
    layout_apply(root, 0, 0, 80, 24);
    CHECK(a->cols == 45, "resize -R grows left pane to 45");
    CHECK(a->cols + 1 + b->cols == 80, "resize keeps widths consistent");

    /* The leftmost pane cannot grow further left (no divider there). */
    CHECK(layout_resize(root, a, DIR_LEFT, 5) == 0, "resize -L on leftmost is a no-op");
    /* Growing b leftward shrinks a. */
    CHECK(layout_resize(root, b, DIR_LEFT, 5) == 1, "resize -L on right pane changes");
    layout_apply(root, 0, 0, 80, 24);
    CHECK(a->cols == 40, "b grew left, a back to 40");

    layout_free(root, 0);
    free_pane(a); free_pane(b);
}

static void test_presets(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2), *c = make_pane(3), *d = make_pane(4);
    layout_node_t *root = layout_leaf(a);
    layout_node_t *lb, *lc;
    pane_t *ps[8];
    int n, i;

    lb = layout_split(&root, root, LN_SPLIT_V, b)->b;
    lc = layout_split(&root, lb, LN_SPLIT_V, c)->b;
    layout_split(&root, lc, LN_SPLIT_H, d);
    CHECK(layout_count(root) == 4, "presets: 4 panes to start");

    /* even-horizontal: one row, every pane full height. */
    CHECK(layout_set_preset(&root, LAYOUT_EVEN_H) == 1, "set even-horizontal");
    layout_apply(root, 0, 0, 80, 24);
    n = layout_collect(root, ps, 8);
    CHECK(n == 4, "even-h keeps 4 panes");
    { int ok = 1; for (i = 0; i < n; i++) if (ps[i]->rows != 24) ok = 0;
      CHECK(ok, "even-h: all panes full height"); }

    /* even-vertical: one column, every pane full width. */
    CHECK(layout_set_preset(&root, LAYOUT_EVEN_V) == 1, "set even-vertical");
    layout_apply(root, 0, 0, 80, 24);
    n = layout_collect(root, ps, 8);
    { int ok = 1; for (i = 0; i < n; i++) if (ps[i]->cols != 80) ok = 0;
      CHECK(ok, "even-v: all panes full width"); }

    /* main-vertical: first pane on the left, full height. */
    CHECK(layout_set_preset(&root, LAYOUT_MAIN_V) == 1, "set main-vertical");
    layout_apply(root, 0, 0, 80, 24);
    n = layout_collect(root, ps, 8);
    CHECK(ps[0]->x == 0 && ps[0]->rows == 24, "main-v: main pane spans left column");
    CHECK(ps[1]->x == ps[2]->x && ps[2]->x == ps[3]->x && ps[1]->x > 0,
          "main-v: the rest share the right column");

    /* tiled: still all four panes, no crash. */
    CHECK(layout_set_preset(&root, LAYOUT_TILED) == 1, "set tiled");
    layout_apply(root, 0, 0, 80, 24);
    CHECK(layout_count(root) == 4, "tiled keeps 4 panes");

    CHECK(layout_preset_from_name("tiled") == LAYOUT_TILED, "preset name -> id");
    CHECK(layout_preset_from_name("bogus") == -1, "unknown preset name -> -1");

    layout_free(root, 0);
    free_pane(a); free_pane(b); free_pane(c); free_pane(d);
}

static void test_rotate_swap(void)
{
    pane_t *a = make_pane(1), *b = make_pane(2), *c = make_pane(3);
    layout_node_t *root = layout_leaf(a);
    layout_node_t *lb;
    pane_t *p[4];

    lb = layout_split(&root, root, LN_SPLIT_V, b)->b;   /* a | b */
    layout_split(&root, lb, LN_SPLIT_V, c);             /* a | b | c */

    /* Leaves hold a, b, c in order. Rotate downward: c, a, b. */
    layout_rotate(root, 1);
    layout_collect(root, p, 4);
    CHECK(p[0] == c && p[1] == a && p[2] == b,
          "rotate downward shifts panes forward");

    /* Rotate upward returns to a, b, c. */
    layout_rotate(root, 0);
    layout_collect(root, p, 4);
    CHECK(p[0] == a && p[1] == b && p[2] == c,
          "rotate upward restores order");

    /* Swap a with its next neighbor (b): b, a, c. */
    CHECK(layout_swap(root, a, 1) == 1, "swap reports change");
    layout_collect(root, p, 4);
    CHECK(p[0] == b && p[1] == a && p[2] == c,
          "swap next exchanges a and b");

    layout_free(root, 0);
    free_pane(a); free_pane(b); free_pane(c);
}

int main(void)
{
    test_single_leaf();
    test_vsplit();
    test_hsplit();
    test_traversal();
    test_directional();
    test_remove();
    test_resize();
    test_presets();
    test_rotate_swap();

    if (failures == 0) { printf("\nALL PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}

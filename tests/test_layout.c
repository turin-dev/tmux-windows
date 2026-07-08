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

int main(void)
{
    test_single_leaf();
    test_vsplit();
    test_hsplit();
    test_traversal();
    test_directional();
    test_remove();

    if (failures == 0) { printf("\nALL PASSED\n"); return 0; }
    printf("\n%d FAILURE(S)\n", failures);
    return 1;
}

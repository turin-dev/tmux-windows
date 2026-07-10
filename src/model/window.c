/* window.c — see window.h. */
#include "model/window.h"

#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Derive a short display name from a shell command line: the first token, with
 * any path and ".exe" stripped (e.g. "C:\\...\\pwsh.exe -nol" -> "pwsh"). */
static void derive_name(char *out, size_t cap, const wchar_t *shell)
{
    char narrow[256];
    int i, start = 0, end;
    WideCharToMultiByte(CP_UTF8, 0, shell, -1, narrow, sizeof(narrow), NULL, NULL);

    /* Cut at the first space (drop arguments). */
    for (end = 0; narrow[end] && narrow[end] != ' '; end++)
        ;
    /* Basename: last path separator before `end`. */
    for (i = 0; i < end; i++)
        if (narrow[i] == '\\' || narrow[i] == '/')
            start = i + 1;
    /* Strip a trailing ".exe". */
    if (end - start > 4 && _stricmp(narrow + end - 4, ".exe") == 0)
        end -= 4;

    {
        int n = end - start;
        if (n > (int)cap - 1) n = (int)cap - 1;
        if (n < 0) n = 0;
        memcpy(out, narrow + start, (size_t)n);
        out[n] = '\0';
    }
}

window_t *window_create(const wchar_t *shell, int cols, int rows, HANDLE wake,
                        const wchar_t *cwd, const wchar_t *envblock)
{
    window_t *w;
    pane_t *first;
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 24;

    w = (window_t *)calloc(1, sizeof(*w));
    if (w == NULL)
        return NULL;
    w->cols = cols;
    w->rows = rows;
    w->next_pane_id = 1;
    derive_name(w->name, sizeof(w->name), shell);

    first = pane_create(w->next_pane_id++, shell, cols, rows, wake, cwd, envblock);
    if (first == NULL) {
        free(w);
        return NULL;
    }
    w->root = layout_leaf(first);
    w->active = first;
    layout_apply(w->root, 0, 0, cols, rows);
    return w;
}

void window_free(window_t *w)
{
    if (w == NULL)
        return;
    if (w->root)
        layout_free(w->root, 1);
    free(w);
}

window_t *window_create_with_pane(pane_t *p, int cols, int rows, const char *name)
{
    window_t *w;
    if (p == NULL)
        return NULL;
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 24;
    w = (window_t *)calloc(1, sizeof(*w));
    if (w == NULL)
        return NULL;
    w->cols = cols;
    w->rows = rows;
    w->next_pane_id = 2;
    strncpy_s(w->name, sizeof(w->name), (name && name[0]) ? name : "win", _TRUNCATE);
    w->root = layout_leaf(p);
    if (w->root == NULL) { free(w); return NULL; }
    w->active = p;
    layout_apply(w->root, 0, 0, cols, rows);
    return w;
}

pane_t *window_extract_active(window_t *w)
{
    pane_t *victim = w->active;
    layout_node_t *leaf;
    if (w->root == NULL || victim == NULL || layout_count(w->root) < 2)
        return NULL;                       /* the sole pane: nothing to break out */
    leaf = layout_find(w->root, victim);
    if (leaf == NULL)
        return NULL;
    layout_remove(&w->root, leaf);         /* frees the node, keeps the pane */
    w->zoomed = 0;
    w->drag = NULL;
    if (w->last_active == victim)
        w->last_active = NULL;
    w->active = layout_first_leaf(w->root)->pane;
    layout_apply(w->root, 0, 0, w->cols, w->rows);
    return victim;
}

void window_apply(window_t *w, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    w->cols = cols;
    w->rows = rows;
    if (w->root == NULL)
        return;
    if (w->zoomed && w->active) {
        /* Zoomed: the active pane owns the whole area; others keep their size
         * (they are hidden) and are restored on unzoom. */
        w->active->x = 0;
        w->active->y = 0;
        w->active->cols = cols;
        w->active->rows = rows;
        pane_apply_geometry(w->active);
        return;
    }
    layout_apply(w->root, 0, 0, cols, rows);
}

/* Split the active leaf, placing existing pane `np` as the new child. */
static int split_with_pane(window_t *w, pane_t *np, int type)
{
    layout_node_t *leaf = layout_find(w->root, w->active);
    if (leaf == NULL || layout_count(w->root) >= TMUXW_MAX_PANES)
        return 0;
    if (layout_split(&w->root, leaf, type, np) == NULL)
        return 0;
    w->zoomed = 0;         /* splitting always reveals the full layout */
    w->drag = NULL;
    w->last_active = w->active;
    w->active = np;
    layout_apply(w->root, 0, 0, w->cols, w->rows);
    return 1;
}

void window_split(window_t *w, int type, const wchar_t *shell, HANDLE wake,
                  const wchar_t *envblock)
{
    pane_t *np;
    if (layout_find(w->root, w->active) == NULL || layout_count(w->root) >= TMUXW_MAX_PANES)
        return;
    np = pane_create(w->next_pane_id++, shell, w->active->cols, w->active->rows, wake, NULL,
                     envblock);
    if (np == NULL)
        return;
    if (!split_with_pane(w, np, type))
        pane_close(np);
}

pane_t *window_detach_active(window_t *w)
{
    pane_t *victim = w->active;
    layout_node_t *leaf;
    if (w->root == NULL || victim == NULL)
        return NULL;
    leaf = layout_find(w->root, victim);
    if (leaf == NULL)
        return NULL;
    layout_remove(&w->root, leaf);
    w->zoomed = 0;
    w->drag = NULL;
    if (w->last_active == victim)
        w->last_active = NULL;
    w->active = (w->root != NULL) ? layout_first_leaf(w->root)->pane : NULL;
    if (w->root != NULL)
        layout_apply(w->root, 0, 0, w->cols, w->rows);
    return victim;
}

int window_insert_pane(window_t *w, pane_t *p, int type)
{
    if (p == NULL)
        return 0;
    if (w->root == NULL) {                 /* empty window: p becomes the sole pane */
        w->root = layout_leaf(p);
        if (w->root == NULL) return 0;
        w->active = p;
        layout_apply(w->root, 0, 0, w->cols, w->rows);
        return 1;
    }
    return split_with_pane(w, p, type);
}

void window_resize_active(window_t *w, int dir, int amount)
{
    if (w->zoomed || w->root == NULL || w->active == NULL)
        return;
    if (layout_resize(w->root, w->active, dir, amount))
        layout_apply(w->root, 0, 0, w->cols, w->rows);
}

void window_set_layout(window_t *w, int preset)
{
    if (w->root == NULL)
        return;
    w->zoomed = 0;
    w->drag = NULL;
    if (layout_set_preset(&w->root, preset)) {
        w->layout = preset;
        layout_apply(w->root, 0, 0, w->cols, w->rows);
    }
}

void window_next_layout(window_t *w)
{
    window_set_layout(w, (w->layout + 1) % LAYOUT_COUNT);
}

void window_previous_layout(window_t *w)
{
    window_set_layout(w, (w->layout + LAYOUT_COUNT - 1) % LAYOUT_COUNT);
}

void window_toggle_zoom(window_t *w)
{
    if (w->root == NULL || w->active == NULL)
        return;
    if (layout_count(w->root) < 2) {   /* nothing to zoom over */
        w->zoomed = 0;
        return;
    }
    w->zoomed = !w->zoomed;
    window_apply(w, w->cols, w->rows);
}

int window_is_zoomed(const window_t *w)
{
    return w->zoomed;
}

/* Switch the active pane, remembering the previous one for last-pane. */
static void set_active(window_t *w, pane_t *p)
{
    if (p && p != w->active) {
        w->last_active = w->active;
        w->active = p;
    }
}

void window_select_next_pane(window_t *w)
{
    layout_node_t *leaf = layout_find(w->root, w->active);
    layout_node_t *next;
    if (leaf == NULL)
        return;
    next = layout_next_leaf(w->root, leaf);
    if (next)
        set_active(w, next->pane);
}

void window_select_dir(window_t *w, int dir)
{
    pane_t *t = layout_pane_in_dir(w->root, w->active, dir);
    if (t)
        set_active(w, t);
}

pane_t *window_pane_at(window_t *w, int x, int y)
{
    if (w->root == NULL)
        return NULL;
    if (w->zoomed)
        return w->active;   /* only the active pane is visible */
    return layout_pane_at(w->root, x, y);
}

void window_select_pane(window_t *w, pane_t *p)
{
    if (p && layout_find(w->root, p))
        set_active(w, p);
}

void window_select_index(window_t *w, int n)
{
    pane_t *ps[TMUXW_MAX_PANES];
    int c = layout_collect(w->root, ps, TMUXW_MAX_PANES);
    if (n >= 0 && n < c)
        set_active(w, ps[n]);
}

int window_mouse_press(window_t *w, int x, int y)
{
    int vert;
    layout_node_t *div;
    if (w->root == NULL || w->zoomed)
        return 0;
    div = layout_divider_at(w->root, x, y, &vert);
    if (div) {
        w->drag = div;      /* begin dragging this divider */
        return 1;
    }
    {
        pane_t *p = layout_pane_at(w->root, x, y);
        if (p) set_active(w, p);
    }
    return 0;
}

void window_mouse_drag(window_t *w, int x, int y)
{
    if (w->drag && layout_set_divider(w->drag, x, y))
        layout_apply(w->root, 0, 0, w->cols, w->rows);
}

void window_mouse_release(window_t *w)
{
    w->drag = NULL;
}

void window_rotate(window_t *w, int downward)
{
    if (w->root == NULL || layout_count(w->root) < 2)
        return;
    w->zoomed = 0;
    layout_rotate(w->root, downward);
    layout_apply(w->root, 0, 0, w->cols, w->rows);
}

void window_swap_active(window_t *w, int next)
{
    if (w->root == NULL || w->active == NULL)
        return;
    w->zoomed = 0;
    if (layout_swap(w->root, w->active, next))
        layout_apply(w->root, 0, 0, w->cols, w->rows);
}

void window_select_last(window_t *w)
{
    if (w->last_active && layout_find(w->root, w->last_active))
        set_active(w, w->last_active);
}

pane_t *window_active(window_t *w)
{
    return w->active;
}

int window_kill_active(window_t *w)
{
    layout_node_t *leaf = layout_find(w->root, w->active);
    pane_t *victim = w->active;
    if (leaf == NULL)
        return window_empty(w);
    layout_remove(&w->root, leaf);
    pane_close(victim);
    w->zoomed = 0;
    w->drag = NULL;
    if (w->last_active == victim)
        w->last_active = NULL;
    if (w->root == NULL) {
        w->active = NULL;
        return 1;
    }
    w->active = layout_first_leaf(w->root)->pane;
    layout_apply(w->root, 0, 0, w->cols, w->rows);
    return 0;
}

void window_kill_others(window_t *w)
{
    pane_t *keep = w->active, *ps[TMUXW_MAX_PANES];
    int n, i;
    if (w->root == NULL || keep == NULL)
        return;
    n = layout_collect(w->root, ps, TMUXW_MAX_PANES);
    if (n <= 1)
        return;
    for (i = 0; i < n; i++)
        if (ps[i] != keep) pane_close(ps[i]);
    layout_free(w->root, 0);            /* free nodes; panes already closed/kept */
    w->root = layout_leaf(keep);
    w->active = keep;
    w->last_active = NULL;
    w->zoomed = 0;
    w->drag = NULL;
    if (w->root)
        layout_apply(w->root, 0, 0, w->cols, w->rows);
}

int window_respawn_active(window_t *w)
{
    if (w->active == NULL)
        return -1;
    return pane_respawn(w->active);
}

void window_respawn_all(window_t *w)
{
    pane_t *ps[TMUXW_MAX_PANES];
    int n, i;
    if (w->root == NULL)
        return;
    n = layout_collect(w->root, ps, TMUXW_MAX_PANES);
    for (i = 0; i < n; i++)
        pane_respawn(ps[i]);
}

void window_write_active(window_t *w, const char *bytes, size_t n)
{
    if (w->active)
        pane_write_input(w->active, bytes, n);
}

size_t window_pump_ex(window_t *w, int *out_died)
{
    pane_t *leaves[TMUXW_MAX_PANES];
    int count, i, removed = 0;
    size_t parsed = 0;

    if (out_died) *out_died = 0;

    count = layout_collect(w->root, leaves, TMUXW_MAX_PANES);
    for (i = 0; i < count; i++)
        parsed += pane_pump(leaves[i]);

    /* Reap any exited children. */
    count = layout_collect(w->root, leaves, TMUXW_MAX_PANES);
    for (i = 0; i < count; i++) {
        pane_t *p = leaves[i];
        if (!pane_child_exited(p))
            continue;
        pane_pump(p);
        {
            layout_node_t *leaf = layout_find(w->root, p);
            if (leaf) layout_remove(&w->root, leaf);
        }
        if (w->active == p)
            w->active = NULL;
        if (w->last_active == p)
            w->last_active = NULL;
        pane_close(p);
        removed = 1;
        if (out_died) *out_died = 1;
        if (w->root == NULL) {
            w->active = NULL;
            return parsed;
        }
    }
    if (removed) {
        w->zoomed = 0;
        w->drag = NULL;
        if (w->active == NULL && w->root)
            w->active = layout_first_leaf(w->root)->pane;
        layout_apply(w->root, 0, 0, w->cols, w->rows);
    }
    return parsed;
}

size_t window_pump(window_t *w)
{
    return window_pump_ex(w, NULL);
}

int window_empty(const window_t *w)
{
    return w->root == NULL;
}

void window_render(strbuf_t *frame, window_t *w, int full_redraw,
                   const copymode_t *cm)
{
    pane_t *leaves[TMUXW_MAX_PANES];
    int count, i;
    int copy_on_active = (cm && cm->active && cm->pane == w->active);

    if (w->root == NULL)
        return;

    /* Zoomed: draw only the active pane, filling the window (no dividers). */
    if (w->zoomed && w->active) {
        if (full_redraw)
            screen_mark_all_dirty(w->active->screen);
        if (copy_on_active)
            render_pane_copymode(frame, w->active, cm);
        else {
            render_pane(frame, w->active);
            render_active_cursor(frame, w->active);
        }
        return;
    }

    if (full_redraw) {
        layout_draw_borders(w->root, frame);
        count = layout_collect(w->root, leaves, TMUXW_MAX_PANES);
        for (i = 0; i < count; i++)
            screen_mark_all_dirty(leaves[i]->screen);
    }

    count = layout_collect(w->root, leaves, TMUXW_MAX_PANES);
    for (i = 0; i < count; i++) {
        if (copy_on_active && leaves[i] == w->active)
            render_pane_copymode(frame, leaves[i], cm);
        else
            render_pane(frame, leaves[i]);
    }

    /* In copy mode the copy renderer already placed the cursor. */
    if (!copy_on_active)
        render_active_cursor(frame, w->active);
}

void window_display_panes(strbuf_t *frame, window_t *w, int base)
{
    pane_t *leaves[TMUXW_MAX_PANES];
    int count, i;
    if (w->root == NULL)
        return;
    count = layout_collect(w->root, leaves, TMUXW_MAX_PANES);
    for (i = 0; i < count; i++) {
        pane_t *p = leaves[i];
        char label[16];
        int len, row, col;
        _snprintf_s(label, sizeof(label), _TRUNCATE, " %d ", i + base);
        len = (int)strlen(label);
        row = p->y + p->rows / 2;
        col = p->x + (p->cols - len) / 2;
        if (col < p->x) col = p->x;
        /* Bright reverse-video number, emphasised on the active pane. */
        strbuf_printf(frame, "\x1b[%d;%dH\x1b[7m%s%s\x1b[0m",
                      row + 1, col + 1, (p == w->active) ? "\x1b[1m" : "", label);
    }
}

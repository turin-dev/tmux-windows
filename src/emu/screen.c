/* screen.c — see screen.h. */
#include "emu/screen.h"

#include <stdlib.h>
#include <string.h>

#define SB_CAP 2000   /* scrollback lines retained per pane */

typedef struct sbline {
    int              cols;
    VTermScreenCell *cells;
} sbline_t;

struct screen {
    VTerm        *vt;
    VTermScreen  *vts;
    VTermState   *vstate;
    int           cols;
    int           rows;
    int           cur_row;
    int           cur_col;
    int           cur_visible;
    int           dirty_cursor;  /* cursor moved / visibility changed */
    unsigned char *dirty;   /* one flag per row */
    /* Scrollback ring: lines that scrolled off the top. */
    sbline_t     *sb;
    int           sb_cap;
    int           sb_len;
    int           sb_head;  /* index of the oldest retained line */
};

static void mark_rows(screen_t *s, int start_row, int end_row)
{
    int r;
    if (start_row < 0) start_row = 0;
    if (end_row > s->rows) end_row = s->rows;
    for (r = start_row; r < end_row; r++)
        s->dirty[r] = 1;
}

static int cb_damage(VTermRect rect, void *user)
{
    screen_t *s = (screen_t *)user;
    mark_rows(s, rect.start_row, rect.end_row);
    return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
    screen_t *s = (screen_t *)user;
    (void)src;
    mark_rows(s, dest.start_row, dest.end_row);
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    screen_t *s = (screen_t *)user;
    (void)oldpos;
    s->cur_row = pos.row;
    s->cur_col = pos.col;
    s->cur_visible = visible;
    s->dirty_cursor = 1;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    screen_t *s = (screen_t *)user;
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        s->cur_visible = val->boolean;
        s->dirty_cursor = 1;
    }
    return 1;
}

/* A line has scrolled off the top: retain a copy in the ring buffer. */
static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    screen_t *s = (screen_t *)user;
    sbline_t line;
    int idx;

    if (s->sb == NULL || s->sb_cap <= 0)
        return 0;

    line.cols = cols;
    line.cells = (VTermScreenCell *)malloc((size_t)cols * sizeof(VTermScreenCell));
    if (line.cells == NULL)
        return 0;
    memcpy(line.cells, cells, (size_t)cols * sizeof(VTermScreenCell));

    if (s->sb_len < s->sb_cap) {
        idx = (s->sb_head + s->sb_len) % s->sb_cap;
        s->sb[idx] = line;
        s->sb_len++;
    } else {
        /* Full: evict the oldest line. */
        free(s->sb[s->sb_head].cells);
        s->sb[s->sb_head] = line;
        s->sb_head = (s->sb_head + 1) % s->sb_cap;
    }
    return 1;
}

/* libvterm wants the most recent scrollback line back (e.g. scroll-down). */
static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    screen_t *s = (screen_t *)user;
    sbline_t *line;
    int idx, i, n;

    if (s->sb == NULL || s->sb_len == 0)
        return 0;

    idx = (s->sb_head + s->sb_len - 1) % s->sb_cap;
    line = &s->sb[idx];
    n = (cols < line->cols) ? cols : line->cols;
    for (i = 0; i < n; i++)
        cells[i] = line->cells[i];
    for (; i < cols; i++) {
        memset(&cells[i], 0, sizeof(cells[i]));
        cells[i].width = 1;
    }
    free(line->cells);
    s->sb_len--;
    return 1;
}

static void sb_clear(screen_t *s)
{
    int i;
    if (s->sb == NULL)
        return;
    for (i = 0; i < s->sb_len; i++)
        free(s->sb[(s->sb_head + i) % s->sb_cap].cells);
    s->sb_len = 0;
    s->sb_head = 0;
}

static int cb_sb_clear(void *user)
{
    sb_clear((screen_t *)user);
    return 1;
}

static const VTermScreenCallbacks SCREEN_CBS = {
    .damage      = cb_damage,
    .moverect    = cb_moverect,
    .movecursor  = cb_movecursor,
    .settermprop = cb_settermprop,
    .sb_pushline = cb_sb_pushline,
    .sb_popline  = cb_sb_popline,
    .sb_clear    = cb_sb_clear,
};

screen_t *screen_new(int cols, int rows)
{
    screen_t *s;
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 25;

    s = (screen_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;

    s->cols = cols;
    s->rows = rows;
    s->cur_visible = 1;
    s->dirty = (unsigned char *)calloc((size_t)rows, 1);
    s->sb_cap = SB_CAP;
    s->sb = (sbline_t *)calloc((size_t)s->sb_cap, sizeof(sbline_t));
    if (s->dirty == NULL || s->sb == NULL) {
        free(s->dirty);
        free(s->sb);
        free(s);
        return NULL;
    }

    /* Note: libvterm takes (rows, cols) in this order. */
    s->vt = vterm_new(rows, cols);
    if (s->vt == NULL) {
        free(s->dirty);
        free(s->sb);
        free(s);
        return NULL;
    }
    vterm_set_utf8(s->vt, 1);
    s->vts = vterm_obtain_screen(s->vt);
    s->vstate = vterm_obtain_state(s->vt);
    vterm_screen_set_callbacks(s->vts, &SCREEN_CBS, s);
    vterm_screen_reset(s->vts, 1);
    mark_rows(s, 0, s->rows);
    return s;
}

void screen_free(screen_t *s)
{
    if (s == NULL)
        return;
    if (s->vt)
        vterm_free(s->vt);
    sb_clear(s);
    free(s->sb);
    free(s->dirty);
    free(s);
}

void screen_clear_history(screen_t *s)
{
    if (s != NULL)
        sb_clear(s);
}

void screen_write(screen_t *s, const char *bytes, size_t len)
{
    vterm_input_write(s->vt, bytes, len);
    vterm_screen_flush_damage(s->vts);
}

void screen_resize(screen_t *s, int cols, int rows)
{
    unsigned char *d;
    if (cols <= 0) cols = 1;
    if (rows <= 0) rows = 1;
    if (cols == s->cols && rows == s->rows)
        return;

    d = (unsigned char *)calloc((size_t)rows, 1);
    if (d == NULL)
        return;
    free(s->dirty);
    s->dirty = d;
    s->cols = cols;
    s->rows = rows;

    vterm_set_size(s->vt, rows, cols);
    vterm_screen_flush_damage(s->vts);
    mark_rows(s, 0, rows);
}

int screen_cols(const screen_t *s) { return s->cols; }
int screen_rows(const screen_t *s) { return s->rows; }

void screen_cursor(const screen_t *s, int *row, int *col, int *visible)
{
    if (row) *row = s->cur_row;
    if (col) *col = s->cur_col;
    if (visible) *visible = s->cur_visible;
}

int screen_get_cell(const screen_t *s, int row, int col, VTermScreenCell *cell)
{
    VTermPos pos;
    if (row < 0 || row >= s->rows || col < 0 || col >= s->cols)
        return 0;
    pos.row = row;
    pos.col = col;
    return vterm_screen_get_cell(s->vts, pos, cell);
}

void screen_color_rgb(const screen_t *s, VTermColor *col)
{
    vterm_state_convert_color_to_rgb(s->vstate, col);
}

int screen_row_dirty(const screen_t *s, int row)
{
    if (row < 0 || row >= s->rows)
        return 0;
    return s->dirty[row];
}

int screen_has_dirty(const screen_t *s)
{
    int r;
    if (s->dirty_cursor)
        return 1;
    for (r = 0; r < s->rows; r++)
        if (s->dirty[r])
            return 1;
    return 0;
}

void screen_mark_all_dirty(screen_t *s)
{
    mark_rows(s, 0, s->rows);
    s->dirty_cursor = 1;
}

void screen_clear_dirty(screen_t *s)
{
    memset(s->dirty, 0, (size_t)s->rows);
    s->dirty_cursor = 0;
}

int screen_sb_count(const screen_t *s)
{
    return s->sb_len;
}

int screen_total_lines(const screen_t *s)
{
    return s->sb_len + s->rows;
}

int screen_line_cell(const screen_t *s, int absrow, int col, VTermScreenCell *cell)
{
    if (absrow < 0 || col < 0 || col >= s->cols)
        return 0;

    if (absrow < s->sb_len) {
        const sbline_t *line = &s->sb[(s->sb_head + absrow) % s->sb_cap];
        if (col < line->cols) {
            *cell = line->cells[col];
        } else {
            memset(cell, 0, sizeof(*cell));
            cell->width = 1;
        }
        return 1;
    }
    return screen_get_cell(s, absrow - s->sb_len, col, cell);
}

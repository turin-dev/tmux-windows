/* render.c — see render.h. */
#include "render.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Encode a Unicode code point as UTF-8 into `out`. */
static void put_utf8(strbuf_t *out, uint32_t cp)
{
    if (cp < 0x80) {
        strbuf_putc(out, (char)cp);
    } else if (cp < 0x800) {
        strbuf_putc(out, (char)(0xC0 | (cp >> 6)));
        strbuf_putc(out, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        strbuf_putc(out, (char)(0xE0 | (cp >> 12)));
        strbuf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        strbuf_putc(out, (char)(0x80 | (cp & 0x3F)));
    } else {
        strbuf_putc(out, (char)(0xF0 | (cp >> 18)));
        strbuf_putc(out, (char)(0x80 | ((cp >> 12) & 0x3F)));
        strbuf_putc(out, (char)(0x80 | ((cp >> 6) & 0x3F)));
        strbuf_putc(out, (char)(0x80 | (cp & 0x3F)));
    }
}

/* A resolved drawing pen: attributes plus concrete RGB / default colours.
 * Renders diff against the previously emitted pen so runs of same-styled cells
 * emit no redundant SGR (and plain text stays contiguous in the output). */
typedef struct pen {
    int bold, italic, underline, blink, reverse, conceal, strike;
    int fg_default, bg_default;
    int fr, fg, fb;   /* foreground rgb (valid when !fg_default) */
    int br, bg, bb;   /* background rgb (valid when !bg_default) */
} pen_t;

static void cell_to_pen(screen_t *s, const VTermScreenCell *cell, pen_t *p)
{
    VTermColor fg = cell->fg;
    VTermColor bg = cell->bg;

    p->bold      = cell->attrs.bold ? 1 : 0;
    p->italic    = cell->attrs.italic ? 1 : 0;
    p->underline = cell->attrs.underline ? 1 : 0;
    p->blink     = cell->attrs.blink ? 1 : 0;
    p->reverse   = cell->attrs.reverse ? 1 : 0;
    p->conceal   = cell->attrs.conceal ? 1 : 0;
    p->strike    = cell->attrs.strike ? 1 : 0;

    p->fg_default = VTERM_COLOR_IS_DEFAULT_FG(&fg);
    if (!p->fg_default) {
        screen_color_rgb(s, &fg);
        p->fr = fg.rgb.red; p->fg = fg.rgb.green; p->fb = fg.rgb.blue;
    }
    p->bg_default = VTERM_COLOR_IS_DEFAULT_BG(&bg);
    if (!p->bg_default) {
        screen_color_rgb(s, &bg);
        p->br = bg.rgb.red; p->bg = bg.rgb.green; p->bb = bg.rgb.blue;
    }
}

static int pen_eq(const pen_t *a, const pen_t *b)
{
    if (a->bold != b->bold || a->italic != b->italic ||
        a->underline != b->underline || a->blink != b->blink ||
        a->reverse != b->reverse || a->conceal != b->conceal ||
        a->strike != b->strike)
        return 0;
    if (a->fg_default != b->fg_default || a->bg_default != b->bg_default)
        return 0;
    if (!a->fg_default && (a->fr != b->fr || a->fg != b->fg || a->fb != b->fb))
        return 0;
    if (!a->bg_default && (a->br != b->br || a->bg != b->bg || a->bb != b->bb))
        return 0;
    return 1;
}

static void put_pen(strbuf_t *out, const pen_t *p)
{
    strbuf_append(out, "\x1b[0", 3);
    if (p->bold)      strbuf_append(out, ";1", 2);
    if (p->italic)    strbuf_append(out, ";3", 2);
    if (p->underline) strbuf_append(out, ";4", 2);
    if (p->blink)     strbuf_append(out, ";5", 2);
    if (p->reverse)   strbuf_append(out, ";7", 2);
    if (p->conceal)   strbuf_append(out, ";8", 2);
    if (p->strike)    strbuf_append(out, ";9", 2);
    if (!p->fg_default)
        strbuf_printf(out, ";38;2;%d;%d;%d", p->fr, p->fg, p->fb);
    if (!p->bg_default)
        strbuf_printf(out, ";48;2;%d;%d;%d", p->br, p->bg, p->bb);
    strbuf_putc(out, 'm');
}

void render_frame(strbuf_t *out, screen_t *s)
{
    int rows = screen_rows(s);
    int cols = screen_cols(s);
    int row, col, cur_row, cur_col, cur_visible;

    if (!screen_has_dirty(s))
        return;

    /* Hide the cursor while repainting to avoid it flickering across the grid. */
    strbuf_append(out, "\x1b[?25l", 6);

    for (row = 0; row < rows; row++) {
        pen_t cur;
        int have_pen = 0;
        if (!screen_row_dirty(s, row))
            continue;
        /* Move to column 1 of this row (1-based) and clear it. */
        strbuf_printf(out, "\x1b[%d;1H\x1b[2K", row + 1);
        for (col = 0; col < cols; col++) {
            VTermScreenCell cell;
            pen_t p;
            if (!screen_get_cell(s, row, col, &cell))
                continue;
            /* Width-0 continuation of a wide glyph: skip, it was already drawn. */
            if (cell.width == 0)
                continue;
            cell_to_pen(s, &cell, &p);
            if (!have_pen || !pen_eq(&cur, &p)) {
                put_pen(out, &p);
                cur = p;
                have_pen = 1;
            }
            if (cell.chars[0] == 0) {
                strbuf_putc(out, ' ');
            } else {
                int i;
                for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
                    put_utf8(out, cell.chars[i]);
            }
            /* A width-2 glyph occupies this cell and the next; advance over it. */
            if (cell.width == 2)
                col++;
        }
    }

    strbuf_append(out, "\x1b[0m", 4);

    screen_cursor(s, &cur_row, &cur_col, &cur_visible);
    strbuf_printf(out, "\x1b[%d;%dH", cur_row + 1, cur_col + 1);
    if (cur_visible)
        strbuf_append(out, "\x1b[?25h", 6);

    screen_clear_dirty(s);
}

void render_clear(strbuf_t *out)
{
    /* Reset attrs, clear whole screen, home the cursor. */
    strbuf_append(out, "\x1b[0m\x1b[2J\x1b[H", 10);
}

void render_pane(strbuf_t *out, const pane_t *p)
{
    screen_t *s = p->screen;
    int rows = screen_rows(s);
    int cols = screen_cols(s);
    int row, col;

    if (rows > p->rows) rows = p->rows;
    if (cols > p->cols) cols = p->cols;

    if (!screen_has_dirty(s))
        return;

    for (row = 0; row < rows; row++) {
        pen_t cur;
        int have_pen = 0;
        if (!screen_row_dirty(s, row))
            continue;
        /* Position at the pane's origin for this row. We write exactly `cols`
         * cells (no erase-to-EOL, which would eat borders / neighbour panes). */
        strbuf_printf(out, "\x1b[%d;%dH", p->y + row + 1, p->x + 1);
        for (col = 0; col < cols; col++) {
            VTermScreenCell cell;
            pen_t pn;
            if (!screen_get_cell(s, row, col, &cell))
                continue;
            if (cell.width == 0)
                continue;
            /* A wide glyph would spill past the pane's right edge: pad instead. */
            if (cell.width == 2 && col + 1 >= cols) {
                if (!have_pen) { pen_t def; cell_to_pen(s, &cell, &def); put_pen(out, &def); cur = def; have_pen = 1; }
                strbuf_putc(out, ' ');
                break;
            }
            cell_to_pen(s, &cell, &pn);
            if (!have_pen || !pen_eq(&cur, &pn)) {
                put_pen(out, &pn);
                cur = pn;
                have_pen = 1;
            }
            if (cell.chars[0] == 0) {
                strbuf_putc(out, ' ');
            } else {
                int i;
                for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
                    put_utf8(out, cell.chars[i]);
            }
            if (cell.width == 2)
                col++;
        }
        strbuf_append(out, "\x1b[0m", 4);
    }

    screen_clear_dirty(s);
}

void render_pane_copymode(strbuf_t *out, const pane_t *p, const copymode_t *cm)
{
    screen_t *s = p->screen;
    int vh = p->rows, w = p->cols;
    int top = copymode_top(cm);
    int totlines = screen_total_lines(s);
    int vrow, col, vr, vc;
    char ind[40];
    int ilen;

    strbuf_append(out, "\x1b[?25l", 6);

    for (vrow = 0; vrow < vh; vrow++) {
        int absline = top + vrow;
        pen_t cur;
        int have_pen = 0;
        strbuf_printf(out, "\x1b[%d;%dH", p->y + vrow + 1, p->x + 1);
        for (col = 0; col < w; col++) {
            VTermScreenCell cell;
            pen_t pn;
            if (absline >= totlines || !screen_line_cell(s, absline, col, &cell)) {
                pen_t def;
                memset(&def, 0, sizeof(def));
                def.fg_default = 1;
                def.bg_default = 1;
                if (!have_pen || !pen_eq(&cur, &def)) { put_pen(out, &def); cur = def; have_pen = 1; }
                strbuf_putc(out, ' ');
                continue;
            }
            if (cell.width == 0)
                continue;
            cell_to_pen(s, &cell, &pn);
            if (copymode_selected(cm, absline, col))
                pn.reverse = 1;
            if (!have_pen || !pen_eq(&cur, &pn)) { put_pen(out, &pn); cur = pn; have_pen = 1; }
            if (cell.chars[0] == 0) {
                strbuf_putc(out, ' ');
            } else {
                int i;
                for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
                    put_utf8(out, cell.chars[i]);
            }
            if (cell.width == 2 && col + 1 < w)
                col++;
        }
        strbuf_append(out, "\x1b[0m", 4);
    }

    /* Copy-mode indicator, top-right of the pane. */
    _snprintf_s(ind, sizeof(ind), _TRUNCATE, "[COPY %d/%d]", top, totlines);
    ilen = (int)strlen(ind);
    if (ilen < w)
        strbuf_printf(out, "\x1b[%d;%dH\x1b[7m%s\x1b[0m",
                      p->y + 1, p->x + w - ilen + 1, ind);

    /* Place the copy cursor. */
    copymode_cursor(cm, &vr, &vc);
    if (vr >= 0 && vr < vh)
        strbuf_printf(out, "\x1b[%d;%dH\x1b[?25h", p->y + vr + 1, p->x + vc + 1);
}

void render_active_cursor(strbuf_t *out, const pane_t *active)
{
    int r = 0, c = 0, vis = 0;
    if (active == NULL) {
        strbuf_append(out, "\x1b[?25l", 6);
        return;
    }
    screen_cursor(active->screen, &r, &c, &vis);
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    if (r >= active->rows) r = active->rows - 1;
    if (c >= active->cols) c = active->cols - 1;
    strbuf_printf(out, "\x1b[%d;%dH", active->y + r + 1, active->x + c + 1);
    strbuf_append(out, vis ? "\x1b[?25h" : "\x1b[?25l", 6);
}

/* copymode.c — see copymode.h. */
#include "copymode.h"

#include "emu/screen.h"

#include <stdlib.h>
#include <string.h>

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int vp_rows(const copymode_t *cm) { return cm->pane->rows; }
static int vp_cols(const copymode_t *cm) { return cm->pane->cols; }
static int total(const copymode_t *cm)   { return screen_total_lines(cm->pane->screen); }

/* Keep the cursor line inside the viewport, and the viewport inside history. */
static void adjust_view(copymode_t *cm)
{
    int vh = vp_rows(cm);
    int last_top = total(cm) - vh;
    if (last_top < 0) last_top = 0;

    cm->cur_line = clampi(cm->cur_line, 0, total(cm) - 1);
    cm->cur_col = clampi(cm->cur_col, 0, vp_cols(cm) - 1);

    if (cm->cur_line < cm->top)
        cm->top = cm->cur_line;
    else if (cm->cur_line >= cm->top + vh)
        cm->top = cm->cur_line - vh + 1;
    cm->top = clampi(cm->top, 0, last_top);
}

void copymode_enter(copymode_t *cm, pane_t *pane)
{
    int vh;
    memset(cm, 0, sizeof(*cm));
    cm->active = 1;
    cm->pane = pane;
    vh = pane->rows;
    cm->top = total(cm) - vh;
    if (cm->top < 0) cm->top = 0;
    cm->cur_line = total(cm) - 1;
    if (cm->cur_line < 0) cm->cur_line = 0;
    cm->cur_col = 0;
    adjust_view(cm);
}

void copymode_exit(copymode_t *cm)
{
    cm->active = 0;
    cm->sel = 0;
    cm->pane = NULL;
}

/* Order the two selection endpoints so (l0,c0) precedes (l1,c1). */
static void order(int *l0, int *c0, int *l1, int *c1)
{
    if (*l0 > *l1 || (*l0 == *l1 && *c0 > *c1)) {
        int tl = *l0, tc = *c0;
        *l0 = *l1; *c0 = *c1;
        *l1 = tl; *c1 = tc;
    }
}

static void put_cp(strbuf_t *out, uint32_t cp)
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

/* Gather the selected (or current-line) text into `text` as UTF-8. */
static void extract(copymode_t *cm, strbuf_t *text)
{
    screen_t *sc = cm->pane->screen;
    int cols = vp_cols(cm);
    int l0, c0, l1, c1, line;

    if (cm->sel) {
        l0 = cm->anchor_line; c0 = cm->anchor_col;
        l1 = cm->cur_line;    c1 = cm->cur_col;
        order(&l0, &c0, &l1, &c1);
    } else {
        l0 = l1 = cm->cur_line;
        c0 = 0; c1 = cols - 1;
    }

    strbuf_clear(text);
    for (line = l0; line <= l1; line++) {
        int startc = (line == l0) ? c0 : 0;
        int endc = (line == l1) ? c1 : cols - 1;
        strbuf_t ln;
        int col, i, trim;
        strbuf_init(&ln);
        for (col = startc; col <= endc; col++) {
            VTermScreenCell cell;
            if (!screen_line_cell(sc, line, col, &cell) || cell.width == 0)
                continue;
            if (cell.chars[0] == 0) {
                strbuf_putc(&ln, ' ');
            } else {
                for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
                    put_cp(&ln, cell.chars[i]);
            }
        }
        /* Trim trailing spaces on the line. */
        trim = (int)ln.len;
        while (trim > 0 && ln.data[trim - 1] == ' ')
            trim--;
        strbuf_append(text, ln.data, (size_t)trim);
        if (line < l1)
            strbuf_append(text, "\r\n", 2);
        strbuf_free(&ln);
    }
}

static void move(copymode_t *cm, int dline, int dcol)
{
    cm->cur_line += dline;
    cm->cur_col += dcol;
    adjust_view(cm);
}

static void page(copymode_t *cm, int dir)
{
    move(cm, dir * vp_rows(cm), 0);
}

/* Apply a single logical action. Returns 1 if a copy was produced. */
static int action(copymode_t *cm, int what, strbuf_t *text)
{
    switch (what) {
        case 'k': move(cm, -1, 0); break;
        case 'j': move(cm, +1, 0); break;
        case 'h': move(cm, 0, -1); break;
        case 'l': move(cm, 0, +1); break;
        case 'u': page(cm, -1); break;   /* page up */
        case 'D': page(cm, +1); break;   /* page down */
        case 'g': cm->cur_line = 0; adjust_view(cm); break;
        case 'G': cm->cur_line = total(cm) - 1; adjust_view(cm); break;
        case ' ':
            if (!cm->sel) {
                cm->sel = 1;
                cm->anchor_line = cm->cur_line;
                cm->anchor_col = cm->cur_col;
            } else {
                cm->sel = 0;
            }
            break;
        case '\r':
            extract(cm, text);
            copymode_exit(cm);
            return 1;
        case 'q':
            copymode_exit(cm);
            break;
        default:
            break;
    }
    return 0;
}

/* Translate a completed CSI (in cm->csi) to a logical action char, or 0. */
static int csi_action(copymode_t *cm)
{
    char f = cm->csilen ? cm->csi[cm->csilen - 1] : 0;
    switch (f) {
        case 'A': return 'k';
        case 'B': return 'j';
        case 'C': return 'l';
        case 'D': return 'h';
        case 'H': return 'g';   /* Home */
        case 'F': return 'G';   /* End */
        case '~':
            if (cm->csilen >= 2 && cm->csi[0] == '5') return 'u';  /* PageUp */
            if (cm->csilen >= 2 && cm->csi[0] == '6') return 'D';  /* PageDown */
            return 0;
        default: return 0;
    }
}

int copymode_input(copymode_t *cm, const char *bytes, size_t n, strbuf_t *text)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (!cm->active)                    /* exited mid-chunk */
            break;

        if (cm->esc == 1) {                 /* just saw ESC */
            if (c == '[') {
                cm->esc = 2;
                cm->csilen = 0;
            } else {
                copymode_exit(cm);          /* lone ESC cancels */
                return 0;
            }
            continue;
        }
        if (cm->esc == 2) {                 /* accumulating CSI */
            if (cm->csilen < (int)sizeof(cm->csi))
                cm->csi[cm->csilen++] = (char)c;
            if (c >= 0x40 && c <= 0x7e) {   /* final byte */
                int a = csi_action(cm);
                cm->esc = 0;
                if (a && action(cm, a, text))
                    return 1;
            }
            continue;
        }

        if (c == 0x1b) { cm->esc = 1; continue; }

        {
            int a = 0;
            switch (c) {
                case 'k': case 'j': case 'h': case 'l':
                case 'g': case 'G': case 'q': a = c; break;
                case ' ': a = ' '; break;
                case '\r': case '\n': case 'y': a = '\r'; break;
                case 0x15: a = 'u'; break;   /* Ctrl-U */
                case 0x04: a = 'D'; break;   /* Ctrl-D */
                default: a = 0; break;
            }
            if (a && action(cm, a, text))
                return 1;
        }
    }

    /* A chunk ending on a lone ESC (not the start of a CSI) is a cancel. Local
     * console input delivers full arrow sequences in one read, so this doesn't
     * clip real escape sequences. */
    if (cm->active && cm->esc == 1) {
        copymode_exit(cm);
        cm->esc = 0;
    }
    return 0;
}

int copymode_top(const copymode_t *cm)
{
    return cm->top;
}

void copymode_cursor(const copymode_t *cm, int *vrow, int *vcol)
{
    if (vrow) *vrow = cm->cur_line - cm->top;
    if (vcol) *vcol = cm->cur_col;
}

int copymode_selected(const copymode_t *cm, int line, int col)
{
    int l0, c0, l1, c1;
    if (!cm->sel)
        return 0;
    l0 = cm->anchor_line; c0 = cm->anchor_col;
    l1 = cm->cur_line;    c1 = cm->cur_col;
    order(&l0, &c0, &l1, &c1);
    if (line < l0 || line > l1)
        return 0;
    if (line == l0 && col < c0)
        return 0;
    if (line == l1 && col > c1)
        return 0;
    return 1;
}

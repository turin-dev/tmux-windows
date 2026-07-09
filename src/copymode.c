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

    if (cm->sel && cm->sel_mode == 1) {                 /* linewise */
        l0 = cm->anchor_line < cm->cur_line ? cm->anchor_line : cm->cur_line;
        l1 = cm->anchor_line > cm->cur_line ? cm->anchor_line : cm->cur_line;
        c0 = 0; c1 = cols - 1;
    } else if (cm->sel && cm->sel_mode == 2) {          /* block */
        l0 = cm->anchor_line < cm->cur_line ? cm->anchor_line : cm->cur_line;
        l1 = cm->anchor_line > cm->cur_line ? cm->anchor_line : cm->cur_line;
        c0 = cm->anchor_col < cm->cur_col ? cm->anchor_col : cm->cur_col;
        c1 = cm->anchor_col > cm->cur_col ? cm->anchor_col : cm->cur_col;
    } else if (cm->sel) {                               /* characterwise */
        l0 = cm->anchor_line; c0 = cm->anchor_col;
        l1 = cm->cur_line;    c1 = cm->cur_col;
        order(&l0, &c0, &l1, &c1);
    } else {
        l0 = l1 = cm->cur_line;
        c0 = 0; c1 = cols - 1;
    }

    strbuf_clear(text);
    for (line = l0; line <= l1; line++) {
        int block = (cm->sel && cm->sel_mode == 2);
        int startc = block ? c0 : ((line == l0) ? c0 : 0);
        int endc   = block ? c1 : ((line == l1) ? c1 : cols - 1);
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

/* Render one history line into `buf` as one char per column (ASCII; non-ASCII
 * cells become '?'), for substring searching. Returns the length. */
static int build_line(copymode_t *cm, int line, char *buf, int cap)
{
    screen_t *sc = cm->pane->screen;
    int cols = vp_cols(cm), col, n = 0;
    for (col = 0; col < cols && n < cap - 1; col++) {
        VTermScreenCell cell;
        char ch = ' ';
        if (screen_line_cell(sc, line, col, &cell) && cell.width != 0) {
            uint32_t cp = cell.chars[0];
            ch = (cp == 0) ? ' ' : (cp < 128 ? (char)cp : '?');
        }
        buf[n++] = ch;
    }
    buf[n] = '\0';
    return n;
}

/* Find the next occurrence of the query in direction `dir`, starting just past
 * the cursor and wrapping around. Moves the cursor and returns 1 if found. */
static int do_search(copymode_t *cm, int dir)
{
    int nlines = total(cm);
    char buf[1024];
    int steps, qlen = cm->qlen;

    if (qlen == 0 || nlines == 0 || dir == 0)
        return 0;
    cm->search_dir = dir;

    for (steps = 0; steps <= nlines; steps++) {
        int line = (((cm->cur_line + dir * steps) % nlines) + nlines) % nlines;
        int len = build_line(cm, line, buf, sizeof(buf));
        if (dir > 0) {
            int from = (steps == 0) ? cm->cur_col + 1 : 0;
            int i;
            if (from < 0) from = 0;
            for (i = from; i + qlen <= len; i++) {
                if (strncmp(buf + i, cm->query, (size_t)qlen) == 0) {
                    cm->cur_line = line; cm->cur_col = i; adjust_view(cm); return 1;
                }
            }
        } else {
            int limit = (steps == 0) ? cm->cur_col - 1 : len - 1;
            int best = -1, i;
            for (i = 0; i + qlen <= len && i <= limit; i++)
                if (strncmp(buf + i, cm->query, (size_t)qlen) == 0) best = i;
            if (best >= 0) {
                cm->cur_line = line; cm->cur_col = best; adjust_view(cm); return 1;
            }
        }
    }
    return 0;
}

static void move(copymode_t *cm, int dline, int dcol)
{
    cm->cur_line += dline;
    cm->cur_col += dcol;
    adjust_view(cm);
}

static int last_nonspace(copymode_t *cm, int line)
{
    char b[1024];
    int n = build_line(cm, line, b, sizeof b), i;
    for (i = n - 1; i >= 0; i--)
        if (b[i] != ' ') return i;
    return 0;
}

static int first_nonspace(copymode_t *cm, int line)
{
    char b[1024];
    int n = build_line(cm, line, b, sizeof b), i;
    for (i = 0; i < n; i++)
        if (b[i] != ' ') return i;
    return 0;
}

/* vi-style word motions within the current line (space is the separator). */
static void word_fwd(copymode_t *cm)
{
    char b[1024];
    int n = build_line(cm, cm->cur_line, b, sizeof b), i = cm->cur_col;
    while (i < n && b[i] != ' ') i++;   /* leave the current word */
    while (i < n && b[i] == ' ') i++;   /* skip the gap */
    if (i >= n) i = (n > 0) ? n - 1 : 0;
    cm->cur_col = i;
}

static void word_back(copymode_t *cm)
{
    char b[1024];
    int i = cm->cur_col;
    (void)build_line(cm, cm->cur_line, b, sizeof b);
    if (i > 0) i--;
    while (i > 0 && b[i] == ' ') i--;
    while (i > 0 && b[i - 1] != ' ') i--;
    cm->cur_col = i;
}

static void word_end(copymode_t *cm)
{
    char b[1024];
    int n = build_line(cm, cm->cur_line, b, sizeof b), i = cm->cur_col;
    if (i < n - 1) i++;
    while (i < n && b[i] == ' ') i++;
    while (i < n - 1 && b[i + 1] != ' ') i++;
    if (i >= n) i = (n > 0) ? n - 1 : 0;
    cm->cur_col = i;
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
        case '0': cm->cur_col = 0; adjust_view(cm); break;
        case '$': cm->cur_col = last_nonspace(cm, cm->cur_line); adjust_view(cm); break;
        case '^': cm->cur_col = first_nonspace(cm, cm->cur_line); adjust_view(cm); break;
        case 'w': word_fwd(cm); adjust_view(cm); break;
        case 'b': word_back(cm); adjust_view(cm); break;
        case 'e': word_end(cm); adjust_view(cm); break;
        case 'H': cm->cur_line = cm->top; adjust_view(cm); break;
        case 'M': cm->cur_line = cm->top + vp_rows(cm) / 2; adjust_view(cm); break;
        case 'L': cm->cur_line = cm->top + vp_rows(cm) - 1; adjust_view(cm); break;
        case ' ':
            if (!cm->sel) {
                cm->sel = 1;
                cm->sel_mode = 0;
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

        if (cm->searching) {                /* typing a search query */
            if (c == '\r' || c == '\n') {
                cm->searching = 0;
                do_search(cm, cm->search_dir);
            } else if (c == 0x1b) {
                cm->searching = 0;          /* cancel the query, stay in copy mode */
                cm->qlen = 0;
                cm->query[0] = '\0';
            } else if (c == 0x7f || c == 0x08) {
                if (cm->qlen > 0) cm->query[--cm->qlen] = '\0';
            } else if (c >= 0x20 && c < 0x7f && cm->qlen < (int)sizeof(cm->query) - 1) {
                cm->query[cm->qlen++] = (char)c;
                cm->query[cm->qlen] = '\0';
            }
            continue;
        }

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

        if (c == '/') { cm->searching = 1; cm->search_dir = +1; cm->qlen = 0; cm->query[0] = '\0'; continue; }
        if (c == '?') { cm->searching = 1; cm->search_dir = -1; cm->qlen = 0; cm->query[0] = '\0'; continue; }
        if (c == 'n') { do_search(cm, cm->search_dir ? cm->search_dir : 1); continue; }
        if (c == 'N') { do_search(cm, cm->search_dir ? -cm->search_dir : -1); continue; }
        if (c == 0x15) { move(cm, -(vp_rows(cm) / 2), 0); continue; }   /* Ctrl-U: half up */
        if (c == 0x04) { move(cm, +(vp_rows(cm) / 2), 0); continue; }   /* Ctrl-D: half down */
        if (c == 'V') {                       /* linewise visual toggle */
            if (cm->sel && cm->sel_mode == 1) cm->sel = 0;
            else { cm->sel = 1; cm->sel_mode = 1; cm->anchor_line = cm->cur_line; cm->anchor_col = cm->cur_col; }
            continue;
        }
        if (c == 0x16) {                      /* Ctrl-V: block visual toggle */
            if (cm->sel && cm->sel_mode == 2) cm->sel = 0;
            else { cm->sel = 1; cm->sel_mode = 2; cm->anchor_line = cm->cur_line; cm->anchor_col = cm->cur_col; }
            continue;
        }

        {
            int a = 0;
            switch (c) {
                case 'k': case 'j': case 'h': case 'l':
                case 'g': case 'G': case 'q':
                case '0': case '$': case '^':
                case 'w': case 'b': case 'e':
                case 'H': case 'M': case 'L': a = c; break;
                case 'v': case ' ': a = ' '; break;   /* v: visual toggle (like Space) */
                case '\r': case '\n': case 'y': a = '\r'; break;
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

int copymode_search_prompt(const copymode_t *cm, char *dir, const char **query)
{
    if (!cm->searching)
        return 0;
    if (dir)   *dir = (cm->search_dir < 0) ? '?' : '/';
    if (query) *query = cm->query;
    return 1;
}

int copymode_selected(const copymode_t *cm, int line, int col)
{
    int l0, c0, l1, c1;
    if (!cm->sel)
        return 0;

    if (cm->sel_mode == 1) {                     /* linewise */
        int lo = cm->anchor_line < cm->cur_line ? cm->anchor_line : cm->cur_line;
        int hi = cm->anchor_line > cm->cur_line ? cm->anchor_line : cm->cur_line;
        return line >= lo && line <= hi;
    }
    if (cm->sel_mode == 2) {                     /* block */
        int lo = cm->anchor_line < cm->cur_line ? cm->anchor_line : cm->cur_line;
        int hi = cm->anchor_line > cm->cur_line ? cm->anchor_line : cm->cur_line;
        int cl = cm->anchor_col < cm->cur_col ? cm->anchor_col : cm->cur_col;
        int cr = cm->anchor_col > cm->cur_col ? cm->anchor_col : cm->cur_col;
        return line >= lo && line <= hi && col >= cl && col <= cr;
    }

    l0 = cm->anchor_line; c0 = cm->anchor_col;   /* characterwise */
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

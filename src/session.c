/* session.c — see session.h.
 *
 * A session is a set of windows (tabs) sharing one attached client. It runs the
 * prefix state machine, switches between windows, and draws the status bar; the
 * per-window pane work lives in window.c.
 */
#include "session.h"

#include "model/window.h"
#include "status.h"
#include "render.h"
#include "copymode.h"
#include "cmd.h"
#include "platform/clipboard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PS_NORMAL, PS_PREFIX, PS_ESC, PS_CSI };
/* Normal-mode scanner for SGR mouse reports: ESC [ < b ; x ; y (M|m). */
enum { MN_NONE, MN_ESC, MN_CSI, MN_MOUSE };

#define PREFIX_KEY    0x02   /* default prefix: Ctrl-B */
#define MAX_WINDOWS   64
#define MAX_BINDINGS  128
#define BIND_CMD_MAX  128
#define PROMPT_MAX    256

#define REPEAT_TICKS  10   /* ~0.5s window for -r repeat keys (50ms serve tick) */

typedef struct keybind {
    int  key;
    int  repeat;           /* -r: usable without the prefix during the window */
    char cmd[BIND_CMD_MAX];
} keybind_t;

struct session {
    HANDLE          wake;
    const wchar_t  *shell;
    window_t       *windows[MAX_WINDOWS];
    int             nwindows;
    int             cur;
    int             last_window;   /* previously current window (for last-window) */
    char            name[64];
    int             cols, rows;   /* full terminal size (status bar included) */
    int             pstate;
    int             full_redraw;
    int             changed;
    int             quit;
    int             detach;
    int             last_min;
    copymode_t      copy;
    /* options */
    int             prefix_key;
    int             status_on;
    int             base_index;       /* first window number shown/selected */
    int             pane_base_index;  /* first pane number for select-pane -t */
    char            status_left[128]; /* status-left format */
    char            status_right[128];/* status-right format */
    int             show_panes;       /* display-panes overlay ticks remaining */
    int             clock_mode;       /* big-clock overlay active */
    int             repeat_ticks;     /* -r repeat window remaining */
    char            message[160];     /* transient status message */
    int             message_ticks;    /* display-message ticks remaining */
    int             choose_active;    /* choose-window picker open */
    int             choose_sel;       /* highlighted window in the picker */
    int             mouse_on;
    int             mouse_dirty;   /* mouse mode changed; (re)emit DECSET/DECRST */
    /* normal-mode SGR mouse scanner */
    int             mstate;
    char            mraw[48];
    int             mrawlen;
    /* key bindings (prefix table) */
    keybind_t       bind[MAX_BINDINGS];
    int             nbind;
    /* command prompt */
    int             prompt_active;
    char            prompt[PROMPT_MAX];
    int             prompt_len;
    /* CSI (arrow/modified-arrow) accumulator while decoding prefix sequences */
    char            csi[16];
    int             csilen;
};

static void session_run_command(session_t *s, const char *line);
static void session_load_config_path(session_t *s, const char *path);
static void expand_status(session_t *s, const char *fmt, char *out, size_t cap);

/* Run `cmdline` through cmd.exe, capturing up to cap-1 bytes of stdout into
 * `out` and the process exit code into *code. Returns 0 on success. Blocks. */
static int capture_command(const char *cmdline, char *out, size_t cap, DWORD *code)
{
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char full[1024];
    DWORD total = 0;

    if (out && cap) out[0] = '\0';
    if (code) *code = (DWORD)-1;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return 1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    ZeroMemory(&pi, sizeof(pi));

    _snprintf_s(full, sizeof(full), _TRUNCATE, "cmd.exe /c %s", cmdline);
    if (!CreateProcessA(NULL, full, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return 1;
    }
    CloseHandle(wr);   /* parent keeps only the read end */

    if (out && cap > 1) {
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(rd, out + total, (DWORD)(cap - 1 - total), &got, NULL) || got == 0)
                break;
            total += got;
            if (total >= cap - 1) break;
        }
        out[total] = '\0';
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 10000);
    if (code) GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

/* Trim a captured buffer to its first line (for status display). */
static void first_line(char *s)
{
    char *p = s;
    while (*p && *p != '\r' && *p != '\n') p++;
    *p = '\0';
}

/* ----- big-digit clock (clock-mode) ----------------------------------------- */

/* 3x5 glyphs for '0'..'9' and ':' ('#' = lit block). */
static const char *const CLOCK_FONT[11][5] = {
    {"###","# #","# #","# #","###"}, /* 0 */
    {"  #","  #","  #","  #","  #"}, /* 1 */
    {"###","  #","###","#  ","###"}, /* 2 */
    {"###","  #","###","  #","###"}, /* 3 */
    {"# #","# #","###","  #","  #"}, /* 4 */
    {"###","#  ","###","  #","###"}, /* 5 */
    {"###","#  ","###","# #","###"}, /* 6 */
    {"###","  #","  #","  #","  #"}, /* 7 */
    {"###","# #","###","# #","###"}, /* 8 */
    {"###","# #","###","  #","###"}, /* 9 */
    {"   "," # ","   "," # ","   "}, /* : */
};

static const char *clock_glyph(char c, int row)
{
    if (c >= '0' && c <= '9') return CLOCK_FONT[c - '0'][row];
    if (c == ':')             return CLOCK_FONT[10][row];
    return "   ";
}

/* Draw "HH:MM" in big digits centred in the rectangle (x,y,cols,rows). */
static void clock_draw(strbuf_t *out, int x, int y, int cols, int rows)
{
    SYSTEMTIME st;
    char t[6];
    int gw, ox, oy, row, i;

    GetLocalTime(&st);
    _snprintf_s(t, sizeof(t), _TRUNCATE, "%02d:%02d", st.wHour, st.wMinute);

    gw = 5 * 3 + 4;                 /* 5 glyphs, 3 wide, 1-col gaps */
    ox = x + (cols - gw) / 2;
    oy = y + (rows - 5) / 2;
    if (ox < x) ox = x;
    if (oy < y) oy = y;

    for (row = 0; row < 5; row++) {
        int cx = ox;
        strbuf_printf(out, "\x1b[%d;%dH", oy + row + 1, ox + 1);
        for (i = 0; t[i]; i++) {
            const char *g = clock_glyph(t[i], row);
            int k;
            for (k = 0; k < 3; k++)
                if (g[k] == '#') strbuf_append(out, "\x1b[7m \x1b[0m", 9);
                else             strbuf_putc(out, ' ');
            strbuf_putc(out, ' ');   /* inter-glyph gap */
            cx += 4;
        }
        (void)cx;
    }
}

/* Exit copy mode if it no longer applies to the current active pane (e.g. after
 * switching panes/windows or a pane exiting). */
static void sync_copy(session_t *s)
{
    window_t *w = (s->nwindows > 0) ? s->windows[s->cur] : NULL;
    if (s->copy.active && (w == NULL || window_active(w) != s->copy.pane)) {
        copymode_exit(&s->copy);
        s->full_redraw = 1;
        s->changed = 1;
    }
}

static int win_area_rows(const session_t *s)
{
    int h = s->rows - 1;   /* reserve the bottom row for the status bar */
    return h < 1 ? 1 : h;
}

static window_t *cur_window(session_t *s)
{
    return (s->nwindows > 0) ? s->windows[s->cur] : NULL;
}

static void mark(session_t *s, int full)
{
    if (full) s->full_redraw = 1;
    s->changed = 1;
}

/* ----- window management ---------------------------------------------------- */

static void new_window(session_t *s)
{
    window_t *w;
    if (s->nwindows >= MAX_WINDOWS)
        return;
    w = window_create(s->shell, s->cols, win_area_rows(s), s->wake);
    if (w == NULL)
        return;
    s->windows[s->nwindows] = w;
    s->cur = s->nwindows;
    s->nwindows++;
    mark(s, 1);
}

static void remove_window(session_t *s, int idx)
{
    int i;
    if (idx < 0 || idx >= s->nwindows)
        return;
    window_free(s->windows[idx]);
    for (i = idx; i < s->nwindows - 1; i++)
        s->windows[i] = s->windows[i + 1];
    s->nwindows--;
    if (s->last_window == idx)        s->last_window = -1;
    else if (s->last_window > idx)    s->last_window--;
    if (s->nwindows == 0) {
        s->quit = 1;
        return;
    }
    if (s->cur >= s->nwindows)
        s->cur = s->nwindows - 1;
    else if (idx < s->cur)
        s->cur--;
    mark(s, 1);
}

static void select_window(session_t *s, int idx)
{
    if (idx >= 0 && idx < s->nwindows && idx != s->cur) {
        s->last_window = s->cur;
        s->cur = idx;
        mark(s, 1);
    }
}

static void kill_window(session_t *s)
{
    remove_window(s, s->cur);
}

/* ----- key bindings --------------------------------------------------------- */

static const keybind_t *bind_lookup(const session_t *s, int key)
{
    int i;
    for (i = 0; i < s->nbind; i++)
        if (s->bind[i].key == key)
            return &s->bind[i];
    return NULL;
}

static void bind_mark_repeat(session_t *s, int key, int repeat)
{
    int i;
    for (i = 0; i < s->nbind; i++)
        if (s->bind[i].key == key) { s->bind[i].repeat = repeat; return; }
}

static void bind_set(session_t *s, int key, const char *cmd)
{
    int i;
    for (i = 0; i < s->nbind; i++) {
        if (s->bind[i].key == key) {
            strncpy_s(s->bind[i].cmd, BIND_CMD_MAX, cmd, _TRUNCATE);
            return;
        }
    }
    if (s->nbind < MAX_BINDINGS) {
        s->bind[s->nbind].key = key;
        strncpy_s(s->bind[s->nbind].cmd, BIND_CMD_MAX, cmd, _TRUNCATE);
        s->nbind++;
    }
}

static void bind_remove(session_t *s, int key)
{
    int i, j;
    for (i = 0; i < s->nbind; i++) {
        if (s->bind[i].key == key) {
            for (j = i; j < s->nbind - 1; j++)
                s->bind[j] = s->bind[j + 1];
            s->nbind--;
            return;
        }
    }
}

/* ----- commands ------------------------------------------------------------- */

static void cmd_new_window(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    new_window(s);
}

static void cmd_split_window(session_t *s, int argc, char **argv)
{
    int type = LN_SPLIT_H;   /* default: top / bottom (tmux -v) */
    window_t *w = cur_window(s);
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0) type = LN_SPLIT_V;
        else if (strcmp(argv[i], "-v") == 0) type = LN_SPLIT_H;
    }
    if (w) { window_split(w, type, s->shell, s->wake); mark(s, 1); }
}

static void cmd_select_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    int i, did = 0;
    if (w == NULL)
        return;
    for (i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-U") == 0) { window_select_dir(w, DIR_UP);    did = 1; }
        else if (strcmp(argv[i], "-D") == 0) { window_select_dir(w, DIR_DOWN);  did = 1; }
        else if (strcmp(argv[i], "-L") == 0) { window_select_dir(w, DIR_LEFT);  did = 1; }
        else if (strcmp(argv[i], "-R") == 0) { window_select_dir(w, DIR_RIGHT); did = 1; }
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            window_select_index(w, atoi(argv[i + 1]) - s->pane_base_index); did = 1; i++;
        }
    }
    if (!did)
        window_select_next_pane(w);
    mark(s, 1);
}

static void cmd_kill_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w && window_kill_active(w)) remove_window(s, s->cur);
    else mark(s, 1);
}

static void cmd_resize_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    int i;
    if (w == NULL)
        return;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-Z") == 0) {   /* toggle zoom */
            window_toggle_zoom(w);
            mark(s, 1);
            return;
        }
    }
    for (i = 1; i < argc; i++) {
        int dir = -1;
        if      (strcmp(argv[i], "-L") == 0) dir = DIR_LEFT;
        else if (strcmp(argv[i], "-R") == 0) dir = DIR_RIGHT;
        else if (strcmp(argv[i], "-U") == 0) dir = DIR_UP;
        else if (strcmp(argv[i], "-D") == 0) dir = DIR_DOWN;
        if (dir >= 0) {
            int amount = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                amount = atoi(argv[i + 1]);
                i++;
            }
            window_resize_active(w, dir, amount);
            mark(s, 1);
        }
    }
}

static void cmd_select_layout(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    if (w == NULL)
        return;
    if (argc > 1) {
        int p = layout_preset_from_name(argv[1]);
        if (p >= 0) { window_set_layout(w, p); mark(s, 1); }
    } else {
        window_next_layout(w);
        mark(s, 1);
    }
}

static void cmd_next_layout(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w) { window_next_layout(w); mark(s, 1); }
}

static void cmd_rotate_window(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    int downward = 1, i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-U") == 0) downward = 0;
    if (w) { window_rotate(w, downward); mark(s, 1); }
}

static void cmd_swap_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    int next = 1, i;   /* -D (default): swap with the next pane; -U: previous */
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-U") == 0) next = 0;
    if (w) { window_swap_active(w, next); mark(s, 1); }
}

static void cmd_last_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w) { window_select_last(w); mark(s, 1); }
}

static void cmd_last_window(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s->last_window >= 0 && s->last_window < s->nwindows &&
        s->last_window != s->cur)
        select_window(s, s->last_window);
}

static void cmd_break_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    window_t *nw;
    pane_t *p;
    (void)argc; (void)argv;
    if (w == NULL || s->nwindows >= MAX_WINDOWS)
        return;
    p = window_extract_active(w);
    if (p == NULL)
        return;                         /* only pane in the window */
    nw = window_create_with_pane(p, s->cols, win_area_rows(s), w->name);
    if (nw == NULL) { pane_close(p); mark(s, 1); return; }
    s->windows[s->nwindows] = nw;
    s->last_window = s->cur;
    s->cur = s->nwindows;
    s->nwindows++;
    mark(s, 1);
}

static void cmd_join_pane(session_t *s, int argc, char **argv)
{
    window_t *dst = cur_window(s), *src;
    pane_t *p;
    int srcidx = -1, type = LN_SPLIT_H, i;
    for (i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-s") == 0 && i + 1 < argc) { srcidx = atoi(argv[++i]) - s->base_index; }
        else if (strcmp(argv[i], "-h") == 0) type = LN_SPLIT_V;
        else if (strcmp(argv[i], "-v") == 0) type = LN_SPLIT_H;
    }
    if (dst == NULL || srcidx < 0 || srcidx >= s->nwindows || srcidx == s->cur)
        return;
    src = s->windows[srcidx];
    p = window_detach_active(src);
    if (p == NULL)
        return;
    if (!window_insert_pane(dst, p, type)) {
        pane_close(p);                 /* could not place it */
    }
    if (window_empty(src))
        remove_window(s, srcidx);      /* adjusts s->cur; dst pointer stays valid */
    mark(s, 1);
}

static void cmd_paste_buffer(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    size_t len = 0;
    char *buf;
    (void)argc; (void)argv;
    if (w == NULL)
        return;
    buf = clipboard_get_utf8(&len);
    if (buf == NULL)
        return;
    {
        strbuf_t o;
        size_t i;
        strbuf_init(&o);
        for (i = 0; i < len; i++) {     /* newlines -> CR so the shell runs lines */
            char c = buf[i];
            if (c == '\n')      strbuf_putc(&o, '\r');
            else if (c == '\r') /* skip (CRLF collapses to one CR) */;
            else                strbuf_putc(&o, c);
        }
        if (o.len)
            window_write_active(w, o.data, o.len);
        strbuf_free(&o);
    }
    free(buf);
}

static void cmd_next_window(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s->nwindows > 1) select_window(s, (s->cur + 1) % s->nwindows);
}

static void cmd_prev_window(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s->nwindows > 1) select_window(s, (s->cur + s->nwindows - 1) % s->nwindows);
}

static void cmd_select_window(session_t *s, int argc, char **argv)
{
    int idx = -1, i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) { idx = atoi(argv[i + 1]); i++; }
        else if (argv[i][0] >= '0' && argv[i][0] <= '9') idx = atoi(argv[i]);
    }
    if (idx >= 0) select_window(s, idx - s->base_index);   /* -t is base-relative */
}

static void cmd_kill_window(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    kill_window(s);
}

static void cmd_move_window(session_t *s, int argc, char **argv)
{
    int dst = -1, i;
    window_t *cur;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) dst = atoi(argv[++i]) - s->base_index;
    if (dst < 0 || dst == s->cur)
        return;
    cur = s->windows[s->cur];
    for (i = s->cur; i < s->nwindows - 1; i++)   /* pull the window out */
        s->windows[i] = s->windows[i + 1];
    s->nwindows--;
    if (dst > s->nwindows) dst = s->nwindows;
    for (i = s->nwindows; i > dst; i--)          /* open a slot at dst */
        s->windows[i] = s->windows[i - 1];
    s->windows[dst] = cur;
    s->nwindows++;
    s->cur = dst;
    s->last_window = -1;
    mark(s, 1);
}

static void cmd_swap_window(session_t *s, int argc, char **argv)
{
    int a = s->cur, b = -1, i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) { a = atoi(argv[++i]) - s->base_index; }
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) { b = atoi(argv[++i]) - s->base_index; }
    }
    if (a < 0 || a >= s->nwindows || b < 0 || b >= s->nwindows || a == b)
        return;
    {
        window_t *tmp = s->windows[a];
        s->windows[a] = s->windows[b];
        s->windows[b] = tmp;
    }
    if (s->cur == a) s->cur = b;
    else if (s->cur == b) s->cur = a;
    mark(s, 1);
}

static void cmd_detach(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    s->detach = 1;
}

static void cmd_copymode(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w && window_active(w)) { copymode_enter(&s->copy, window_active(w)); mark(s, 1); }
}

static void cmd_send_prefix(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    char b = (char)s->prefix_key;
    (void)argc; (void)argv;
    if (w) window_write_active(w, &b, 1);
}

/* Translate one send-keys argument (a key name or literal text) into bytes. */
static void append_key(strbuf_t *o, const char *arg)
{
    int k = cmd_parse_key(arg);
    switch (k) {
        case KEY_UP:    strbuf_append(o, "\x1b[A", 3); return;
        case KEY_DOWN:  strbuf_append(o, "\x1b[B", 3); return;
        case KEY_RIGHT: strbuf_append(o, "\x1b[C", 3); return;
        case KEY_LEFT:  strbuf_append(o, "\x1b[D", 3); return;
        case KEY_PPAGE: strbuf_append(o, "\x1b[5~", 4); return;
        case KEY_NPAGE: strbuf_append(o, "\x1b[6~", 4); return;
        default: break;
    }
    if (k >= 0 && k < 256) { strbuf_putc(o, (char)k); return; }  /* char / C-x / Enter */
    strbuf_append(o, arg, strlen(arg));                          /* literal text */
}

static void cmd_send_keys(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    strbuf_t o;
    int i;
    if (w == NULL)
        return;
    strbuf_init(&o);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) { i++; continue; }  /* ignore target */
        append_key(&o, argv[i]);
    }
    if (o.len)
        window_write_active(w, o.data, o.len);
    strbuf_free(&o);
}

static void cmd_rename_window(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    if (w && argc > 1) { strncpy_s(w->name, sizeof(w->name), argv[1], _TRUNCATE); mark(s, 1); }
}

static void cmd_rename_session(session_t *s, int argc, char **argv)
{
    if (argc > 1) { strncpy_s(s->name, sizeof(s->name), argv[1], _TRUNCATE); mark(s, 1); }
}

static void cmd_set(session_t *s, int argc, char **argv)
{
    int i = 1;
    const char *opt, *val;
    while (i < argc && argv[i][0] == '-')   /* skip -g / -a etc. */
        i++;
    if (i >= argc) return;
    opt = argv[i++];
    val = (i < argc) ? argv[i] : NULL;

    if (strcmp(opt, "prefix") == 0 && val) {
        int k = cmd_parse_key(val);
        if (k >= 0) s->prefix_key = k;
    } else if (strcmp(opt, "status") == 0 && val) {
        s->status_on = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0);
        mark(s, 1);
    } else if (strcmp(opt, "mouse") == 0 && val) {
        int on = (strcmp(val, "on") == 0 || strcmp(val, "1") == 0);
        if (on != s->mouse_on) {
            s->mouse_on = on;
            s->mouse_dirty = 1;
            if (!on) { s->mstate = MN_NONE; s->mrawlen = 0; }
            mark(s, 1);
        }
    } else if (strcmp(opt, "base-index") == 0 && val) {
        s->base_index = atoi(val);
        mark(s, 1);
    } else if (strcmp(opt, "pane-base-index") == 0 && val) {
        s->pane_base_index = atoi(val);
    } else if (strcmp(opt, "status-left") == 0 && val) {
        strncpy_s(s->status_left, sizeof(s->status_left), val, _TRUNCATE);
        mark(s, 1);
    } else if (strcmp(opt, "status-right") == 0 && val) {
        strncpy_s(s->status_right, sizeof(s->status_right), val, _TRUNCATE);
        mark(s, 1);
    }
    /* Other options (mode-keys, history-limit, ...) are accepted and ignored. */
}

static void cmd_bind(session_t *s, int argc, char **argv)
{
    int k, i, ki = 1, repeat = 0;
    char buf[BIND_CMD_MAX];
    while (ki < argc && argv[ki][0] == '-' && argv[ki][1] != '\0') {
        if (strcmp(argv[ki], "-r") == 0) repeat = 1;
        ki++;   /* skip -r / -n / -T etc. */
    }
    if (argc - ki < 2) return;
    k = cmd_parse_key(argv[ki]);
    if (k < 0) return;
    buf[0] = '\0';
    for (i = ki + 1; i < argc; i++) {
        if (i > ki + 1) strcat_s(buf, sizeof(buf), " ");
        strcat_s(buf, sizeof(buf), argv[i]);
    }
    bind_set(s, k, buf);
    bind_mark_repeat(s, k, repeat);
}

static void cmd_unbind(session_t *s, int argc, char **argv)
{
    int k;
    if (argc < 2) return;
    k = cmd_parse_key(argv[1]);
    if (k >= 0) bind_remove(s, k);
}

static void cmd_source(session_t *s, int argc, char **argv)
{
    if (argc > 1) session_load_config_path(s, argv[1]);
}

static void cmd_display_panes(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    s->show_panes = 40;   /* ~2s at the ~50ms serve tick, or until a key */
    mark(s, 1);
}

static void cmd_choose_window(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    s->choose_active = 1;
    s->choose_sel = s->cur;
    mark(s, 1);
}

static void show_message(session_t *s, const char *text)
{
    strncpy_s(s->message, sizeof(s->message), text, _TRUNCATE);
    s->message_ticks = 40;    /* ~2s */
    mark(s, 1);
}

static void cmd_display_message(session_t *s, int argc, char **argv)
{
    char out[160];
    if (argc < 2)
        return;
    expand_status(s, argv[1], out, sizeof(out));   /* #S/#W/%H etc. */
    show_message(s, out);
}

static void cmd_run_shell(session_t *s, int argc, char **argv)
{
    char out[512];
    if (argc < 2)
        return;
    if (capture_command(argv[1], out, sizeof(out), NULL) == 0 && out[0]) {
        first_line(out);
        show_message(s, out);
    }
}

static void cmd_if_shell(session_t *s, int argc, char **argv)
{
    DWORD code = (DWORD)-1;
    if (argc < 3)
        return;
    capture_command(argv[1], NULL, 0, &code);
    if (code == 0)
        session_run_command(s, argv[2]);        /* condition succeeded */
    else if (argc > 3)
        session_run_command(s, argv[3]);        /* else branch */
}

static void cmd_clock_mode(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    s->clock_mode = 1;
    mark(s, 1);
}

static void cmd_command_prompt(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    s->prompt_active = 1;
    s->prompt_len = 0;
    s->prompt[0] = '\0';
    mark(s, 1);
}

typedef void (*cmd_fn)(session_t *, int, char **);

static const struct { const char *name; cmd_fn fn; } CMD_TABLE[] = {
    { "new-window",      cmd_new_window },
    { "split-window",    cmd_split_window },
    { "select-pane",     cmd_select_pane },
    { "resize-pane",     cmd_resize_pane },
    { "select-layout",   cmd_select_layout },
    { "next-layout",     cmd_next_layout },
    { "rotate-window",   cmd_rotate_window },
    { "swap-pane",       cmd_swap_pane },
    { "break-pane",      cmd_break_pane },
    { "join-pane",       cmd_join_pane },
    { "paste-buffer",    cmd_paste_buffer },
    { "last-pane",       cmd_last_pane },
    { "last-window",     cmd_last_window },
    { "kill-pane",       cmd_kill_pane },
    { "next-window",     cmd_next_window },
    { "previous-window", cmd_prev_window },
    { "select-window",   cmd_select_window },
    { "swap-window",     cmd_swap_window },
    { "move-window",     cmd_move_window },
    { "kill-window",     cmd_kill_window },
    { "detach-client",   cmd_detach },
    { "copy-mode",       cmd_copymode },
    { "send-prefix",     cmd_send_prefix },
    { "send-keys",       cmd_send_keys },
    { "rename-window",   cmd_rename_window },
    { "rename-session",  cmd_rename_session },
    { "set",             cmd_set },
    { "set-option",      cmd_set },
    { "bind",            cmd_bind },
    { "bind-key",        cmd_bind },
    { "unbind",          cmd_unbind },
    { "unbind-key",      cmd_unbind },
    { "source-file",     cmd_source },
    { "command-prompt",  cmd_command_prompt },
    { "display-panes",   cmd_display_panes },
    { "choose-window",   cmd_choose_window },
    { "choose-tree",     cmd_choose_window },
    { "display-message", cmd_display_message },
    { "run-shell",       cmd_run_shell },
    { "if-shell",        cmd_if_shell },
    { "clock-mode",      cmd_clock_mode },
};

static void run_argv(session_t *s, int argc, char **argv)
{
    int i;
    if (argc == 0)
        return;
    for (i = 0; i < (int)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])); i++) {
        if (strcmp(argv[0], CMD_TABLE[i].name) == 0) {
            CMD_TABLE[i].fn(s, argc, argv);
            return;
        }
    }
}

static void session_run_command(session_t *s, const char *line)
{
    char storage[512];
    char *argv[CMD_MAX_ARGS];
    int argc, i, seg = 0;

    if (line == NULL || line[0] == '\0')
        return;
    argc = cmd_tokenize(line, storage, sizeof(storage), argv, CMD_MAX_ARGS);
    if (argc == 0)
        return;
    /* A standalone ';' token separates commands: "cmd1 args ; cmd2 args". */
    for (i = 0; i <= argc; i++) {
        if (i == argc || strcmp(argv[i], ";") == 0) {
            run_argv(s, i - seg, &argv[seg]);
            seg = i + 1;
        }
    }
}

/* Run the command bound to `keyid`, if any. Returns 1 if the binding is
 * repeatable (-r). */
static int run_key(session_t *s, int keyid)
{
    const keybind_t *b = bind_lookup(s, keyid);
    if (b) {
        session_run_command(s, b->cmd);
        return b->repeat;
    }
    return 0;
}

static void handle_prefix(session_t *s, unsigned char c)
{
    s->pstate = PS_NORMAL;
    if (c == 0x1b) {          /* an arrow-key sequence follows */
        s->pstate = PS_ESC;
        return;
    }
    if (run_key(s, (int)c))
        s->repeat_ticks = REPEAT_TICKS;   /* open the -r repeat window */
}

/* Act on one decoded SGR mouse event (x, y are 1-based screen cells). */
static void mouse_action(session_t *s, int b, int x, int y, int press)
{
    window_t *w = cur_window(s);
    int col = x - 1, row = y - 1;
    int button = b & 0x03;
    int motion = (b & 0x20) != 0;
    int wheel  = (b & 0x40) != 0;

    if (w == NULL || row < 0 || col < 0)
        return;

    if (wheel) {                          /* scroll wheel -> copy-mode scroll */
        pane_t *p = window_pane_at(w, col, row);
        int down = (b & 0x01), i;
        strbuf_t junk;
        if (p == NULL)
            return;
        if (!s->copy.active) {
            if (down) return;             /* already at the live bottom */
            copymode_enter(&s->copy, p);
        }
        strbuf_init(&junk);
        for (i = 0; i < 3; i++)
            copymode_input(&s->copy, down ? "j" : "k", 1, &junk);
        strbuf_free(&junk);
        mark(s, 1);
        return;
    }

    if (s->copy.active)                   /* copy mode is keyboard-driven */
        return;

    if (!press) {                         /* button release ends any drag */
        window_mouse_release(w);
        return;
    }
    if (motion) {                         /* drag with a button held */
        window_mouse_drag(w, col, row);
        mark(s, 1);
        return;
    }
    if (button == 0) {                    /* left press: divider or pane */
        window_mouse_press(w, col, row);
        mark(s, 1);
    }
}

/* Forward the bytes buffered by the mouse scanner to the pane (they turned out
 * not to be a mouse report). */
static void mouse_flush(session_t *s, strbuf_t *fwd)
{
    strbuf_append(fwd, s->mraw, s->mrawlen);
    s->mrawlen = 0;
    s->mstate = MN_NONE;
}

/* Feed one byte to the normal-mode SGR mouse scanner. Either it completes a
 * mouse report (consumed) or, on any mismatch, the buffered bytes are flushed
 * to the pane so ordinary escape sequences pass through untouched. */
static void feed_mouse_scanner(session_t *s, unsigned char c, strbuf_t *fwd)
{
    if (s->mrawlen >= (int)sizeof(s->mraw) - 1) {
        mouse_flush(s, fwd);
        return;
    }
    s->mraw[s->mrawlen++] = (char)c;

    switch (s->mstate) {
        case MN_ESC:
            if (c == '[') s->mstate = MN_CSI;
            else          mouse_flush(s, fwd);
            break;
        case MN_CSI:
            if (c == '<') s->mstate = MN_MOUSE;
            else          mouse_flush(s, fwd);
            break;
        case MN_MOUSE:
            if (c == 'M' || c == 'm') {
                int b = 0, x = 0, y = 0, i;
                const char *lt = NULL;
                for (i = 0; i < s->mrawlen; i++)
                    if (s->mraw[i] == '<') { lt = &s->mraw[i + 1]; break; }
                s->mraw[s->mrawlen] = '\0';
                if (lt && sscanf_s(lt, "%d;%d;%d", &b, &x, &y) == 3)
                    mouse_action(s, b, x, y, c == 'M');
                s->mstate = MN_NONE;
                s->mrawlen = 0;
            } else if (!((c >= '0' && c <= '9') || c == ';')) {
                mouse_flush(s, fwd);
            }
            break;
        default:
            s->mstate = MN_NONE;
            s->mrawlen = 0;
            break;
    }
}

/* Decode a completed CSI arrow sequence (after the prefix) into a bound key.
 * The second CSI parameter carries the modifier: ";5"=Ctrl, ";3"=Alt. */
static void handle_csi_final(session_t *s, unsigned char final)
{
    int base, mod = 0, keyid = 0;
    s->csi[s->csilen] = '\0';
    switch (final) {
        case 'A': base = 0; break;   /* Up    */
        case 'B': base = 1; break;   /* Down  */
        case 'C': base = 2; break;   /* Right */
        case 'D': base = 3; break;   /* Left  */
        default:  return;            /* e.g. '~' — not bound after prefix */
    }
    if (strstr(s->csi, ";5") || strstr(s->csi, ";6")) mod = 1;       /* Ctrl */
    else if (strstr(s->csi, ";3") || strstr(s->csi, ";4")) mod = 2;  /* Alt  */

    switch (base) {
        case 0: keyid = (mod == 1) ? KEY_C_UP    : (mod == 2) ? KEY_M_UP    : KEY_UP;    break;
        case 1: keyid = (mod == 1) ? KEY_C_DOWN  : (mod == 2) ? KEY_M_DOWN  : KEY_DOWN;  break;
        case 2: keyid = (mod == 1) ? KEY_C_RIGHT : (mod == 2) ? KEY_M_RIGHT : KEY_RIGHT; break;
        case 3: keyid = (mod == 1) ? KEY_C_LEFT  : (mod == 2) ? KEY_M_LEFT  : KEY_LEFT;  break;
    }
    run_key(s, keyid);
}

/* Install the default tmux-like key bindings. */
static void install_default_bindings(session_t *s)
{
    int i;
    char name[8], cmd[32];
    s->nbind = 0;
    bind_set(s, '%',  "split-window -h");
    bind_set(s, '"',  "split-window -v");
    bind_set(s, 'o',  "select-pane");
    bind_set(s, 'x',  "kill-pane");
    bind_set(s, 'c',  "new-window");
    bind_set(s, 'n',  "next-window");
    bind_set(s, 'p',  "previous-window");
    bind_set(s, '&',  "kill-window");
    bind_set(s, '[',  "copy-mode");
    bind_set(s, 'd',  "detach-client");
    bind_set(s, ':',  "command-prompt");
    bind_set(s, 'q',  "display-panes");
    bind_set(s, 'w',  "choose-window");
    bind_set(s, 't',  "clock-mode");
    bind_set(s, 'z',  "resize-pane -Z");
    bind_set(s, ' ',  "next-layout");
    bind_set(s, '{',  "swap-pane -U");
    bind_set(s, '}',  "swap-pane -D");
    bind_set(s, '!',  "break-pane");
    bind_set(s, ']',  "paste-buffer");
    bind_set(s, ';',  "last-pane");
    bind_set(s, 'l',  "last-window");
    bind_set(s, 0x0f, "rotate-window");   /* Ctrl-O */
    bind_set(s, s->prefix_key, "send-prefix");
    bind_set(s, KEY_UP,    "select-pane -U");
    bind_set(s, KEY_DOWN,  "select-pane -D");
    bind_set(s, KEY_LEFT,  "select-pane -L");
    bind_set(s, KEY_RIGHT, "select-pane -R");
    bind_set(s, KEY_C_UP,    "resize-pane -U 1");
    bind_set(s, KEY_C_DOWN,  "resize-pane -D 1");
    bind_set(s, KEY_C_LEFT,  "resize-pane -L 1");
    bind_set(s, KEY_C_RIGHT, "resize-pane -R 1");
    bind_set(s, KEY_M_UP,    "resize-pane -U 5");
    bind_set(s, KEY_M_DOWN,  "resize-pane -D 5");
    bind_set(s, KEY_M_LEFT,  "resize-pane -L 5");
    bind_set(s, KEY_M_RIGHT, "resize-pane -R 5");
    for (i = 0; i <= 9; i++) {
        _snprintf_s(name, sizeof(name), _TRUNCATE, "%d", i);
        _snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "select-window -t %d", i);
        bind_set(s, name[0], cmd);
    }
}

/* ----- public API ----------------------------------------------------------- */

session_t *session_create(const wchar_t *shell, int cols, int rows, HANDLE wake)
{
    session_t *s;
    window_t *w;
    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 25;

    s = (session_t *)calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->wake = wake;
    s->shell = shell;
    s->cols = cols;
    s->rows = rows;
    s->last_min = -1;
    s->last_window = -1;
    s->prefix_key = PREFIX_KEY;
    s->status_on = 1;
    strcpy_s(s->status_left, sizeof(s->status_left), "[#S] ");
    strcpy_s(s->status_right, sizeof(s->status_right), "%H:%M");
    strcpy_s(s->name, sizeof(s->name), "0");
    install_default_bindings(s);

    w = window_create(shell, cols, win_area_rows(s), wake);
    if (w == NULL) {
        free(s);
        return NULL;
    }
    s->windows[0] = w;
    s->nwindows = 1;
    s->cur = 0;
    s->full_redraw = 1;
    s->changed = 1;
    return s;
}

void session_free(session_t *s)
{
    int i;
    if (s == NULL)
        return;
    for (i = 0; i < s->nwindows; i++)
        window_free(s->windows[i]);
    free(s);
}

void session_input(session_t *s, const char *bytes, size_t n)
{
    strbuf_t fwd;
    size_t i;

    /* The command prompt captures all input. */
    if (s->prompt_active) {
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)bytes[i];
            if (c == '\r' || c == '\n') {
                s->prompt[s->prompt_len] = '\0';
                s->prompt_active = 0;
                session_run_command(s, s->prompt);
            } else if (c == 0x1b) {
                s->prompt_active = 0;      /* cancel */
            } else if (c == 0x7f || c == 0x08) {
                if (s->prompt_len > 0) s->prompt_len--;
            } else if (c >= 0x20 && c < 0x7f && s->prompt_len < PROMPT_MAX - 1) {
                s->prompt[s->prompt_len++] = (char)c;
            }
        }
        mark(s, 1);
        return;
    }

    /* Clock mode: any key returns to the live view. */
    if (s->clock_mode && n > 0) {
        s->clock_mode = 0;
        mark(s, 1);
        return;
    }

    /* choose-window picker: navigate the list, Enter selects, q/Esc cancels. */
    if (s->choose_active && n > 0) {
        unsigned char c = (unsigned char)bytes[0];
        int nw = s->nwindows;
        if (c == 0x1b) {
            if (n >= 3 && bytes[1] == '[') {
                if (bytes[2] == 'A' && s->choose_sel > 0) s->choose_sel--;
                else if (bytes[2] == 'B' && s->choose_sel < nw - 1) s->choose_sel++;
            } else {
                s->choose_active = 0;               /* lone Esc cancels */
            }
        } else if ((c == 'j' || c == 0x0e) && s->choose_sel < nw - 1) {
            s->choose_sel++;
        } else if ((c == 'k' || c == 0x10) && s->choose_sel > 0) {
            s->choose_sel--;
        } else if (c == 'g') {
            s->choose_sel = 0;
        } else if (c == 'G') {
            s->choose_sel = nw - 1;
        } else if (c == '\r' || c == '\n') {
            int t = s->choose_sel; s->choose_active = 0; select_window(s, t);
        } else if (c == 'q') {
            s->choose_active = 0;
        } else if (c >= '0' && c <= '9') {
            int t = (int)(c - '0') - s->base_index;
            if (t >= 0 && t < nw) { s->choose_active = 0; select_window(s, t); }
        }
        mark(s, 1);
        return;
    }

    /* While the pane-number overlay is up, the next key dismisses it; a digit
     * also selects that pane. */
    if (s->show_panes > 0 && n > 0) {
        unsigned char c = (unsigned char)bytes[0];
        window_t *w = cur_window(s);
        s->show_panes = 0;
        if (w && c >= '0' && c <= '9')
            window_select_index(w, (int)(c - '0') - s->pane_base_index);
        mark(s, 1);
        return;
    }

    /* In copy mode all input drives navigation/selection, not the pane. */
    if (s->copy.active) {
        strbuf_t text;
        strbuf_init(&text);
        if (copymode_input(&s->copy, bytes, n, &text) && text.len)
            clipboard_set_utf8(text.data, text.len);
        strbuf_free(&text);
        mark(s, 1);           /* repaint the viewport (or the restored live view) */
        return;
    }

    strbuf_init(&fwd);

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        switch (s->pstate) {
            case PS_NORMAL:
                if (s->repeat_ticks > 0) {
                    const keybind_t *b = bind_lookup(s, (int)c);
                    if (b && b->repeat) {            /* -r key without prefix */
                        session_run_command(s, b->cmd);
                        s->repeat_ticks = REPEAT_TICKS;
                        break;
                    }
                    s->repeat_ticks = 0;             /* any other key ends it */
                }
                if (s->mouse_on && s->mstate != MN_NONE) {
                    feed_mouse_scanner(s, c, &fwd);
                } else if ((int)c == s->prefix_key) {
                    window_t *w = cur_window(s);
                    if (fwd.len && w) {
                        window_write_active(w, fwd.data, fwd.len);
                        strbuf_clear(&fwd);
                    }
                    s->pstate = PS_PREFIX;
                } else if (s->mouse_on && c == 0x1b) {
                    s->mstate = MN_ESC;       /* maybe an SGR mouse report */
                    s->mrawlen = 0;
                    s->mraw[s->mrawlen++] = (char)c;
                } else {
                    strbuf_putc(&fwd, (char)c);
                }
                break;
            case PS_PREFIX:
                handle_prefix(s, c);
                break;
            case PS_ESC:
                if (c == '[') { s->csilen = 0; s->pstate = PS_CSI; }
                else            s->pstate = PS_NORMAL;
                break;
            case PS_CSI:
                if (c >= 0x40 && c <= 0x7e) {          /* final byte */
                    handle_csi_final(s, c);
                    s->pstate = PS_NORMAL;
                } else if (s->csilen < (int)sizeof(s->csi) - 1) {
                    s->csi[s->csilen++] = (char)c;     /* parameter/intermediate */
                }
                break;
        }
    }

    {
        window_t *w = cur_window(s);
        if (fwd.len && w)
            window_write_active(w, fwd.data, fwd.len);
    }
    strbuf_free(&fwd);
}

void session_resize(session_t *s, int cols, int rows)
{
    int i, h;
    if (cols <= 0) cols = 1;
    if (rows <= 0) rows = 1;
    if (cols == s->cols && rows == s->rows)
        return;
    s->cols = cols;
    s->rows = rows;
    h = win_area_rows(s);
    for (i = 0; i < s->nwindows; i++)
        window_apply(s->windows[i], cols, h);
    mark(s, 1);
}

void session_pump(session_t *s)
{
    int i = 0;
    while (i < s->nwindows) {
        size_t parsed = window_pump(s->windows[i]);
        if (window_empty(s->windows[i])) {
            remove_window(s, i);
            if (s->nwindows == 0)
                return;
            continue;   /* the next window shifted into slot i */
        }
        if (i == s->cur && parsed > 0)
            s->changed = 1;
        i++;
    }
    sync_copy(s);
}

void session_tick(session_t *s)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    if ((int)st.wMinute != s->last_min) {
        s->last_min = (int)st.wMinute;
        s->changed = 1;
    }
    if (s->show_panes > 0 && --s->show_panes == 0)
        mark(s, 1);            /* overlay expired: repaint to clear it */
    if (s->repeat_ticks > 0)
        s->repeat_ticks--;     /* close the -r window over time */
    if (s->message_ticks > 0 && --s->message_ticks == 0)
        mark(s, 1);            /* message expired: repaint the status bar */
}

/* Expand a status format string into `out`: #S session, #W/#I current window,
 * #H host, ## literal '#'; %H %M %S %Y %m %d time fields. */
static void expand_status(session_t *s, const char *fmt, char *out, size_t cap)
{
    SYSTEMTIME st;
    size_t o = 0;
    const char *p = fmt;
    window_t *w = cur_window(s);
    GetLocalTime(&st);

    while (*p && o + 1 < cap) {
        char tmp[64];
        const char *ins = NULL;
        if (*p == '#' && p[1]) {
            p++;
            switch (*p) {
                case 'S': ins = s->name; break;
                case 'W': ins = w ? w->name : ""; break;
                case 'I': _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%d", s->cur + s->base_index); ins = tmp; break;
                case 'H': case 'h': {
                    DWORD n = (DWORD)sizeof(tmp);
                    if (!GetComputerNameA(tmp, &n)) tmp[0] = '\0';
                    ins = tmp; break;
                }
                case '#': tmp[0] = '#'; tmp[1] = '\0'; ins = tmp; break;
                default:  tmp[0] = '#'; tmp[1] = *p; tmp[2] = '\0'; ins = tmp; break;
            }
            p++;
        } else if (*p == '%' && p[1]) {
            p++;
            switch (*p) {
                case 'H': _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02d", st.wHour); break;
                case 'M': _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02d", st.wMinute); break;
                case 'S': _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02d", st.wSecond); break;
                case 'Y': _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%04d", st.wYear); break;
                case 'm': _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02d", st.wMonth); break;
                case 'd': _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%02d", st.wDay); break;
                default:  tmp[0] = '%'; tmp[1] = *p; tmp[2] = '\0'; break;
            }
            ins = tmp;
            p++;
        } else {
            out[o++] = *p++;
            continue;
        }
        while (ins && *ins && o + 1 < cap)
            out[o++] = *ins++;
    }
    out[o] = '\0';
}

static void render_status(session_t *s, strbuf_t *frame)
{
    status_win_t wins[MAX_WINDOWS];
    char left[128], right[128];
    int i;

    for (i = 0; i < s->nwindows; i++) {
        wins[i].index = i + s->base_index;
        wins[i].name = s->windows[i]->name;
        wins[i].current = (i == s->cur);
    }
    expand_status(s, s->status_left, left, sizeof(left));
    expand_status(s, s->status_right, right, sizeof(right));

    status_render(frame, s->cols, s->rows, left, wins, s->nwindows, right);
}

/* Draw the choose-window picker: one window per row, the selection highlighted. */
static void render_choose(session_t *s, strbuf_t *frame)
{
    int i;
    for (i = 0; i < s->nwindows && i < s->rows - 1; i++) {
        char line[96];
        _snprintf_s(line, sizeof(line), _TRUNCATE, " %d: %-20s ",
                    i + s->base_index, s->windows[i]->name);
        strbuf_printf(frame, "\x1b[%d;1H%s%s\x1b[0m",
                      i + 1, (i == s->choose_sel) ? "\x1b[7m" : "\x1b[1m", line);
    }
}

/* Draw a transient message across the status row (reverse video, full width). */
static void render_message(session_t *s, strbuf_t *frame)
{
    int i, len = (int)strlen(s->message);
    strbuf_printf(frame, "\x1b[%d;1H\x1b[7m", s->rows);
    for (i = 0; i < s->cols; i++)
        strbuf_putc(frame, i < len ? s->message[i] : ' ');
    strbuf_append(frame, "\x1b[0m", 4);
}

/* Render the command prompt line at the bottom row (cursor after the text). */
static void render_prompt(session_t *s, strbuf_t *frame)
{
    strbuf_printf(frame, "\x1b[%d;1H\x1b[0m\x1b[K:%.*s", s->rows, s->prompt_len, s->prompt);
    strbuf_printf(frame, "\x1b[%d;%dH\x1b[?25h", s->rows, s->prompt_len + 2);
}

void session_render(session_t *s, strbuf_t *frame)
{
    window_t *w;
    strbuf_clear(frame);
    if (!s->changed || s->nwindows == 0)
        return;

    /* Enable/disable terminal mouse reporting when it changes or on a full
     * repaint (so a freshly attached client re-enables it). */
    if (s->mouse_dirty || (s->mouse_on && s->full_redraw)) {
        strbuf_append(frame,
                      s->mouse_on ? "\x1b[?1000h\x1b[?1002h\x1b[?1006h"
                                  : "\x1b[?1000l\x1b[?1002l\x1b[?1006l", 24);
        s->mouse_dirty = 0;
    }

    w = s->windows[s->cur];
    if (s->full_redraw)
        render_clear(frame);

    if (s->status_on)
        render_status(s, frame);       /* draw bar first */
    window_render(frame, w, s->full_redraw, &s->copy); /* panes + cursor last */
    if (s->clock_mode) {
        pane_t *ap = window_active(w);
        if (ap) clock_draw(frame, ap->x, ap->y, ap->cols, ap->rows);
    }
    if (s->show_panes > 0)
        window_display_panes(frame, w, s->pane_base_index);
    if (s->choose_active)
        render_choose(s, frame);       /* window picker overlay */
    if (s->message_ticks > 0)
        render_message(s, frame);      /* transient message over the status row */
    if (s->prompt_active)
        render_prompt(s, frame);       /* prompt overrides the bar + cursor */

    s->full_redraw = 0;
    s->changed = 0;
}

void session_force_redraw(session_t *s)
{
    s->full_redraw = 1;
    s->changed = 1;
}

int session_alive(const session_t *s)
{
    return !s->quit && s->nwindows > 0;
}

int session_take_detach(session_t *s)
{
    int d = s->detach;
    s->detach = 0;
    return d;
}

/* Run each non-comment line of a config file as a command. */
static void session_load_config_path(session_t *s, const char *path)
{
    FILE *f = NULL;
    char line[512];
    if (fopen_s(&f, path, "r") != 0 || f == NULL)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r'))
            p[--len] = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            continue;
        session_run_command(s, p);
    }
    fclose(f);
}

void session_run(session_t *s, const char *cmdline)
{
    session_run_command(s, cmdline);
}

void session_load_config(session_t *s)
{
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("USERPROFILE", path, sizeof(path));
    if (n > 0 && n < sizeof(path)) {
        strcat_s(path, sizeof(path), "\\.tmuxw.conf");
        session_load_config_path(s, path);
    }
}

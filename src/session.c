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

#define MAX_BUFFERS 16
#define MAX_ENV     32

typedef struct {
    char    name[32];
    char   *data;
    size_t  len;
} paste_buf_t;

typedef struct {
    char name[64];
    char value[512];
} env_var_t;

#define MAX_HOOKS  16
#define MAX_WAITCH 16

typedef struct {
    char event[32];
    char cmd[BIND_CMD_MAX];
} hook_t;

typedef struct {
    char name[64];
    int  signaled;
} wait_chan_t;

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
    int             choose_buf_active; /* choose-buffer picker open */
    int             choose_buf_sel;    /* highlighted buffer in the picker */
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
    /* Text produced by a "listing" command (list-windows, list-panes) for
     * session_run_capture to retrieve; empty after any other command. */
    char            cmd_result[2048];
    /* Named paste buffers (list-buffers/show-buffer/set-buffer/delete-buffer/
     * load-buffer/save-buffer, and copy-mode yanks). Oldest is evicted once
     * full; paste-buffer/show-buffer default to the newest (top). */
    paste_buf_t     buffers[MAX_BUFFERS];
    int             nbuffers;
    int             next_buf_id;    /* for auto-named "bufferN" */
    /* confirm-before: a y/n gate in front of one command. */
    int             confirm_active;
    char            confirm_cmd[BIND_CMD_MAX];
    /* Whether an interactive client is attached right now (list-clients). */
    int             attached;
    /* set-environment overrides, applied to every subsequently created
     * pane's environment (see build_env_block). */
    env_var_t       env[MAX_ENV];
    int             nenv;
    /* set-hook: a command to run when a named event fires (see fire_hook).
     * Events actually fired by this build: window-linked, pane-died,
     * client-attached, client-detached -- but set-hook/show-hooks accept and
     * store any event name, same as tmux. */
    hook_t          hooks[MAX_HOOKS];
    int             nhooks;
    /* wait-for: named channels signalled by `wait-for -S` and consumed by a
     * plain `wait-for` (the CLI polls session_run_capture for this rather
     * than the server blocking its single command-execution thread). */
    wait_chan_t     waitch[MAX_WAITCH];
    int             nwaitch;
};

static void session_run_command(session_t *s, const char *line);
static void session_load_config_path(session_t *s, const char *path);
static void expand_status(session_t *s, const char *fmt, char *out, size_t cap);
static wchar_t *build_env_block(session_t *s);
static void fire_hook(session_t *s, const char *event);

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
    wchar_t *envblock;
    if (s->nwindows >= MAX_WINDOWS)
        return;
    envblock = build_env_block(s);
    w = window_create(s->shell, s->cols, win_area_rows(s), s->wake, NULL, envblock);
    free(envblock);
    if (w == NULL)
        return;
    s->windows[s->nwindows] = w;
    s->cur = s->nwindows;
    s->nwindows++;
    mark(s, 1);
    fire_hook(s, "window-linked");
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
    if (w) {
        wchar_t *envblock = build_env_block(s);
        window_split(w, type, s->shell, s->wake, envblock);
        free(envblock);
        mark(s, 1);
    }
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
    int i, all = 0;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-a") == 0) all = 1;
    if (w == NULL)
        return;
    if (all) { window_kill_others(w); mark(s, 1); return; }
    if (window_kill_active(w)) remove_window(s, s->cur);
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

/* ----- named paste buffers --------------------------------------------------
 *
 * A small internal buffer stack, separate from (but kept in sync with) the
 * Windows clipboard: copy-mode yanks and capture-pane push onto it, and
 * list-buffers/show-buffer/set-buffer/delete-buffer/load-buffer/save-buffer
 * all operate on it. paste-buffer prefers the top of this stack, falling
 * back to the clipboard so a plain OS-level copy still pastes as before. */

static void buffer_push(session_t *s, const char *name, const char *data, size_t len)
{
    paste_buf_t *b;
    int i;
    for (i = 0; i < s->nbuffers; i++) {
        if (strcmp(s->buffers[i].name, name) == 0) {
            free(s->buffers[i].data);
            s->buffers[i].data = (char *)malloc(len + 1);
            if (s->buffers[i].data) {
                memcpy(s->buffers[i].data, data, len);
                s->buffers[i].data[len] = '\0';
            }
            s->buffers[i].len = len;
            return;
        }
    }
    if (s->nbuffers >= MAX_BUFFERS) {
        free(s->buffers[0].data);
        memmove(&s->buffers[0], &s->buffers[1], (size_t)(MAX_BUFFERS - 1) * sizeof(paste_buf_t));
        s->nbuffers--;
    }
    b = &s->buffers[s->nbuffers++];
    strcpy_s(b->name, sizeof(b->name), name);
    b->data = (char *)malloc(len + 1);
    if (b->data) {
        memcpy(b->data, data, len);
        b->data[len] = '\0';
    }
    b->len = len;
}

static void buffer_push_auto(session_t *s, const char *data, size_t len)
{
    char name[32];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "buffer%d", s->next_buf_id++);
    buffer_push(s, name, data, len);
}

static paste_buf_t *buffer_find(session_t *s, const char *name)
{
    int i;
    if (name == NULL)
        return s->nbuffers > 0 ? &s->buffers[s->nbuffers - 1] : NULL;
    for (i = 0; i < s->nbuffers; i++)
        if (strcmp(s->buffers[i].name, name) == 0)
            return &s->buffers[i];
    return NULL;
}

static void buffer_delete(session_t *s, const char *name)
{
    int i, idx = -1;
    if (name == NULL) {
        idx = s->nbuffers - 1;
    } else {
        for (i = 0; i < s->nbuffers; i++)
            if (strcmp(s->buffers[i].name, name) == 0) { idx = i; break; }
    }
    if (idx < 0)
        return;
    free(s->buffers[idx].data);
    memmove(&s->buffers[idx], &s->buffers[idx + 1],
           (size_t)(s->nbuffers - idx - 1) * sizeof(paste_buf_t));
    s->nbuffers--;
}

static void buffers_free_all(session_t *s)
{
    int i;
    for (i = 0; i < s->nbuffers; i++)
        free(s->buffers[i].data);
    s->nbuffers = 0;
}

/* ----- set-environment ------------------------------------------------------
 *
 * A small table of session-level environment overrides, applied to every
 * pane created from here on (new-window, split-window) by build_env_block,
 * which starts from the server's own environment and replaces/adds these on
 * top of it -- existing panes are unaffected, matching tmux. */

static void env_set(session_t *s, const char *name, const char *value)
{
    int i;
    for (i = 0; i < s->nenv; i++) {
        if (_stricmp(s->env[i].name, name) == 0) {
            strncpy_s(s->env[i].value, sizeof(s->env[i].value), value, _TRUNCATE);
            return;
        }
    }
    if (s->nenv >= MAX_ENV)
        return;
    strncpy_s(s->env[s->nenv].name, sizeof(s->env[s->nenv].name), name, _TRUNCATE);
    strncpy_s(s->env[s->nenv].value, sizeof(s->env[s->nenv].value), value, _TRUNCATE);
    s->nenv++;
}

static void env_unset(session_t *s, const char *name)
{
    int i;
    for (i = 0; i < s->nenv; i++) {
        if (_stricmp(s->env[i].name, name) == 0) {
            memmove(&s->env[i], &s->env[i + 1], (size_t)(s->nenv - i - 1) * sizeof(env_var_t));
            s->nenv--;
            return;
        }
    }
}

/* Build a GetEnvironmentStringsW-style block combining the current process
 * environment with this session's set-environment overrides (which replace
 * any base entry of the same name). Returns a malloc'd block the caller
 * frees, or NULL when there are no overrides at all -- the common case,
 * meaning "just inherit" -- or on allocation failure. */
static wchar_t *build_env_block(session_t *s)
{
    wchar_t *base, *result, *w;
    size_t cap, used = 0;
    int i;

    if (s->nenv == 0)
        return NULL;

    base = GetEnvironmentStringsW();
    if (base == NULL)
        return NULL;

    cap = 8192;
    result = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (result == NULL) {
        FreeEnvironmentStringsW(base);
        return NULL;
    }

    w = base;
    while (*w) {
        size_t len = wcslen(w);
        int skip = 0;
        wchar_t *eq = wcschr(w, L'=');
        if (eq && eq != w) {   /* a leading '=' marks a drive-letter pseudo-var; always keep those */
            char nname[64];
            size_t nlen = (size_t)(eq - w);
            if (nlen < sizeof(nname)) {
                WideCharToMultiByte(CP_UTF8, 0, w, (int)nlen, nname, (int)sizeof(nname) - 1, NULL, NULL);
                nname[nlen] = '\0';
                for (i = 0; i < s->nenv; i++)
                    if (_stricmp(s->env[i].name, nname) == 0) { skip = 1; break; }
            }
        }
        if (!skip) {
            if (used + len + 2 > cap) {
                wchar_t *bigger;
                cap *= 2;
                bigger = (wchar_t *)realloc(result, cap * sizeof(wchar_t));
                if (bigger == NULL) { free(result); FreeEnvironmentStringsW(base); return NULL; }
                result = bigger;
            }
            memcpy(result + used, w, (len + 1) * sizeof(wchar_t));
            used += len + 1;
        }
        w += len + 1;
    }
    FreeEnvironmentStringsW(base);

    for (i = 0; i < s->nenv; i++) {
        wchar_t entry[600], wname[64], wvalue[512];
        int n;
        MultiByteToWideChar(CP_UTF8, 0, s->env[i].name, -1, wname, 64);
        MultiByteToWideChar(CP_UTF8, 0, s->env[i].value, -1, wvalue, 512);
        n = _snwprintf_s(entry, 600, _TRUNCATE, L"%s=%s", wname, wvalue);
        if (n > 0) {
            size_t elen = (size_t)n;
            if (used + elen + 2 > cap) {
                wchar_t *bigger;
                cap = (used + elen + 2) * 2;
                bigger = (wchar_t *)realloc(result, cap * sizeof(wchar_t));
                if (bigger == NULL) { free(result); return NULL; }
                result = bigger;
            }
            memcpy(result + used, entry, (elen + 1) * sizeof(wchar_t));
            used += elen + 1;
        }
    }
    result[used++] = L'\0';   /* the block's final terminating NUL */
    return result;
}

/* ----- set-hook --------------------------------------------------------- */

static void hook_set(session_t *s, const char *event, const char *cmd)
{
    int i;
    for (i = 0; i < s->nhooks; i++) {
        if (strcmp(s->hooks[i].event, event) == 0) {
            strncpy_s(s->hooks[i].cmd, sizeof(s->hooks[i].cmd), cmd, _TRUNCATE);
            return;
        }
    }
    if (s->nhooks >= MAX_HOOKS)
        return;
    strncpy_s(s->hooks[s->nhooks].event, sizeof(s->hooks[s->nhooks].event), event, _TRUNCATE);
    strncpy_s(s->hooks[s->nhooks].cmd, sizeof(s->hooks[s->nhooks].cmd), cmd, _TRUNCATE);
    s->nhooks++;
}

static void hook_unset(session_t *s, const char *event)
{
    int i;
    for (i = 0; i < s->nhooks; i++) {
        if (strcmp(s->hooks[i].event, event) == 0) {
            memmove(&s->hooks[i], &s->hooks[i + 1], (size_t)(s->nhooks - i - 1) * sizeof(hook_t));
            s->nhooks--;
            return;
        }
    }
}

/* Run the command registered for `event`, if any. Events actually raised by
 * this build: window-linked (new-window), pane-died, client-attached,
 * client-detached. set-hook/show-hooks accept and store any event name
 * (matching tmux), but only those four ever get fired automatically. */
static void fire_hook(session_t *s, const char *event)
{
    int i;
    for (i = 0; i < s->nhooks; i++) {
        if (strcmp(s->hooks[i].event, event) == 0) {
            session_run_command(s, s->hooks[i].cmd);
            return;
        }
    }
}

/* ----- wait-for ---------------------------------------------------------
 *
 * wait-for's blocking wait is deliberately NOT implemented by blocking the
 * server: this session's single command-execution path also drives
 * rendering and input for any attached client, so blocking it would freeze
 * the whole session for however long the wait takes. Instead, a plain
 * `wait-for <channel>` just reports whether the channel has been signalled
 * yet (see cmd_wait_for's "OK"/"PENDING" result); the CLI (main.c) is the
 * one that polls. */

static wait_chan_t *waitch_find(session_t *s, const char *name)
{
    int i;
    for (i = 0; i < s->nwaitch; i++)
        if (strcmp(s->waitch[i].name, name) == 0)
            return &s->waitch[i];
    return NULL;
}

static void waitch_signal(session_t *s, const char *name)
{
    wait_chan_t *c = waitch_find(s, name);
    if (c) { c->signaled = 1; return; }
    if (s->nwaitch >= MAX_WAITCH)
        return;
    strncpy_s(s->waitch[s->nwaitch].name, sizeof(s->waitch[s->nwaitch].name), name, _TRUNCATE);
    s->waitch[s->nwaitch].signaled = 1;
    s->nwaitch++;
}

/* Extract argv[i]'s "-b <name>" if present anywhere; returns the first
 * non-flag argv token seen (position-independent, unlike the CLI's -t
 * convention) as *rest, or NULL. */
static const char *find_named_arg(int argc, char **argv, const char **rest)
{
    const char *name = NULL;
    int i;
    if (rest) *rest = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (rest && *rest == NULL) {
            *rest = argv[i];
        }
    }
    return name;
}

static void cmd_list_buffers(session_t *s, int argc, char **argv)
{
    int i, o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    for (i = 0; i < s->nbuffers && o < (int)sizeof(s->cmd_result) - 96; i++) {
        int n = _snprintf_s(s->cmd_result + o, sizeof(s->cmd_result) - o, _TRUNCATE,
                            "%s: %zu bytes\n", s->buffers[i].name, s->buffers[i].len);
        if (n < 0) break;
        o += n;
    }
}

static void cmd_show_buffer(session_t *s, int argc, char **argv)
{
    const char *name = find_named_arg(argc, argv, NULL);
    paste_buf_t *b = buffer_find(s, name);
    s->cmd_result[0] = '\0';
    if (b && b->data) {
        size_t n = b->len < sizeof(s->cmd_result) - 1 ? b->len : sizeof(s->cmd_result) - 1;
        memcpy(s->cmd_result, b->data, n);
        s->cmd_result[n] = '\0';
    }
}

static void cmd_set_buffer(session_t *s, int argc, char **argv)
{
    const char *text = NULL;
    const char *name = find_named_arg(argc, argv, &text);
    if (text == NULL)
        return;
    if (name) buffer_push(s, name, text, strlen(text));
    else      buffer_push_auto(s, text, strlen(text));
}

static void cmd_delete_buffer(session_t *s, int argc, char **argv)
{
    buffer_delete(s, find_named_arg(argc, argv, NULL));
}

static void cmd_load_buffer(session_t *s, int argc, char **argv)
{
    const char *path = NULL;
    const char *name = find_named_arg(argc, argv, &path);
    FILE *f = NULL;
    char data[4096];
    size_t n;
    if (path == NULL || fopen_s(&f, path, "rb") != 0 || f == NULL)
        return;
    n = fread(data, 1, sizeof(data), f);
    fclose(f);
    if (name) buffer_push(s, name, data, n);
    else      buffer_push_auto(s, data, n);
}

static void cmd_save_buffer(session_t *s, int argc, char **argv)
{
    const char *path = NULL;
    const char *name = find_named_arg(argc, argv, &path);
    paste_buf_t *b = buffer_find(s, name);
    FILE *f = NULL;
    if (path == NULL || b == NULL || b->data == NULL)
        return;
    if (fopen_s(&f, path, "wb") != 0 || f == NULL)
        return;
    fwrite(b->data, 1, b->len, f);
    fclose(f);
}

static void cmd_paste_buffer(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    const char *name = find_named_arg(argc, argv, NULL);
    paste_buf_t *b = buffer_find(s, name);
    size_t len = 0;
    char *buf = NULL, *owned = NULL;
    if (w == NULL)
        return;
    if (b && b->data) {
        buf = b->data;
        len = b->len;
    } else {
        owned = clipboard_get_utf8(&len);
        buf = owned;
    }
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
    free(owned);
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

/* choose-buffer: navigate the paste-buffer stack (like choose-window);
 * Enter pastes the highlighted buffer into the active pane. */
static void cmd_choose_buffer(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s->nbuffers == 0)
        return;
    s->choose_buf_active = 1;
    s->choose_buf_sel = s->nbuffers - 1;   /* newest first */
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

/* list-windows / lsw: one line per window, "<index>: <name> (<n> panes)",
 * with " (active)" on the current one. Written to s->cmd_result for
 * session_run_capture. */
static void cmd_list_windows(session_t *s, int argc, char **argv)
{
    int i, o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    for (i = 0; i < s->nwindows && o < (int)sizeof(s->cmd_result) - 96; i++) {
        pane_t *leaves[TMUXW_MAX_PANES];
        int npanes = s->windows[i]->root ? layout_collect(s->windows[i]->root, leaves, TMUXW_MAX_PANES) : 0;
        int n = _snprintf_s(s->cmd_result + o, sizeof(s->cmd_result) - o, _TRUNCATE,
                            "%d: %s (%d panes)%s\n",
                            i + s->base_index, s->windows[i]->name, npanes,
                            (i == s->cur) ? " (active)" : "");
        if (n < 0) break;
        o += n;
    }
}

/* list-panes / lsp: one line per pane of the current window, numbered the
 * same way select-pane -t / display-panes do (traversal index +
 * pane-base-index, not the pane's internal creation-order id). */
static void cmd_list_panes(session_t *s, int argc, char **argv)
{
    window_t *w = (s->cur >= 0 && s->cur < s->nwindows) ? s->windows[s->cur] : NULL;
    pane_t *leaves[TMUXW_MAX_PANES];
    int count, i, o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    if (w == NULL || w->root == NULL)
        return;
    count = layout_collect(w->root, leaves, TMUXW_MAX_PANES);
    for (i = 0; i < count && o < (int)sizeof(s->cmd_result) - 64; i++) {
        pane_t *p = leaves[i];
        int n = _snprintf_s(s->cmd_result + o, sizeof(s->cmd_result) - o, _TRUNCATE,
                            "%d: [%dx%d]%s\n",
                            i + s->pane_base_index, p->cols, p->rows,
                            (p == w->active) ? " (active)" : "");
        if (n < 0) break;
        o += n;
    }
}

/* list-keys / lsk: one line per binding, "bind-key [-r] <key> <command>". */
static void cmd_list_keys(session_t *s, int argc, char **argv)
{
    int i, o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    for (i = 0; i < s->nbind && o < (int)sizeof(s->cmd_result) - 128; i++) {
        char keyname[32];
        int n;
        cmd_key_name(s->bind[i].key, keyname, sizeof(keyname));
        n = _snprintf_s(s->cmd_result + o, sizeof(s->cmd_result) - o, _TRUNCATE,
                        "bind-key%s %s %s\n", s->bind[i].repeat ? " -r" : "",
                        keyname, s->bind[i].cmd);
        if (n < 0) break;
        o += n;
    }
}

/* show-options / show-window-options / show: current values of every option
 * `set`/`setw` can change. */
static void cmd_show_options(session_t *s, int argc, char **argv)
{
    char pfx[16];
    (void)argc; (void)argv;
    cmd_key_name(s->prefix_key, pfx, sizeof(pfx));
    s->cmd_result[0] = '\0';
    _snprintf_s(s->cmd_result, sizeof(s->cmd_result), _TRUNCATE,
               "prefix %s\n"
               "status %s\n"
               "mouse %s\n"
               "base-index %d\n"
               "pane-base-index %d\n"
               "status-left \"%s\"\n"
               "status-right \"%s\"\n",
               pfx, s->status_on ? "on" : "off", s->mouse_on ? "on" : "off",
               s->base_index, s->pane_base_index, s->status_left, s->status_right);
}

/* capture-pane / capturep: the active pane's full scrollback + visible grid,
 * written to cmd_result (so `tmux capture-pane -t work` prints it directly)
 * and also pushed onto the paste-buffer stack, matching tmux's default of
 * leaving a new buffer behind. */
static void cmd_capture_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    screen_t *sc;
    int total, cols, line, o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    if (w == NULL || w->active == NULL || w->active->screen == NULL)
        return;
    sc = w->active->screen;
    total = screen_total_lines(sc);
    cols = screen_cols(sc);
    for (line = 0; line < total && o < (int)sizeof(s->cmd_result) - cols - 2; line++) {
        int col, linestart = o, lineend;
        for (col = 0; col < cols; col++) {
            VTermScreenCell cell;
            char ch = ' ';
            if (screen_line_cell(sc, line, col, &cell) && cell.width != 0 && cell.chars[0])
                ch = (cell.chars[0] < 128) ? (char)cell.chars[0] : ' ';
            s->cmd_result[o++] = ch;
        }
        lineend = o;
        while (lineend > linestart && s->cmd_result[lineend - 1] == ' ')
            lineend--;
        o = lineend;
        s->cmd_result[o++] = '\n';
    }
    s->cmd_result[o] = '\0';
    if (o > 0)
        buffer_push_auto(s, s->cmd_result, (size_t)o);
}

/* clear-history / clearhist: drop the active pane's retained scrollback. */
static void cmd_clear_history(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w && w->active && w->active->screen) {
        screen_clear_history(w->active->screen);
        mark(s, 1);
    }
}

static void cmd_previous_layout(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w) { window_previous_layout(w); mark(s, 1); }
}

/* confirm-before -p "message" <command>: gate a (usually destructive) command
 * behind a y/n prompt; see the confirm_active handling in session_input. */
static void cmd_confirm_before(session_t *s, int argc, char **argv)
{
    const char *msg = "confirm? (y/n)";
    const char *cmd = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) msg = argv[++i];
        else cmd = argv[i];
    }
    if (cmd == NULL)
        return;
    s->confirm_active = 1;
    strncpy_s(s->confirm_cmd, sizeof(s->confirm_cmd), cmd, _TRUNCATE);
    show_message(s, msg);
    s->message_ticks = 1200;   /* pinned until answered, not the usual ~2s */
}

static void cmd_respawn_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w) { window_respawn_active(w); mark(s, 1); }
}

static void cmd_respawn_window(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    (void)argc; (void)argv;
    if (w) { window_respawn_all(w); mark(s, 1); }
}

/* pipe-pane [-o] [<shell-command>]: mirror the active pane's raw output to a
 * spawned shell command's stdin (e.g. `pipe-pane 'cat >> log.txt'`), until
 * toggled off by running it again with -o or no command. */
static void cmd_pipe_pane(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    const char *cmdline = NULL;
    int i, off = 0;
    if (w == NULL || w->active == NULL)
        return;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) off = 1;
        else cmdline = argv[i];
    }
    if (off || cmdline == NULL)
        pane_pipe_stop(w->active);
    else
        pane_pipe_start(w->active, cmdline);
}

/* set-environment [-u] <name> [value]: set (or, with -u, unset) a
 * session-level environment override for panes created from here on. */
static void cmd_set_environment(session_t *s, int argc, char **argv)
{
    int i, unset = 0;
    const char *name = NULL, *value = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) unset = 1;
        else if (name == NULL) name = argv[i];
        else value = argv[i];
    }
    if (name == NULL)
        return;
    if (unset) env_unset(s, name);
    else       env_set(s, name, value ? value : "");
}

static void cmd_show_environment(session_t *s, int argc, char **argv)
{
    int i, o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    for (i = 0; i < s->nenv && o < (int)sizeof(s->cmd_result) - 600; i++) {
        int n = _snprintf_s(s->cmd_result + o, sizeof(s->cmd_result) - o, _TRUNCATE,
                            "%s=%s\n", s->env[i].name, s->env[i].value);
        if (n < 0) break;
        o += n;
    }
}

static void cmd_unset_environment(session_t *s, int argc, char **argv)
{
    if (argc > 1)
        env_unset(s, argv[1]);
}

/* set-hook [-u] <event> <command>: run <command> whenever <event> fires (see
 * fire_hook for which events this build actually raises). */
static void cmd_set_hook(session_t *s, int argc, char **argv)
{
    int i, unset = 0;
    const char *event = NULL, *cmd = NULL;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) unset = 1;
        else if (event == NULL) event = argv[i];
        else cmd = argv[i];
    }
    if (event == NULL)
        return;
    if (unset) hook_unset(s, event);
    else if (cmd) hook_set(s, event, cmd);
}

static void cmd_show_hooks(session_t *s, int argc, char **argv)
{
    int i, o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    for (i = 0; i < s->nhooks && o < (int)sizeof(s->cmd_result) - 200; i++) {
        int n = _snprintf_s(s->cmd_result + o, sizeof(s->cmd_result) - o, _TRUNCATE,
                            "%s -> %s\n", s->hooks[i].event, s->hooks[i].cmd);
        if (n < 0) break;
        o += n;
    }
}

/* wait-for [-S|-L|-U] <channel>: -S (and, simplified, -L/-U -- see the
 * wait-for section above for why tmux's lock semantics aren't distinguished
 * here) signals the channel; a bare <channel> reports "OK" if it has been
 * signalled (consuming that signal) or "PENDING" otherwise, for the CLI to
 * poll on. */
static void cmd_wait_for(session_t *s, int argc, char **argv)
{
    int i, is_signal = 0;
    const char *name = NULL;
    wait_chan_t *c;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "-U") == 0)
            is_signal = 1;
        else
            name = argv[i];
    }
    s->cmd_result[0] = '\0';
    if (name == NULL)
        return;
    if (is_signal) {
        waitch_signal(s, name);
        strcpy_s(s->cmd_result, sizeof(s->cmd_result), "OK");
        return;
    }
    c = waitch_find(s, name);
    if (c && c->signaled) {
        c->signaled = 0;
        strcpy_s(s->cmd_result, sizeof(s->cmd_result), "OK");
    } else {
        strcpy_s(s->cmd_result, sizeof(s->cmd_result), "PENDING");
    }
}

/* Forward-declared so they can appear in CMD_TABLE; cmd_list_commands is
 * defined after it since it needs to enumerate the table, and
 * cmd_list_clients simply lives alongside it. */
static void cmd_list_commands(session_t *s, int argc, char **argv);
static void cmd_list_clients(session_t *s, int argc, char **argv);

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
    { "setw",            cmd_set },
    { "set-window-option", cmd_set },
    { "bind",            cmd_bind },
    { "bind-key",        cmd_bind },
    { "unbind",          cmd_unbind },
    { "unbind-key",      cmd_unbind },
    { "source-file",     cmd_source },
    { "command-prompt",  cmd_command_prompt },
    { "display-panes",   cmd_display_panes },
    { "choose-window",   cmd_choose_window },
    { "choose-tree",     cmd_choose_window },
    { "choose-buffer",   cmd_choose_buffer },
    { "display-message", cmd_display_message },
    { "run-shell",       cmd_run_shell },
    { "if-shell",        cmd_if_shell },
    { "clock-mode",      cmd_clock_mode },
    { "list-windows",    cmd_list_windows },
    { "lsw",             cmd_list_windows },
    { "list-panes",      cmd_list_panes },
    { "lsp",             cmd_list_panes },
    { "list-keys",       cmd_list_keys },
    { "lsk",             cmd_list_keys },
    { "show-options",    cmd_show_options },
    { "show-window-options", cmd_show_options },
    { "show",            cmd_show_options },
    { "list-buffers",    cmd_list_buffers },
    { "lsb",             cmd_list_buffers },
    { "show-buffer",     cmd_show_buffer },
    { "set-buffer",      cmd_set_buffer },
    { "delete-buffer",   cmd_delete_buffer },
    { "load-buffer",     cmd_load_buffer },
    { "save-buffer",     cmd_save_buffer },
    { "capture-pane",    cmd_capture_pane },
    { "capturep",        cmd_capture_pane },
    { "clear-history",   cmd_clear_history },
    { "clearhist",       cmd_clear_history },
    { "previous-layout", cmd_previous_layout },
    { "confirm-before",  cmd_confirm_before },
    { "confirm",         cmd_confirm_before },
    { "respawn-pane",    cmd_respawn_pane },
    { "respawn-window",  cmd_respawn_window },
    { "pipe-pane",       cmd_pipe_pane },
    { "list-commands",   cmd_list_commands },
    { "lscm",            cmd_list_commands },
    { "list-clients",    cmd_list_clients },
    { "lsc",             cmd_list_clients },
    { "set-environment", cmd_set_environment },
    { "show-environment", cmd_show_environment },
    { "unset-environment", cmd_unset_environment },
    { "set-hook",        cmd_set_hook },
    { "show-hooks",      cmd_show_hooks },
    { "wait-for",        cmd_wait_for },
};

/* list-clients / lsc: whether (and at what size) a client is attached. Since
 * a session's server only ever serves one interactive client at a time,
 * this is a single line rather than tmux's per-client table. */
static void cmd_list_clients(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    if (s->attached)
        _snprintf_s(s->cmd_result, sizeof(s->cmd_result), _TRUNCATE,
                   "client: attached [%dx%d]\n", s->cols, s->rows);
    else
        strcpy_s(s->cmd_result, sizeof(s->cmd_result), "");
}

/* list-commands / lscm: every command name this build recognizes. */
static void cmd_list_commands(session_t *s, int argc, char **argv)
{
    size_t i;
    int o = 0;
    (void)argc; (void)argv;
    s->cmd_result[0] = '\0';
    for (i = 0; i < sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0]) &&
                o < (int)sizeof(s->cmd_result) - 32; i++) {
        int n = _snprintf_s(s->cmd_result + o, sizeof(s->cmd_result) - o, _TRUNCATE,
                            "%s\n", CMD_TABLE[i].name);
        if (n < 0) break;
        o += n;
    }
}

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
    return session_create_in(shell, cols, rows, wake, NULL);
}

session_t *session_create_in(const wchar_t *shell, int cols, int rows, HANDLE wake,
                              const wchar_t *cwd)
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

    /* No set-environment overrides exist yet at session-creation time. */
    w = window_create(shell, cols, win_area_rows(s), wake, cwd, NULL);
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
    buffers_free_all(s);
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

    /* confirm-before: y/Y runs the pending command, anything else cancels. */
    if (s->confirm_active && n > 0) {
        unsigned char c = (unsigned char)bytes[0];
        s->confirm_active = 0;
        if (c == 'y' || c == 'Y')
            session_run_command(s, s->confirm_cmd);
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

    /* choose-buffer picker: same navigation as choose-window; Enter pastes
     * the highlighted buffer, q/Esc cancels. */
    if (s->choose_buf_active && n > 0) {
        unsigned char c = (unsigned char)bytes[0];
        int nb = s->nbuffers;
        if (c == 0x1b) {
            if (n >= 3 && bytes[1] == '[') {
                if (bytes[2] == 'A' && s->choose_buf_sel > 0) s->choose_buf_sel--;
                else if (bytes[2] == 'B' && s->choose_buf_sel < nb - 1) s->choose_buf_sel++;
            } else {
                s->choose_buf_active = 0;
            }
        } else if ((c == 'j' || c == 0x0e) && s->choose_buf_sel < nb - 1) {
            s->choose_buf_sel++;
        } else if ((c == 'k' || c == 0x10) && s->choose_buf_sel > 0) {
            s->choose_buf_sel--;
        } else if (c == '\r' || c == '\n') {
            char cmdline[96];
            s->choose_buf_active = 0;
            if (s->choose_buf_sel >= 0 && s->choose_buf_sel < s->nbuffers) {
                _snprintf_s(cmdline, sizeof(cmdline), _TRUNCATE,
                           "paste-buffer -b %s", s->buffers[s->choose_buf_sel].name);
                session_run_command(s, cmdline);
            }
        } else if (c == 'q') {
            s->choose_buf_active = 0;
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
        if (copymode_input(&s->copy, bytes, n, &text) && text.len) {
            clipboard_set_utf8(text.data, text.len);
            buffer_push_auto(s, text.data, text.len);
        }
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
        int died = 0;
        size_t parsed = window_pump_ex(s->windows[i], &died);
        if (died) {
            fire_hook(s, "pane-died");   /* may itself run arbitrary commands */
            if (i >= s->nwindows)
                break;                   /* the hook's command changed the window count */
        }
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

/* Draw the choose-buffer picker: one buffer per row, the selection highlighted. */
static void render_choose_buffer(session_t *s, strbuf_t *frame)
{
    int i;
    for (i = 0; i < s->nbuffers && i < s->rows - 1; i++) {
        char line[96];
        _snprintf_s(line, sizeof(line), _TRUNCATE, " %s: %-12zu bytes ",
                    s->buffers[i].name, s->buffers[i].len);
        strbuf_printf(frame, "\x1b[%d;1H%s%s\x1b[0m",
                      i + 1, (i == s->choose_buf_sel) ? "\x1b[7m" : "\x1b[1m", line);
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
    if (s->choose_buf_active)
        render_choose_buffer(s, frame); /* buffer picker overlay */
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

void session_set_attached(session_t *s, int on)
{
    int was = s->attached;
    s->attached = on ? 1 : 0;
    if (s->attached && !was)
        fire_hook(s, "client-attached");
    else if (!s->attached && was)
        fire_hook(s, "client-detached");
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

void session_run_capture(session_t *s, const char *cmdline, strbuf_t *out)
{
    s->cmd_result[0] = '\0';
    session_run_command(s, cmdline);
    if (out != NULL && s->cmd_result[0] != '\0')
        strbuf_append(out, s->cmd_result, strlen(s->cmd_result));
}

void session_load_config(session_t *s)
{
    session_load_config_from(s, NULL);
}

void session_load_config_from(session_t *s, const char *override_path)
{
    char path[MAX_PATH];
    DWORD n;
    if (override_path != NULL && override_path[0] != '\0') {
        session_load_config_path(s, override_path);
        return;
    }
    n = GetEnvironmentVariableA("USERPROFILE", path, sizeof(path));
    if (n > 0 && n < sizeof(path)) {
        strcat_s(path, sizeof(path), "\\.tmuxw.conf");
        session_load_config_path(s, path);
    }
}

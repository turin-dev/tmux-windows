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

typedef struct keybind {
    int  key;
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

static const char *bind_find(const session_t *s, int key)
{
    int i;
    for (i = 0; i < s->nbind; i++)
        if (s->bind[i].key == key)
            return s->bind[i].cmd;
    return NULL;
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
    if (idx >= 0) select_window(s, idx);
}

static void cmd_kill_window(session_t *s, int argc, char **argv)
{
    (void)argc; (void)argv;
    kill_window(s);
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

static void cmd_rename_window(session_t *s, int argc, char **argv)
{
    window_t *w = cur_window(s);
    if (w && argc > 1) { strncpy_s(w->name, sizeof(w->name), argv[1], _TRUNCATE); mark(s, 1); }
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
    }
}

static void cmd_bind(session_t *s, int argc, char **argv)
{
    int k, i;
    char buf[BIND_CMD_MAX];
    if (argc < 3) return;
    k = cmd_parse_key(argv[1]);
    if (k < 0) return;
    buf[0] = '\0';
    for (i = 2; i < argc; i++) {
        if (i > 2) strcat_s(buf, sizeof(buf), " ");
        strcat_s(buf, sizeof(buf), argv[i]);
    }
    bind_set(s, k, buf);
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
    { "last-pane",       cmd_last_pane },
    { "last-window",     cmd_last_window },
    { "kill-pane",       cmd_kill_pane },
    { "next-window",     cmd_next_window },
    { "previous-window", cmd_prev_window },
    { "select-window",   cmd_select_window },
    { "kill-window",     cmd_kill_window },
    { "detach-client",   cmd_detach },
    { "copy-mode",       cmd_copymode },
    { "send-prefix",     cmd_send_prefix },
    { "rename-window",   cmd_rename_window },
    { "set",             cmd_set },
    { "set-option",      cmd_set },
    { "bind",            cmd_bind },
    { "bind-key",        cmd_bind },
    { "unbind",          cmd_unbind },
    { "unbind-key",      cmd_unbind },
    { "source-file",     cmd_source },
    { "command-prompt",  cmd_command_prompt },
};

static void session_run_command(session_t *s, const char *line)
{
    char storage[512];
    char *argv[CMD_MAX_ARGS];
    int argc, i;

    if (line == NULL || line[0] == '\0')
        return;
    argc = cmd_tokenize(line, storage, sizeof(storage), argv, CMD_MAX_ARGS);
    if (argc == 0)
        return;
    for (i = 0; i < (int)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0])); i++) {
        if (strcmp(argv[0], CMD_TABLE[i].name) == 0) {
            CMD_TABLE[i].fn(s, argc, argv);
            return;
        }
    }
}

/* Run the command bound to `keyid`, if any. */
static void run_key(session_t *s, int keyid)
{
    const char *cmd = bind_find(s, keyid);
    if (cmd)
        session_run_command(s, cmd);
}

static void handle_prefix(session_t *s, unsigned char c)
{
    s->pstate = PS_NORMAL;
    if (c == 0x1b) {          /* an arrow-key sequence follows */
        s->pstate = PS_ESC;
        return;
    }
    run_key(s, (int)c);
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
    bind_set(s, 'z',  "resize-pane -Z");
    bind_set(s, ' ',  "next-layout");
    bind_set(s, '{',  "swap-pane -U");
    bind_set(s, '}',  "swap-pane -D");
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
}

static void render_status(session_t *s, strbuf_t *frame)
{
    status_win_t wins[MAX_WINDOWS];
    char clock[16];
    SYSTEMTIME st;
    int i;

    for (i = 0; i < s->nwindows; i++) {
        wins[i].index = i;
        wins[i].name = s->windows[i]->name;
        wins[i].current = (i == s->cur);
    }
    GetLocalTime(&st);
    _snprintf_s(clock, sizeof(clock), _TRUNCATE, "%02d:%02d", st.wHour, st.wMinute);

    status_render(frame, s->cols, s->rows, s->name, wins, s->nwindows, clock);
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

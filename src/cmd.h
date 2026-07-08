/* cmd.h — command-line tokenizing and key-name parsing.
 *
 * These are the pure pieces of the command system (no session state), so they
 * can be unit-tested directly. The session owns the command table, key
 * bindings, and dispatch (see session.c).
 */
#ifndef TMUXW_CMD_H
#define TMUXW_CMD_H

#include <stddef.h>

#define CMD_MAX_ARGS 16

/* Split `line` into argv, writing NUL-terminated tokens into `storage` (which
 * must hold at least strlen(line)+1 bytes). Handles runs of whitespace and
 * simple double-quoted tokens. Returns argc (<= max_args). */
int cmd_tokenize(const char *line, char *storage, size_t storage_len,
                 char **argv, int max_args);

/* Key ids: 0..255 are literal bytes; these name the non-byte keys. */
enum {
    KEY_UP = 0x1001,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PPAGE,
    KEY_NPAGE
};

/* Parse a key name ("C-b", "^B", "%", "Up", "Enter", "Space") to a key id, or
 * -1 if unrecognized. */
int cmd_parse_key(const char *name);

/* Write a display name for `keyid` into buf. */
void cmd_key_name(int keyid, char *buf, size_t cap);

#endif /* TMUXW_CMD_H */

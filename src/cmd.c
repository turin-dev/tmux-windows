/* cmd.c — see cmd.h. */
#include "cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int cmd_tokenize(const char *line, char *storage, size_t storage_len,
                 char **argv, int max_args)
{
    size_t n = strlen(line);
    int argc = 0;
    char *w;
    const char *p = line;

    if (n + 1 > storage_len)
        n = storage_len - 1;

    w = storage;
    while (*p && argc < max_args) {
        /* Skip leading whitespace. */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        argv[argc++] = w;
        if (*p == '"') {
            p++;
            while (*p && *p != '"')
                *w++ = *p++;
            if (*p == '"')
                p++;
        } else {
            while (*p && *p != ' ' && *p != '\t')
                *w++ = *p++;
        }
        *w++ = '\0';
    }
    return argc;
}

int cmd_parse_key(const char *name)
{
    size_t len;
    if (name == NULL || name[0] == '\0')
        return -1;
    len = strlen(name);

    if (len == 1)
        return (unsigned char)name[0];

    /* Control keys: "C-b" or "^B". */
    if (len == 3 && (name[0] == 'C' || name[0] == 'c') && name[1] == '-')
        return (unsigned char)(name[2]) & 0x1f;
    if (len == 2 && name[0] == '^')
        return (unsigned char)(name[1]) & 0x1f;

    if (_stricmp(name, "C-Up") == 0)      return KEY_C_UP;
    if (_stricmp(name, "C-Down") == 0)    return KEY_C_DOWN;
    if (_stricmp(name, "C-Left") == 0)    return KEY_C_LEFT;
    if (_stricmp(name, "C-Right") == 0)   return KEY_C_RIGHT;
    if (_stricmp(name, "M-Up") == 0)      return KEY_M_UP;
    if (_stricmp(name, "M-Down") == 0)    return KEY_M_DOWN;
    if (_stricmp(name, "M-Left") == 0)    return KEY_M_LEFT;
    if (_stricmp(name, "M-Right") == 0)   return KEY_M_RIGHT;

    if (_stricmp(name, "Up") == 0)        return KEY_UP;
    if (_stricmp(name, "Down") == 0)      return KEY_DOWN;
    if (_stricmp(name, "Left") == 0)      return KEY_LEFT;
    if (_stricmp(name, "Right") == 0)     return KEY_RIGHT;
    if (_stricmp(name, "PageUp") == 0 || _stricmp(name, "PPage") == 0)   return KEY_PPAGE;
    if (_stricmp(name, "PageDown") == 0 || _stricmp(name, "NPage") == 0) return KEY_NPAGE;
    if (_stricmp(name, "Enter") == 0)     return '\r';
    if (_stricmp(name, "Space") == 0)     return ' ';
    if (_stricmp(name, "Tab") == 0)       return '\t';
    if (_stricmp(name, "Escape") == 0 || _stricmp(name, "Esc") == 0) return 0x1b;

    return -1;
}

void cmd_key_name(int keyid, char *buf, size_t cap)
{
    switch (keyid) {
        case KEY_UP:    strncpy_s(buf, cap, "Up", _TRUNCATE); return;
        case KEY_DOWN:  strncpy_s(buf, cap, "Down", _TRUNCATE); return;
        case KEY_LEFT:  strncpy_s(buf, cap, "Left", _TRUNCATE); return;
        case KEY_RIGHT: strncpy_s(buf, cap, "Right", _TRUNCATE); return;
        case KEY_PPAGE: strncpy_s(buf, cap, "PageUp", _TRUNCATE); return;
        case KEY_NPAGE: strncpy_s(buf, cap, "PageDown", _TRUNCATE); return;
        case KEY_C_UP:    strncpy_s(buf, cap, "C-Up", _TRUNCATE); return;
        case KEY_C_DOWN:  strncpy_s(buf, cap, "C-Down", _TRUNCATE); return;
        case KEY_C_LEFT:  strncpy_s(buf, cap, "C-Left", _TRUNCATE); return;
        case KEY_C_RIGHT: strncpy_s(buf, cap, "C-Right", _TRUNCATE); return;
        case KEY_M_UP:    strncpy_s(buf, cap, "M-Up", _TRUNCATE); return;
        case KEY_M_DOWN:  strncpy_s(buf, cap, "M-Down", _TRUNCATE); return;
        case KEY_M_LEFT:  strncpy_s(buf, cap, "M-Left", _TRUNCATE); return;
        case KEY_M_RIGHT: strncpy_s(buf, cap, "M-Right", _TRUNCATE); return;
        default: break;
    }
    if (keyid >= ' ' && keyid < 0x7f) {
        buf[0] = (char)keyid;
        buf[1] = '\0';
    } else if (keyid > 0 && keyid < ' ') {
        /* Control key. */
        _snprintf_s(buf, cap, _TRUNCATE, "C-%c", (char)(keyid + 'a' - 1));
    } else {
        _snprintf_s(buf, cap, _TRUNCATE, "0x%02x", keyid);
    }
}

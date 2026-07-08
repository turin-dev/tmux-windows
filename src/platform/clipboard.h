/* clipboard.h — put text on the Windows clipboard. */
#ifndef TMUXW_PLATFORM_CLIPBOARD_H
#define TMUXW_PLATFORM_CLIPBOARD_H

#include <stddef.h>

/* Copy a UTF-8 string (of `len` bytes) to the clipboard as CF_UNICODETEXT.
 * Returns 0 on success, non-zero on failure. */
int clipboard_set_utf8(const char *utf8, size_t len);

/* Read the clipboard's text as a newly-allocated UTF-8 string (caller frees with
 * free()). Returns NULL if empty/unavailable; sets *len to the byte length. */
char *clipboard_get_utf8(size_t *len);

#endif /* TMUXW_PLATFORM_CLIPBOARD_H */

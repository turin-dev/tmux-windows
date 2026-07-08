/* clipboard.h — put text on the Windows clipboard. */
#ifndef TMUXW_PLATFORM_CLIPBOARD_H
#define TMUXW_PLATFORM_CLIPBOARD_H

#include <stddef.h>

/* Copy a UTF-8 string (of `len` bytes) to the clipboard as CF_UNICODETEXT.
 * Returns 0 on success, non-zero on failure. */
int clipboard_set_utf8(const char *utf8, size_t len);

#endif /* TMUXW_PLATFORM_CLIPBOARD_H */

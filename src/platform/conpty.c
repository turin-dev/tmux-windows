/* conpty.c — see conpty.h. */
#include "platform/conpty.h"

#include <stdlib.h>
#include <string.h>

/* Build the STARTUPINFOEX carrying the pseudo console attribute. On success the
 * caller owns si->lpAttributeList and must free it via
 * DeleteProcThreadAttributeList + HeapFree. */
static int build_startup_info(STARTUPINFOEXW *si, HPCON hpc)
{
    SIZE_T bytes = 0;
    memset(si, 0, sizeof(*si));
    si->StartupInfo.cb = sizeof(STARTUPINFOEXW);

    /* First call sizes the attribute list. Expected to "fail" with a size. */
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    si->lpAttributeList =
        (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (si->lpAttributeList == NULL)
        return (int)ERROR_OUTOFMEMORY;

    if (!InitializeProcThreadAttributeList(si->lpAttributeList, 1, 0, &bytes)) {
        int err = (int)GetLastError();
        HeapFree(GetProcessHeap(), 0, si->lpAttributeList);
        si->lpAttributeList = NULL;
        return err;
    }

    if (!UpdateProcThreadAttribute(si->lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hpc, sizeof(hpc), NULL, NULL)) {
        int err = (int)GetLastError();
        DeleteProcThreadAttributeList(si->lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si->lpAttributeList);
        si->lpAttributeList = NULL;
        return err;
    }
    return 0;
}

int conpty_spawn(conpty_t *pty, const wchar_t *cmdline, short cols, short rows,
                  const wchar_t *cwd)
{
    HANDLE in_read = INVALID_HANDLE_VALUE, in_write = INVALID_HANDLE_VALUE;
    HANDLE out_read = INVALID_HANDLE_VALUE, out_write = INVALID_HANDLE_VALUE;
    wchar_t *cmd_copy = NULL;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    COORD size;
    HRESULT hr;
    int err;

    memset(pty, 0, sizeof(*pty));
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));

    if (cols <= 0) cols = 80;
    if (rows <= 0) rows = 25;

    /* Two anonymous pipes. The child ends (in_read, out_write) are handed to
     * the pseudo console; the parent ends (in_write, out_read) we keep. */
    if (!CreatePipe(&in_read, &in_write, NULL, 0)) {
        err = (int)GetLastError();
        goto fail;
    }
    if (!CreatePipe(&out_read, &out_write, NULL, 0)) {
        err = (int)GetLastError();
        goto fail;
    }

    size.X = cols;
    size.Y = rows;
    hr = CreatePseudoConsole(size, in_read, out_write, 0, &pty->hpc);
    if (FAILED(hr)) {
        err = (int)hr;
        goto fail;
    }

    /* ConPTY duplicated the child ends into the conhost; we can close ours. */
    CloseHandle(in_read);  in_read = INVALID_HANDLE_VALUE;
    CloseHandle(out_write); out_write = INVALID_HANDLE_VALUE;

    err = build_startup_info(&si, pty->hpc);
    if (err != 0)
        goto fail;

    /* CreateProcessW may write to the command-line buffer, so pass a copy. */
    {
        size_t n = wcslen(cmdline) + 1;
        cmd_copy = (wchar_t *)malloc(n * sizeof(wchar_t));
        if (cmd_copy == NULL) {
            err = (int)ERROR_OUTOFMEMORY;
            goto fail;
        }
        memcpy(cmd_copy, cmdline, n * sizeof(wchar_t));
    }

    /* A console child with no explicit std handles picks up the parent's
     * current STD_* handles. If ours are redirected (a pipe/file, e.g. a
     * detached server or a test harness), the child's stdout would land there
     * instead of the pty. Blank them across the spawn so the child instead
     * inherits the pseudo console's CONOUT$/CONIN$, which routes through
     * conhost and gives us one clean VT stream. bInheritHandles is FALSE per
     * the ConPTY docs. */
    {
        HANDLE save_in  = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE save_out = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE save_err = GetStdHandle(STD_ERROR_HANDLE);
        BOOL ok;

        SetStdHandle(STD_INPUT_HANDLE, NULL);
        SetStdHandle(STD_OUTPUT_HANDLE, NULL);
        SetStdHandle(STD_ERROR_HANDLE, NULL);

        ok = CreateProcessW(NULL, cmd_copy, NULL, NULL, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT,
                            NULL, (cwd && cwd[0]) ? cwd : NULL,
                            &si.StartupInfo, &pi);

        SetStdHandle(STD_INPUT_HANDLE, save_in);
        SetStdHandle(STD_OUTPUT_HANDLE, save_out);
        SetStdHandle(STD_ERROR_HANDLE, save_err);

        if (!ok) {
            err = (int)GetLastError();
            goto fail;
        }
    }

    pty->input_write = in_write;
    pty->output_read = out_read;
    pty->process = pi.hProcess;
    pty->thread = pi.hThread;

    free(cmd_copy);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    return 0;

fail:
    if (cmd_copy) free(cmd_copy);
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }
    if (pty->hpc) { ClosePseudoConsole(pty->hpc); pty->hpc = NULL; }
    if (in_read  != INVALID_HANDLE_VALUE) CloseHandle(in_read);
    if (in_write != INVALID_HANDLE_VALUE) CloseHandle(in_write);
    if (out_read != INVALID_HANDLE_VALUE) CloseHandle(out_read);
    if (out_write != INVALID_HANDLE_VALUE) CloseHandle(out_write);
    memset(pty, 0, sizeof(*pty));
    return err;
}

int conpty_resize(conpty_t *pty, short cols, short rows)
{
    COORD size;
    HRESULT hr;
    if (pty->hpc == NULL)
        return (int)ERROR_INVALID_HANDLE;
    if (cols <= 0) cols = 1;
    if (rows <= 0) rows = 1;
    size.X = cols;
    size.Y = rows;
    hr = ResizePseudoConsole(pty->hpc, size);
    return FAILED(hr) ? (int)hr : 0;
}

void conpty_close(conpty_t *pty)
{
    if (pty == NULL)
        return;
    /* Close the pseudo console first so the child sees EOF/HUP on its console. */
    if (pty->hpc)          { ClosePseudoConsole(pty->hpc); pty->hpc = NULL; }
    if (pty->input_write)  { CloseHandle(pty->input_write); pty->input_write = NULL; }
    if (pty->output_read)  { CloseHandle(pty->output_read); pty->output_read = NULL; }
    if (pty->thread)       { CloseHandle(pty->thread); pty->thread = NULL; }
    if (pty->process)      { CloseHandle(pty->process); pty->process = NULL; }
}

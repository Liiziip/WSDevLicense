/* file.c - whole-file reader (0x25d8 in arm.exe). */
#include "wss.h"
#include "app.h"

HRESULT wss_read_file(const wchar_t *path, u32 *pcb, void **ppv)
{
    HANDLE h;
    DWORD size, high, got;
    void *buf;
    HRESULT hr = S_OK;

    if (path == NULL || pcb == NULL || ppv == NULL)
        return E_INVALIDARG;
    *pcb = 0;
    *ppv = NULL;

    h = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }

    size = GetFileSize(h, &high);
    if (size == INVALID_FILE_SIZE) {
        DWORD err = GetLastError();
        hr = err ? HRESULT_FROM_WIN32(err) : E_FAIL;
        goto done;
    }
    /* The original rejects anything that does not fit in a single DWORD. */
    if (high != 0 || size == 0) {
        hr = E_INVALIDARG;
        goto done;
    }

    buf = HeapAlloc(GetProcessHeap(), 0, size);
    if (buf == NULL) {
        hr = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
        goto done;
    }

    got = 0;
    if (!ReadFile(h, buf, size, &got, NULL)) {
        DWORD err = GetLastError();
        hr = err ? HRESULT_FROM_WIN32(err) : E_FAIL;
        HeapFree(GetProcessHeap(), 0, buf);
        goto done;
    }
    if (got != size) {
        HeapFree(GetProcessHeap(), 0, buf);
        hr = E_FAIL;
        goto done;
    }

    *pcb = size;
    *ppv = buf;

done:
    CloseHandle(h);
    return hr;
}

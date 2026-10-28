/* print.c - console output helpers (0x21c0 and 0x2238 in arm.exe). */
#include "wss.h"
#include "app.h"

#include <stdarg.h>
#include <stdio.h>

/*
 * 0x21c0: set the console attribute, vwprintf, restore the attribute.
 * When stdout is not a console the original falls back to 0x0e (yellow).
 */
void wss_printf(WORD color, const wchar_t *fmt, ...)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    WORD old = 0x0e;
    va_list ap;

    if (GetConsoleScreenBufferInfo(h, &csbi))
        old = csbi.wAttributes;
    SetConsoleTextAttribute(h, color);

    va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);

    SetConsoleTextAttribute(h, old);
}

/*
 * 0x2238: report a failed HRESULT.  A Win32-facility HRESULT (0x8007xxxx) is
 * formatted with its low word, everything else with the full value.
 */
void wss_print_error(HRESULT hr)
{
    wchar_t *msg = NULL;
    DWORD code = ((u32)hr & 0xffff0000u) == 0x80070000u ? ((u32)hr & 0xffffu) : (u32)hr;

    if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, code, 0x400, (LPWSTR)&msg, 0, NULL) == 0) {
        wss_printf(0x0c, L"The command failed. Error code is 0x%x\r\n", (u32)hr);
    } else {
        wss_printf(0x0c, L"The command failed. %s Error code is 0x%x\r\n", msg, (u32)hr);
    }
    if (code == ERROR_ACCESS_DENIED)
        wss_printf(0x0e, L"Please run the command from an elevated command window\r\n");
    if (msg != NULL)
        LocalFree(msg);
}

/*
 * Diagnostics: set ARM_X86_TRACE=1 in the environment and every step prints its
 * HRESULT to stderr.  Off by default; the normal output is unchanged.
 */
int g_wssTrace = 0;

void wss_trace(const wchar_t *fmt, ...)
{
    va_list ap;

    if (!g_wssTrace)
        return;
    fwprintf(stderr, L"[trace] ");
    va_start(ap, fmt);
    vfwprintf(stderr, fmt, ap);
    va_end(ap);
    fwprintf(stderr, L"\n");
    fflush(stderr);
}

void wss_trace_hex(const wchar_t *tag, const void *pv, u32 cb)
{
    const unsigned char *p = (const unsigned char *)pv;
    u32 i;

    if (!g_wssTrace || p == NULL)
        return;
    fwprintf(stderr, L"[trace] %s (%u bytes):", tag, cb);
    for (i = 0; i < cb && i < 96; i++)
        fwprintf(stderr, L" %02x", p[i]);
    fwprintf(stderr, L"%s\n", cb > 96 ? L" ..." : L"");
    fflush(stderr);
}

/*
 * 0x2538: the original only traps when the failure matches the value stored in
 * its global (0 means "no trap"), so a plain run never aborts.
 */
HRESULT g_wssThrowHr = 0;

void wss_check(HRESULT hr)
{
    if (g_wssThrowHr != 0 && hr < 0 && (hr == g_wssThrowHr || g_wssThrowHr == -1)) {
        DebugBreak();
    }
}

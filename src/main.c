/*
 * main.c - command line front end (0x22d0 in arm.exe).
 *
 *   arm.exe -install <license.xml>   install the developer licence
 *   arm.exe -remove                  remove the developer licence
 *   arm.exe -check                   report the licence expiry (UTC, ISO 8601)
 *
 * Both "-x" and "/x" spellings are accepted, exactly like the original.
 */
#include "wss.h"
#include "app.h"
#include "midl_blob.h"

#include <stdio.h>
#include <wchar.h>

static const wchar_t kUsage[] =
    L"Utility for Windows Store Developer Licensing\r\n"
    L"\r\n"
    L"\t-install   filename          :: Install Developer License\r\n"
    L"\t-remove                      :: remove the developer license\r\n"
    L"\r\n"
    L"\t-check                       :: Check if a developer license is installed\r\n"
    L"\r\n";

int wmain(int argc, wchar_t **argv)
{
    int session = 0;
    u32 cbFile = 0;
    void *pFile = NULL;
    HRESULT hr = S_OK;
    int rc = 0;

    {
        wchar_t flag[8];
        if (GetEnvironmentVariableW(L"ARM_X86_TRACE", flag, 8) != 0)
            g_wssTrace = 1;
    }

    if (argc > 1 && _wcsicmp(argv[1], L"-dumpfmt") == 0) {
        static const int offs[3] = { 0x02, 0x30, 0x5e };
        int i, j;
        for (i = 0; i < 3; i++) {
            const unsigned char *p = g_procFormat + offs[i];
            u32 procnum = (u32)p[2] | ((u32)p[3] << 8) | ((u32)p[4] << 16) | ((u32)p[5] << 24);
            u32 stacksz = (u32)p[8] | ((u32)p[9] << 8);
            u32 typeoff = (u32)p[16] | ((u32)p[17] << 8);
            wprintf(L"proc #%d @ +0x%02x: handle=0x%02x flags=0x%02x procnum=%u "
                    L"stack=0x%x bind=0x%02x typeoff=0x%x oi2=0x%02x nparams=%u ext=0x%02x\n",
                    i, offs[i], p[0], p[1], procnum, stacksz, p[10], typeoff, p[18], p[19], p[21]);
            for (j = 0; j < 12; j++)
                wprintf(L"    %02x: %02x %02x %02x %02x\n", j,
                        p[j * 4], p[j * 4 + 1], p[j * 4 + 2], p[j * 4 + 3]);
        }
        return 0;
    }

    if (argc > 1) {
        if (_wcsicmp(argv[1], L"/install") == 0 || _wcsicmp(argv[1], L"-install") == 0) {
            if (argc != 3)
                goto usage;
            hr = wss_read_file(argv[2], &cbFile, &pFile);
            if (hr >= 0) {
                hr = wss_session_ensure(&session);
                if (hr >= 0) {
                    wss_trace(L"main: session=0x%x before install", (u32)session);
                    hr = wss_install_license(session, cbFile, pFile);
                    if (hr >= 0)
                        wss_printf(0x0e, L"The developer license was successfully installed\r\n");
                }
            }
            wss_check(hr);
            goto report;
        }

        if (_wcsicmp(argv[1], L"/remove") == 0 || _wcsicmp(argv[1], L"-remove") == 0) {
            if (argc != 2)
                goto usage;
            hr = wss_session_ensure(&session);
            if (hr >= 0) {
                hr = wss_remove_license(session);
                if (hr >= 0)
                    wss_printf(0x0e, L"The developer license was successfully removed\r\n");
            }
            wss_check(hr);
            goto report;
        }

        if (_wcsicmp(argv[1], L"/check") == 0 || _wcsicmp(argv[1], L"-check") == 0) {
            FILETIME ft;
            SYSTEMTIME st;
            wchar_t stamp[102];

            if (argc != 2)
                goto usage;
            memset(&ft, 0, sizeof(ft));
            hr = wss_check_license(&ft);
            if (hr >= 0) {
                stamp[0] = L'\0';
                if (FileTimeToSystemTime(&ft, &st)) {
                    swprintf_s(stamp, 102, L"%04u-%02u-%02u%s%02u:%02u:%02u%s",
                               st.wYear, st.wMonth, st.wDay, L"T",
                               st.wHour, st.wMinute, st.wSecond, L"Z");
                } else {
                    /* The original ignores the failure and prints an empty stamp. */
                    GetLastError();
                }
                wprintf(L"Developer License is installed. Expiration is %s\n", stamp);
                wss_check(hr);
                goto report;
            }
            wprintf(L"CheckDeveloperLicense Failed, error is 0X%x\n", (u32)hr);
            wss_check(hr);
            goto report;
        }
    }

usage:
    wss_check(E_INVALIDARG);
    wss_printf(0x0a, kUsage);
    goto cleanup;

report:
    if (hr < 0) {
        wss_print_error(hr);
        rc = 1;
    }

cleanup:
    if (pFile != NULL)
        HeapFree(GetProcessHeap(), 0, pFile);
    if (session != 0)
        wss_rpc_session_close(&session);
    return rc;
}

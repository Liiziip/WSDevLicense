/*
 * svc.c - start and wait for the WSService service (ARM 0x2d14 / 0x2e70 / 0x2f3c).
 *
 * 0x2d14 opens the SCM, opens "WSService" with QUERY_STATUS|START, waits for a
 * RUNNING notification and otherwise starts the service and waits again.  The
 * x86-64 build of the same tool does the same thing with a plain status poll
 * (it does not import NotifyServiceStatusChangeW at all), and that is what is
 * used here: the notification API is fussy about the SERVICE_NOTIFYW version
 * and is not needed to make the tool work.
 */
#include "wss.h"
#include "app.h"

#define WSS_SERVICE_NAME L"WSService"

static HRESULT status_process(SC_HANDLE svc, SERVICE_STATUS_PROCESS *ssp)
{
    DWORD cb = 0;

    memset(ssp, 0, sizeof(*ssp));
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)ssp, sizeof(*ssp), &cb)) {
        DWORD err = GetLastError();
        wss_trace(L"QueryServiceStatusEx failed err=%u", (unsigned)err);
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }
    return S_OK;
}

/* 0x2f3c: poll until RUNNING, 30 attempts one second apart. */
static HRESULT wait_running_poll(SC_HANDLE svc)
{
    SERVICE_STATUS_PROCESS ssp;
    HRESULT hr;
    int i;

    for (i = 0; i <= 0x1d; i++) {
        hr = status_process(svc, &ssp);
        if (hr < 0)
            return hr;
        if ((i % 5) == 0)
            wss_trace(L"WSService state=%u (attempt %d)", (unsigned)ssp.dwCurrentState, i);
        if (ssp.dwCurrentState == SERVICE_RUNNING)
            return S_OK;
        if (ssp.dwCurrentState == SERVICE_STOPPED)
            return HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
        Sleep(1000);
    }
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

HRESULT wss_service_start(void)
{
    SC_HANDLE scm, svc;
    SERVICE_STATUS_PROCESS ssp;
    HRESULT hr;
    DWORD err;

    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        err = GetLastError();
        wss_trace(L"OpenSCManagerW failed err=%u", (unsigned)err);
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }
    svc = OpenServiceW(scm, WSS_SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_START);
    if (svc == NULL) {
        err = GetLastError();
        wss_trace(L"OpenServiceW(WSService) failed err=%u", (unsigned)err);
        CloseServiceHandle(scm);
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }
    CloseServiceHandle(scm);
    wss_trace(L"opened WSService");

    hr = status_process(svc, &ssp);
    if (hr < 0)
        goto done;
    wss_trace(L"WSService state=%u", (unsigned)ssp.dwCurrentState);

    if (ssp.dwCurrentState != SERVICE_RUNNING) {
        if (StartServiceW(svc, 0, NULL)) {
            wss_trace(L"StartServiceW ok");
        } else {
            err = GetLastError();
            wss_trace(L"StartServiceW err=%u", (unsigned)err);
            if (err != ERROR_SERVICE_ALREADY_RUNNING) {
                hr = err ? HRESULT_FROM_WIN32(err) : E_FAIL;
                goto done;
            }
        }
        hr = wait_running_poll(svc);
        if (hr < 0)
            goto done;
    }

    hr = S_OK;
done:
    if (svc != NULL)
        CloseServiceHandle(svc);
    if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
        hr = HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE);
    wss_trace(L"service start result -> 0x%08x", (u32)hr);
    return hr;
}

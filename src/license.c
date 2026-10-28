/*
 * license.c - the three operations the tool offers (0x2ffc / 0x2c2c / 0x2ac4).
 */
#include "wss.h"
#include "app.h"

/*
 * 0x2ffc: add one blob property to a bag, creating the bag on first use.
 * The value layout is the 20-byte WssValue {name, vt=4, pv, cb}.
 */
static HRESULT bag_add_blob(WssBag **ppbag, int tag, const void *pv, u32 cb)
{
    WssValue v;
    HRESULT hr;

    if (pv == NULL)
        return E_INVALIDARG;
    if (*ppbag == NULL) {
        hr = wss_bag_create(ppbag);
        if (hr < 0)
            return hr;
    }
    memset(&v, 0, sizeof(v));
    v.name = NULL;
    v.vt = WSS_VT_BLOB;
    v.u.pv = pv;
    v.cb = cb;
    return wss_bag_set_value(*ppbag, (u32)(tag - 1), &v);
}

/*
 * 0x2c2c: install request.  Three properties are sent:
 *   1 = request size (4 bytes), 2 = the licence XML file, 3 = constant 6.
 * Opcode 0 is used.
 */
HRESULT wss_install_license(int session, u32 cb, const void *data)
{
    WssBag *bag = NULL;
    WssBag *result = NULL;
    void *wire = NULL;
    u32 wirelen = 0;
    u32 size = cb;
    u32 magic = 6;
    HRESULT hr;

    if (cb == 0)
        return E_INVALIDARG;

    hr = bag_add_blob(&bag, 1, &size, sizeof(size));
    wss_trace(L"bag set[0] size=%u -> 0x%08x", (unsigned)size, (u32)hr);
    if (hr >= 0)
        hr = bag_add_blob(&bag, 2, data, cb);
    wss_trace(L"bag set[1] data cb=%u -> 0x%08x", (unsigned)cb, (u32)hr);
    if (hr >= 0)
        hr = bag_add_blob(&bag, 3, &magic, sizeof(magic));
    wss_trace(L"bag set[2] magic=%u -> 0x%08x", (unsigned)magic, (u32)hr);
    if (hr >= 0)
        hr = wss_bag_serialize(bag, &wire, &wirelen);
    wss_trace(L"wss_bag_serialize -> 0x%08x len=%u", (u32)hr, (unsigned)wirelen);
    if (hr >= 0)
        hr = wss_rpc_invoke(session, 0, wire, wirelen, &result);
    wss_trace(L"wss_rpc_invoke -> 0x%08x", (u32)hr);

    if (wire != NULL)
        LocalFree(wire);
    if (bag != NULL)
        wss_bag_release(bag);
    if (result != NULL)
        wss_bag_release(result);
    return hr;
}

/*
 * 0x2be0: open a session; if that fails, bring WSService up and retry once.
 */
HRESULT wss_session_ensure(int *pSession)
{
    HRESULT hr, hr2;

    hr = wss_rpc_session_open(pSession);
    wss_trace(L"session open -> 0x%08x (session=0x%x)", (u32)hr, pSession ? (u32)*pSession : 0);
    if (hr >= 0)
        return hr;

    hr2 = wss_service_start();
    wss_trace(L"service start -> 0x%08x", (u32)hr2);
    wss_check(hr2);
    if (hr2 < 0)
        return hr2;

    hr = wss_rpc_session_open(pSession);
    wss_trace(L"session open (retry) -> 0x%08x session=0x%x", (u32)hr,
              pSession ? (u32)*pSession : 0);
    wss_check(hr);
    return hr;
}

/* 0x2ac4 with opcode 0xc and an empty request: remove the developer licence. */
HRESULT wss_remove_license(int session)
{
    WssBag *result = NULL;
    HRESULT hr;

    hr = wss_rpc_invoke(session, 0x0c, NULL, 0, &result);
    if (result != NULL)
        wss_bag_release(result);
    return hr;
}

/*
 * WSClient!CheckDeveloperLicense.  The original links WSClient.dll directly;
 * here it is resolved at run time so the tool also loads on systems where the
 * export is missing (the error is then the same one the RPC layer would give).
 */
HRESULT wss_check_license(FILETIME *pExpiration)
{
    typedef HRESULT(WINAPI *PFN_CHECK)(FILETIME *);
    HMODULE mod;
    PFN_CHECK pfn;

    mod = LoadLibraryW(L"WSClient.dll");
    if (mod == NULL) {
        DWORD err = GetLastError();
        return err ? HRESULT_FROM_WIN32(err) : E_FAIL;
    }
    pfn = (PFN_CHECK)(void *)GetProcAddress(mod, "CheckDeveloperLicense");
    if (pfn == NULL)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return pfn(pExpiration);
}

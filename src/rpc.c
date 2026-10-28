/*
 * rpc.c - RPC client side of the WSS (Windows Store Service) developer licence
 *         interface, reconstructed from arm.exe.
 *
 * The original binary calls NdrClientCall2 with a MIDL stub descriptor that
 * lives at VA 0x401A80 and a format string table at VA 0x401890.  Instead of
 * hand-marshalling through copies of those blobs, this file links the stub that
 * midl generates from src/wss_rpc.idl - an interface reconstructed from the
 * format entry at 0x4018EE and verified against a midl reference build
 * (docs/ndr-proc2.md).  The wire format is therefore identical to the original,
 * including the two things a hand-written call cannot express:
 *
 *   - the session is a *context handle* (20 bytes on the wire: 16-byte GUID +
 *     4 bytes of attributes), not an int;
 *   - the request buffer is a conformant array of char whose conformance is
 *     the `cb` parameter, which carries [range(0, 0x100000)].
 *
 * Interface: {C453C318-B4A8-4188-80C4-73E6C7417DBF} v1.0, NDR32, explicit
 * primitive binding for Open, context handle for Close/Invoke.
 */
#include "wss.h"
#include "app.h"
#include "wss_rpc.h"

#include <rpc.h>

/* GetVersionExW is deprecated by the SDK but is exactly what the original calls. */
#pragma warning(disable : 4996)

/* rpcrt4 allocates the [out] buffers of Invoke with these. */
void *__RPC_USER MIDL_user_allocate(size_t cb)
{
    return LocalAlloc(LPTR, cb);
}

void __RPC_USER MIDL_user_free(void *pv)
{
    LocalFree(pv);
}

/*
 * Run-down routine for the WSS context handle: rpcrt4 calls it when the
 * association goes away.  The handle itself is owned by rpcrt4, so nothing to
 * release here; arm.exe has the same empty rundown.
 */
void __RPC_USER WSSCTX_rundown(WSSCTX hCtx)
{
    (void)hCtx;
}

/*
 * rpcrt4 reports failures by raising an RPC exception whose code is an RPC_STATUS
 * (a Win32-style number, e.g. 1702 = RPC_S_INVALID_BINDING).  Turn it into a
 * failing HRESULT so the rest of the tool sees an ordinary error.
 */
static HRESULT hr_from_rpc_status(DWORD st)
{
    if (st & 0x80000000u)
        return (HRESULT)st;
    if (st <= 0xFFFFu)
        return HRESULT_FROM_WIN32(st);
    return (HRESULT)st;
}

/* binding used by Open; kept alive until Close because the context handle
 * returned by the service is bound to it. */
static handle_t g_binding = NULL;

/*
 * 0x2774: compose the ncalrpc binding to WSSServiceTransportEndpoint-00001 and
 * authenticate as LOCAL SERVICE (the service runs as LOCAL SERVICE, so the
 * client impersonates the same identity class; on Windows 8+ an explicit
 * LOCAL SERVICE SID is used instead of a principal name).
 */
HRESULT wss_rpc_bind(handle_t *ph)
{
    RPC_STATUS st;
    RPC_WSTR stringBinding = NULL;
    handle_t binding = NULL;
    OSVERSIONINFOEXW vi;
    RPC_SECURITY_QOS_V3 qos3;
    PSID sid = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = { {0, 0, 0, 0, 0, 5} };
    HRESULT hr = S_OK;
    BOOL win8;

    if (ph == NULL)
        return E_INVALIDARG;
    *ph = NULL;

    st = RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL,
                                  (RPC_WSTR)L"WSSServiceTransportEndpoint-00001",
                                  NULL, &stringBinding);
    wss_trace(L"RpcStringBindingComposeW -> status %u", (unsigned)st);
    if (st != RPC_S_OK)
        return HRESULT_FROM_WIN32(I_RpcMapWin32Status(st));
    if (stringBinding == NULL)
        return E_INVALIDARG;

    st = RpcBindingFromStringBindingW(stringBinding, &binding);
    wss_trace(L"RpcBindingFromStringBindingW -> status %u", (unsigned)st);
    if (st != RPC_S_OK) {
        hr = HRESULT_FROM_WIN32(I_RpcMapWin32Status(st));
        goto done;
    }

    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (!GetVersionExW((OSVERSIONINFOW *)&vi)) {
        DWORD err = GetLastError();
        hr = err ? HRESULT_FROM_WIN32(err) : E_FAIL;
        goto done;
    }
    win8 = (vi.dwPlatformId == VER_PLATFORM_WIN32_NT) &&
           (vi.dwMajorVersion > 5 || (vi.dwMajorVersion == 5 && vi.dwMinorVersion >= 2));

    /*
     * The original passes a RPC_SECURITY_QOS_V3: Version selects the layout, so
     * the buffer must really be a V3 (Sid at +0x18), otherwise rpcrt4 reads past
     * the struct and RpcBindingSetAuthInfoExW fails with ERROR_INVALID_PARAMETER.
     * On Windows 8+ the server principal is not a name but the LOCAL SERVICE SID
     * (ARM 0x2774: AllocateAndInitializeSid(..., 0x13 = SECURITY_LOCAL_SERVICE_RID)).
     */
    memset(&qos3, 0, sizeof(qos3));
    qos3.Version = win8 ? RPC_C_SECURITY_QOS_VERSION_3 : RPC_C_SECURITY_QOS_VERSION_2;
    qos3.Capabilities = RPC_C_QOS_CAPABILITIES_MUTUAL_AUTH;
    qos3.IdentityTracking = RPC_C_QOS_IDENTITY_DYNAMIC;
    qos3.ImpersonationType = RPC_C_IMP_LEVEL_IMPERSONATE;
    qos3.AdditionalSecurityInfoType = 0;

    {
        RPC_WSTR principal = (RPC_WSTR)L"NT AUTHORITY\\LOCALSERVICE";
        if (win8) {
            if (!AllocateAndInitializeSid(&ntAuthority, 1, SECURITY_LOCAL_SERVICE_RID,
                                          0, 0, 0, 0, 0, 0, 0, &sid)) {
                DWORD err = GetLastError();
                wss_trace(L"AllocateAndInitializeSid failed err=%u", (unsigned)err);
                hr = err ? HRESULT_FROM_WIN32(err) : E_FAIL;
                goto done;
            }
            principal = NULL;
            qos3.Sid = sid;
        }
        st = RpcBindingSetAuthInfoExW(binding, principal,
                                      RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                                      RPC_C_AUTHN_WINNT, NULL,
                                      RPC_C_AUTHZ_NONE, (PRPC_SECURITY_QOS)&qos3);
        wss_trace(L"RpcBindingSetAuthInfoExW -> status %u (win8=%d qos=%u sid=%p)",
                  (unsigned)st, (int)win8, (unsigned)qos3.Version, (void *)qos3.Sid);
        if (st != RPC_S_OK) {
            hr = HRESULT_FROM_WIN32(I_RpcMapWin32Status(st));
            goto done;
        }
    }

    *ph = binding;
    binding = NULL;

done:
    if (stringBinding != NULL)
        RpcStringFreeW(&stringBinding);
    if (sid != NULL)
        FreeSid(sid);
    if (binding != NULL)
        RpcBindingFree(&binding);
    return hr;
}

/*
 * 0x2990: proc 0, Open.  The explicit binding handle is the first stack slot
 * (FC_BIND_PRIMITIVE); the service answers with a context handle that the
 * remaining calls carry instead of the binding.
 */
HRESULT wss_rpc_session_open(int *pSession)
{
    handle_t binding = NULL;
    WSSCTX ctx = NULL;
    HRESULT hr;

    if (pSession == NULL)
        return E_INVALIDARG;
    *pSession = 0;

    if (g_binding == NULL) {
        hr = wss_rpc_bind(&g_binding);
        if (hr < 0)
            return hr;
    }
    binding = g_binding;

    wss_trace(L"Open(&ctx) binding=%p", (void *)binding);
    RpcTryExcept {
        hr = Open(binding, &ctx);
    }
    RpcExcept(I_RpcExceptionFilter(RpcExceptionCode())) {
        hr = hr_from_rpc_status((DWORD)RpcExceptionCode());
    }
    RpcEndExcept

    wss_trace(L"Open -> 0x%08x ctx=%p", (u32)hr, (void *)ctx);
    if (hr >= 0 && ctx != NULL)
        *pSession = (int)(intptr_t)ctx;
    return hr;
}

/* 0x2a3c: proc 1, Close.  The context handle is in/out by pointer: the service
 * invalidates it, so the local copy is cleared afterwards. */
HRESULT wss_rpc_session_close(int *pSession)
{
    WSSCTX ctx;
    HRESULT hr;

    if (pSession == NULL || *pSession == 0)
        return S_OK;

    ctx = (WSSCTX)(intptr_t)*pSession;
    wss_trace(L"Close(ctx=%p)", (void *)ctx);
    RpcTryExcept {
        hr = Close(&ctx);
    }
    RpcExcept(I_RpcExceptionFilter(RpcExceptionCode())) {
        hr = hr_from_rpc_status((DWORD)RpcExceptionCode());
    }
    RpcEndExcept
    wss_trace(L"Close -> 0x%08x", (u32)hr);

    *pSession = 0;
    if (g_binding != NULL) {
        RpcBindingFree(&g_binding);
        g_binding = NULL;
    }
    return hr;
}

/*
 * 0x2ac4: proc 2, Invoke: one request blob in, one response blob out.  The
 * request is capped at 1 MiB, the range the original declares for `cb`.
 */
HRESULT wss_rpc_invoke(int session, int opcode, const void *req, u32 reqlen, WssBag **pResult)
{
    WSSCTX ctx = (WSSCTX)(intptr_t)session;
    static const unsigned char kEmpty[1] = { 0 };
    long cbResp = 0;
    unsigned char *pbResp = NULL;
    HRESULT hr;

    /* 0x8007006F = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); the original caps
     * the request at 1 MiB (and the IDL declares the same [range]). */
    if (reqlen > 0x100000)
        return (HRESULT)0x8007006fL;

    wss_trace(L"Invoke(ctx=%p opcode=0x%x cb=%u)", (void *)ctx,
              (unsigned)opcode, (unsigned)reqlen);
    wss_trace_hex(L"request", req, reqlen);
    RpcTryExcept {
        hr = Invoke(ctx, (OP)opcode, (unsigned long)reqlen,
                    (unsigned char *)(req != NULL ? req : (const void *)kEmpty),
                    &cbResp, &pbResp);
    }
    RpcExcept(I_RpcExceptionFilter(RpcExceptionCode())) {
        hr = hr_from_rpc_status((DWORD)RpcExceptionCode());
    }
    RpcEndExcept

    wss_trace(L"Invoke -> 0x%08x, resp=%p len=%ld", (u32)hr, (void *)pbResp, cbResp);
    if (hr >= 0 && pResult != NULL && pbResp != NULL)
        hr = wss_bag_deserialize(pResult, pbResp, (u32)cbResp);

    if (pbResp != NULL)
        MIDL_user_free(pbResp);
    return hr;
}

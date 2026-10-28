/* app.h - application level declarations for the arm.exe reconstruction. */
#ifndef APP_H
#define APP_H

#include "wss.h"

/* rpc.c */
HRESULT wss_rpc_bind(handle_t *ph);
HRESULT wss_rpc_session_open(int *pSession);
HRESULT wss_rpc_session_close(int *pSession);
HRESULT wss_rpc_invoke(int session, int opcode, const void *req, u32 reqlen, WssBag **pResult);

/* svc.c */
HRESULT wss_service_start(void);

/* file.c */
HRESULT wss_read_file(const wchar_t *path, u32 *pcb, void **ppv);

/* print.c - console colour values used by the original: 0xa usage, 0xc error, 0xe success */
void wss_printf(WORD color, const wchar_t *fmt, ...);
void wss_print_error(HRESULT hr);

/* diagnostics, enabled with ARM_X86_TRACE=1 */
extern int g_wssTrace;
void wss_trace(const wchar_t *fmt, ...);
void wss_trace_hex(const wchar_t *tag, const void *pv, u32 cb);

/* license.c */
HRESULT wss_session_ensure(int *pSession);
HRESULT wss_install_license(int session, u32 cb, const void *data);
HRESULT wss_remove_license(int session);
HRESULT wss_check_license(FILETIME *pExpiration);

#endif /* APP_H */

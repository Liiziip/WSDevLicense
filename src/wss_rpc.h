

/* this ALWAYS GENERATED file contains the definitions for the interfaces */


 /* File created by MIDL compiler version 8.01.0622 */
/* at Tue Jan 19 11:14:07 2038
 */
/* Compiler settings for src\wss_rpc.idl, src\wss_rpc.acf:
    Oicf, W1, Zp8, env=Win32 (32b run), target_arch=X86 8.01.0622 
    protocol : dce , ms_ext, c_ext, robust
    error checks: allocation ref bounds_check enum stub_data 
    VC __declspec() decoration level: 
         __declspec(uuid()), __declspec(selectany), __declspec(novtable)
         DECLSPEC_UUID(), MIDL_INTERFACE()
*/
/* @@MIDL_FILE_HEADING(  ) */

#pragma warning( disable: 4049 )  /* more than 64k source lines */


/* verify that the <rpcndr.h> version is high enough to compile this file*/
#ifndef __REQUIRED_RPCNDR_H_VERSION__
#define __REQUIRED_RPCNDR_H_VERSION__ 475
#endif

#include "rpc.h"
#include "rpcndr.h"

#ifndef __RPCNDR_H_VERSION__
#error this stub requires an updated version of <rpcndr.h>
#endif /* __RPCNDR_H_VERSION__ */


#ifndef __wss_rpc_h__
#define __wss_rpc_h__

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
#pragma once
#endif

/* Forward Declarations */ 

/* header files for imported files */
#include "unknwn.h"

#ifdef __cplusplus
extern "C"{
#endif 


#ifndef __Wss_INTERFACE_DEFINED__
#define __Wss_INTERFACE_DEFINED__

/* interface Wss */
/* [explicit_handle][unique][version][uuid] */ 

typedef /* [context_handle] */ void *WSSCTX;

typedef 
enum _OP
    {
        OP_INSTALL	= 0,
        OP_REMOVE	= 0xc
    } 	OP;

HRESULT Open( 
    /* [in] */ handle_t IDL_handle,
    /* [out] */ WSSCTX *pctx);

HRESULT Close( 
    /* [out][in] */ WSSCTX *pctx);

HRESULT Invoke( 
    /* [in] */ WSSCTX ctx,
    /* [in] */ OP op,
    /* [range][in] */ unsigned long cb,
    /* [size_is][in] */ unsigned char p[  ],
    /* [out] */ long *pcb,
    /* [size_is][size_is][out] */ unsigned char **pp);



extern RPC_IF_HANDLE Wss_v1_0_c_ifspec;
extern RPC_IF_HANDLE Wss_v1_0_s_ifspec;
#endif /* __Wss_INTERFACE_DEFINED__ */

/* Additional Prototypes for ALL interfaces */

void __RPC_USER WSSCTX_rundown( WSSCTX );

/* end of Additional Prototypes */

#ifdef __cplusplus
}
#endif

#endif



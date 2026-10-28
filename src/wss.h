/*
 * wss.h - shared types for the x86 reconstruction of arm.exe
 *         ("Utility for Windows Store Developer Licensing", WinRT 2011).
 *
 * The original is a 32-bit ARM (Thumb-2) PE built by MSVC for Windows RT.
 * This reconstruction keeps the original data layouts (32-bit, 4-byte
 * pointers, 20-byte property values, 0x1c-byte bag object) so the wire
 * format produced by the serializer is byte-identical.
 */
#ifndef WSS_H
#define WSS_H

#include <windows.h>
#include <stddef.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef int            i32;

/* ---- value types, as used by the property bag (field "vt") ------------- */
#define WSS_VT_PTR        1          /* 0x00000001: raw pointer / opaque     */
#define WSS_VT_STRING     2          /* 0x00000002: NUL-terminated wchar_t*  */
#define WSS_VT_BLOB       4          /* 0x00000004: pv + cb byte block       */
#define WSS_VT_ARRAY_PTR  0x80000002 /* 0x80000002: array of pointers        */
#define WSS_VT_ARRAY_BLOB 0x80000004 /* 0x80000004: array of blobs           */

/* 20-byte property value (matches the 0x14-byte stack structs). */
typedef struct WssValue {
    const wchar_t *name;   /* +0x00 */
    u32            vt;     /* +0x04 */
    union {
        const void    *pv;      /* vt=4 / array types */
        const wchar_t *str;     /* vt=2 */
        u32            u32val;  /* vt=1 */
    } u;                   /* +0x08 */
    u32            cb;     /* +0x0c */
    void          *owned;  /* +0x10: internally owned copy, freed on clear */
} WssValue;

/* ---- error plumbing ---------------------------------------------------- */
/* Original: FUN_00402538(hr) - only traps for the "expected" HRESULT set. */
extern HRESULT g_wssThrowHr;   /* DAT_00402550-like global; 0 disables      */
void wss_check(HRESULT hr);
/* Original: FUN_00402554() - failure reporting hook; logging only. */
#define wss_pending() ((void)0)

/* ---- property bag (vtable 0x401B68, object size 0x1c) ------------------ */
/*
 * NOTE: the bag stores its entries in a private 20-byte layout that differs
 * from WssValue: {+0x00 vt, +0x04 name, +0x08 u, +0x0c cb, +0x10 owned}.
 * WssValue below is the layout that crosses the get/set interface
 * ({+0x00 name, +0x04 vt, ...}).  A value returned by wss_bag_get_value()
 * borrows the bag's storage and must not be freed.
 */
typedef struct WssBag WssBag;

HRESULT wss_bag_create(WssBag **out);
HRESULT wss_bag_get_count(WssBag *bag, u32 *count);
HRESULT wss_bag_get_value(WssBag *bag, u32 index, WssValue *out);
HRESULT wss_bag_set_value(WssBag *bag, u32 tag, const WssValue *value);
void    wss_bag_release(WssBag *bag);

/* deep copy / normalisation helpers used by the bag itself (0x39f0 / 0x3e74).
 *
 * WARNING: these two operate on the bag's PRIVATE 20-byte entry layout
 * ({+0 vt, +4 name, +8 u, +0xc cb, +0x10 owned}), not on WssValue.  The
 * pointer type below is a convenience so that bag.c can reuse one prototype;
 * callers must cast a BagEntry*, never a real WssValue*, and no other module
 * in this project calls them.  They are internal to bag.c in practice. */
HRESULT wss_value_copy(WssValue *dst_entry, const WssValue *src);
void    wss_value_clear(WssValue *entry);

/* ---- serializer writer (vtable 0x401BA8, object size 0x24) ------------- */
typedef struct WssWriter WssWriter;

HRESULT wss_writer_create(WssWriter **out);                     /* 0x4258 */
HRESULT wss_writer_add(WssWriter *w, const WssValue *value);     /* 0x4b4c */
HRESULT wss_writer_get_buffer(WssWriter *w, void **ppv, u32 *pcb); /* 0x4414 */
void    wss_writer_release(WssWriter *w);

/* bag -> bytes (0x4f20) */
HRESULT wss_bag_serialize(WssBag *bag, void **ppv, u32 *pcb);
/* bytes -> bag (0x40dc) */
HRESULT wss_bag_deserialize(WssBag **out, const void *pv, u32 cb);

#endif /* WSS_H */

/*
 * bag.c -- the "property bag" object of arm.exe ("Utility for Windows Store
 *          Developer Licensing", ARM/Thumb-2, WinRT 2011) transcribed to
 *          portable C for MSVC 2019 / x86.
 *
 * Implements the WssBag API of wss.h:
 *
 *      wss_bag_create        0x4031d0     wss_value_copy   0x4039f0
 *      wss_bag_get_count     0x4032bc     wss_value_clear  0x403e74
 *      wss_bag_get_value     0x4032d4
 *      wss_bag_set_value     0x403558
 *      wss_bag_release       0x4030cc
 *
 * ---------------------------------------------------------------------------
 * OBJECT LAYOUT (0x1c bytes, vtable 0x401b68)
 * ---------------------------------------------------------------------------
 *   +0x00  const BagVtbl  *vtbl     primary vtable (0x401b68, 10 slots)
 *   +0x04  const BagVtbl2 *vtbl2    second interface (0x401b90, 6 slots)
 *   +0x08  u32  vec.cap             entry vector: capacity
 *   +0x0c  u32  vec.count           entry vector: element count
 *   +0x10  void*vec.data            entry vector: array of 0x14-byte entries
 *   +0x14  u32  locked              0 = writable, != 0 = read-only
 *   +0x18  u32  refs                reference count (atomic), starts at 1
 *
 * The second interface lives at object+4 and its first three slots are
 * adjustor thunks (0x4030b4 / 0x4030c0 / 0x403150) that subtract 4 from
 * `this` and jump to the primary QueryInterface/AddRef/Release, so both
 * interfaces share the single refcount at +0x18.
 *
 * ---------------------------------------------------------------------------
 * TWO DIFFERENT 0x14-BYTE VALUE LAYOUTS (important!)
 * ---------------------------------------------------------------------------
 * The stored entry (private, see BagEntry below) is laid out as
 *
 *   +0x00 u32 vt     +0x04 wchar_t *name   +0x08 u   +0x0c cb   +0x10 owned
 *
 * while the WssValue that crosses the GetValue/SetValue interface is
 *
 *   +0x00 wchar_t *name   +0x04 u32 vt   +0x08 u   +0x0c cb   (+0x10 unused)
 *
 * i.e. the first two fields are swapped.  This is not a guess: GetValue
 * (0x4032d4) reads entry[+0x04] as the name and compares entry[+0x00]
 * against the vt constants, then stores them as out[0]/out[1]; the internal
 * helpers 0x4039f0 / 0x403b68 / 0x403e74 all treat +0x00 as vt, +0x04 as the
 * counted name string and +0x10 as the owned data block.
 *
 * Consequently wss_value_copy()/wss_value_clear() -- which are literally
 * 0x4039f0 and 0x403e74 -- operate on the PRIVATE entry layout (5 fields),
 * and must not be applied to a value that was filled in by
 * wss_bag_get_value() (that one only borrows the bag's storage).
 *
 * ---------------------------------------------------------------------------
 * NOTES ON THE TRANSCRIPTION
 * ---------------------------------------------------------------------------
 *  - `dmb`, `ldrex`/`strex` spin loops and ARM stack probes are rendered as
 *    InterlockedIncrement/InterlockedDecrement or plain stores; there are no
 *    calls to DataMemoryBarrier/ExclusiveAccess in the original machine code.
 *  - LocalAlloc(0x40, n) is LocalAlloc(LPTR, n); HeapAlloc(GetProcessHeap(),
 *    0, n) / HeapFree(GetProcessHeap(), 0, p) stay as in the original.
 *  - Names are stored as counted strings: HeapAlloc(4 + 2*(len+1)), the
 *    wchar_t count in the first dword, the text (NUL terminated) at +4.
 *  - Every failure exit that the original passes through wss_check() does so
 *    here as well, in the same order; wss_pending() is a no-op macro.
 */

#include "wss.h"
#include <string.h>

/* ------------------------------------------------------------------------ */
/* HRESULT values as found in the original literal pools                     */
/* ------------------------------------------------------------------------ */
#define WSS_E_NOINTERFACE  ((HRESULT)0x80004002L)   /* 0x403cf0 */
#define WSS_E_POINTER      ((HRESULT)0x80004003L)   /* 0x4031cc, 0x403cec   */
#define WSS_E_UNEXPECTED   ((HRESULT)0x8000FFFFL)   /* 0x403358, 0x4034e4, 0x40367c, 0x40376c */
#define WSS_E_OUTOFMEMORY  ((HRESULT)0x8007000EL)   /* 0x4032a4, 0x4034e0, 0x4038c4, 0x4039e4, 0x403b54, 0x403fd8 */
#define WSS_E_ACCESSDENIED ((HRESULT)0x80070005L)   /* 0x4035c4, 0x4037d0, 0x403c54, 0x403e40, 0x403e70 */
#define WSS_E_INVALIDARG   ((HRESULT)0x80070057L)   /* 0x4030b0, 0x40335c, 0x403554, 0x4035c8, 0x403b64, 0x403c58, 0x403e00 */
/* HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW) -- the "safe int" failure. */
#define WSS_E_OVERFLOW     ((HRESULT)0x80070216L)   /* 0x4034ec, 0x403678, 0x403768, 0x4038cc, 0x4039e8, 0x403e04, 0x403fe8 */

#define BAG_ENTRY_SIZE     0x14u
#define BAG_INITIAL_CAP    0x20u
#define BAG_OBJ_SIZE       0x1cu

/* ------------------------------------------------------------------------ */
/* Private 0x14-byte stored entry.                                           */
/* ------------------------------------------------------------------------ */
typedef struct BagEntry {
    u32      vt;      /* +0x00  WSS_VT_*                                      */
    wchar_t *name;    /* +0x04  counted string: ((u32 *)name)[-1] = wchar count */
    union {
        const void    *pv;     /* vt=1 / vt=2 / vt=4 / array types */
        const wchar_t *str;
        u32            u32val;
    } u;              /* +0x08                                               */
    u32      cb;      /* +0x0c  byte count (vt=4, 0x80000004 only)            */
    void    *owned;   /* +0x10  heap block owned by the entry, freed by clear */
} BagEntry;

/* The entry vector, at object+0x08. */
typedef struct BagVec {
    u32   cap;        /* +0x00 */
    u32   count;      /* +0x04 */
    void *data;       /* +0x08 */
} BagVec;

typedef struct BagVtbl  BagVtbl;
typedef struct BagVtbl2 BagVtbl2;

struct WssBag {
    const BagVtbl  *vtbl;    /* +0x00 */
    const BagVtbl2 *vtbl2;   /* +0x04 */
    BagVec          vec;     /* +0x08 */
    u32             locked;  /* +0x14 */
    u32             refs;    /* +0x18 */
};

struct BagVtbl {                                  /* 0x401b68 */
    HRESULT (__stdcall *QueryInterface)(WssBag *, const GUID *, void **);  /* +0x00 0x403178 */
    u32     (__stdcall *AddRef)(WssBag *);                                 /* +0x04 0x40315c */
    u32     (__stdcall *Release)(WssBag *);                                /* +0x08 0x4030cc */
    HRESULT (__stdcall *GetCount)(WssBag *, u32 *);                        /* +0x0c 0x4032bc */
    HRESULT (__stdcall *GetValue)(WssBag *, u32, WssValue *);              /* +0x10 0x4032d4 */
    HRESULT (__stdcall *GetEntry)(WssBag *, u32, void **);                 /* +0x14 0x4034f0 */
    HRESULT (__stdcall *SetValue)(WssBag *, u32, const WssValue *);        /* +0x18 0x403558 */
    HRESULT (__stdcall *Append)(WssBag *, const WssValue *);               /* +0x1c 0x403770 */
    HRESULT (__stdcall *SetValueAt)(WssBag *, u32, const WssValue *);      /* +0x20 0x403be4 */
    HRESULT (__stdcall *QueryInterface2)(WssBag *, const GUID *, void **); /* +0x24 0x403c5c */
};

struct BagVtbl2 {                                 /* 0x401b90, this == object+4 */
    HRESULT (__stdcall *QueryInterface)(void *, const GUID *, void **);    /* +0x00 0x4030b4 thunk */
    u32     (__stdcall *AddRef)(void *);                                   /* +0x04 0x4030c0 thunk */
    u32     (__stdcall *Release)(void *);                                  /* +0x08 0x403150 thunk */
    HRESULT (__stdcall *Lock)(void *);                                     /* +0x0c 0x403cf4 */
    HRESULT (__stdcall *RemoveAt)(void *, u32);                            /* +0x10 0x403e08 */
    HRESULT (__stdcall *Clear)(void *);                                    /* +0x14 0x403e44 */
};

/* Compile-time guards: object 0x1c bytes, entry 0x14 bytes, WssValue 0x14. */
typedef char bag_obj_size_check[(sizeof(struct WssBag) == BAG_OBJ_SIZE) ? 1 : -1];
typedef char bag_entry_size_check[(sizeof(BagEntry) == BAG_ENTRY_SIZE) ? 1 : -1];
typedef char bag_value_size_check[(sizeof(WssValue) == BAG_ENTRY_SIZE) ? 1 : -1];

/* ------------------------------------------------------------------------ */
/* Interface ids (verbatim copies of the 16-byte blobs in .text)             */
/* ------------------------------------------------------------------------ */
static const GUID g_iidUnknown =                            /* 0x4011b0 */
    { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const GUID g_iidBag =                                /* 0x4017a8 */
    { 0x7DF9A4E0, 0xFCAB, 0x4525, { 0xA4, 0x20, 0x78, 0x75, 0x3D, 0x41, 0x60, 0x5C } };
static const GUID g_iidSecond =                             /* 0x401ad0, -> object+4 */
    { 0x96B97320, 0xED0E, 0x4D9F, { 0xB3, 0x90, 0x6C, 0x17, 0xEA, 0xF6, 0x72, 0x77 } };
static const GUID g_iidExtraA =                             /* 0x4017b8 */
    { 0xBE73DD34, 0x4DAD, 0x4AC5, { 0xBB, 0xE0, 0x79, 0x30, 0xF4, 0x5C, 0xED, 0x73 } };
static const GUID g_iidExtraB =                             /* 0x401798 */
    { 0xBB5DFAFC, 0x1EF2, 0x436E, { 0xBD, 0xFB, 0xB2, 0x02, 0x4A, 0xAF, 0xAB, 0xF7 } };

/* Live-object counter (0x40733c): bumped by create, dropped by Release.    */
static volatile LONG g_bagObjects = 0;

/* ------------------------------------------------------------------------ */
/* Forward declarations                                                      */
/* ------------------------------------------------------------------------ */
static HRESULT __stdcall bag_QueryInterface(WssBag *bag, const GUID *riid, void **out);
static HRESULT __stdcall bag_QueryInterface2(WssBag *bag, const GUID *riid, void **out);
static u32     __stdcall bag_AddRef(WssBag *bag);
static u32     __stdcall bag_Release(WssBag *bag);
static HRESULT __stdcall bag_GetCount(WssBag *bag, u32 *count);
static HRESULT __stdcall bag_GetValue(WssBag *bag, u32 index, WssValue *out);
static HRESULT __stdcall bag_GetEntry(WssBag *bag, u32 index, void **out);
static HRESULT __stdcall bag_SetValue(WssBag *bag, u32 tag, const WssValue *value);
static HRESULT __stdcall bag_Append(WssBag *bag, const WssValue *value);
static HRESULT __stdcall bag_SetValueAt(WssBag *bag, u32 index, const WssValue *value);

static HRESULT __stdcall bag2_QueryInterface(void *self, const GUID *riid, void **out);
static u32     __stdcall bag2_AddRef(void *self);
static u32     __stdcall bag2_Release(void *self);
static HRESULT __stdcall bag2_Lock(void *self);
static HRESULT __stdcall bag2_RemoveAt(void *self, u32 index);
static HRESULT __stdcall bag2_Clear(void *self);

static HRESULT bag_vec_resize(BagVec *v, u32 newcap, u32 noise);
static HRESULT bag_vec_grow(BagVec *v, u32 needed);
static HRESULT bag_vec_insert(BagVec *v, i32 index, const BagEntry *src, u32 noise);
static void    bag_vec_remove(BagVec *v, i32 index, u32 noise1, u32 noise2);
static void    bag_entry_assign(BagEntry *dst, BagEntry *src);
static HRESULT bag_entry_copy_block(const BagEntry *e, void **out, u32 noise);
static HRESULT bag_str_alloc(const wchar_t *src, u32 len, wchar_t **out);
static HRESULT bag_entry_set_string(BagEntry *e, const wchar_t *src, u32 len);

/* ------------------------------------------------------------------------ */
/* The two vtables (0x401b68 and 0x401b90).                                  */
/* ------------------------------------------------------------------------ */
static const BagVtbl g_bagVtbl = {
    bag_QueryInterface,
    bag_AddRef,
    bag_Release,
    bag_GetCount,
    bag_GetValue,
    bag_GetEntry,
    bag_SetValue,
    bag_Append,
    bag_SetValueAt,
    bag_QueryInterface2
};

static const BagVtbl2 g_bagVtbl2 = {
    bag2_QueryInterface,
    bag2_AddRef,
    bag2_Release,
    bag2_Lock,
    bag2_RemoveAt,
    bag2_Clear
};

/* ======================================================================== */
/* 0x403ec0  bag_vec_resize(v, newcap)                                       */
/*                                                                          */
/* Grows or shrinks the entry array to `newcap` elements: allocates a new    */
/* zeroed block, copies the first min(count,newcap) entries, clears the      */
/* entries that fall off the end (they own heap blocks), frees the old       */
/* array and commits capacity/count/data.  A newcap <= 0 just empties it.    */
/* ======================================================================== */
static HRESULT bag_vec_resize(BagVec *v, u32 newcap, u32 noise)
{
    HRESULT hr = S_OK;
    void *newdata = NULL;
    u32 keep;

    (void)noise;

    if (v->cap == newcap) {
        wss_pending();
        return S_OK;
    }

    keep = v->count;
    if ((i32)newcap < (i32)v->count) {
        keep = newcap;
    }

    if ((i32)newcap > 0) {
        u32 bytes = newcap * BAG_ENTRY_SIZE;

        /* 0x403ec0 checks the multiply with the 0xCCCCCCCD magic. */
        if ((newcap != 0) && (bytes / BAG_ENTRY_SIZE != newcap)) {
            hr = WSS_E_OVERFLOW;
            wss_check(hr);
            wss_pending();
            return hr;
        }
        wss_pending();

        newdata = HeapAlloc(GetProcessHeap(), 0, bytes);
        if (newdata == NULL) {
            hr = WSS_E_OUTOFMEMORY;
            wss_check(hr);
            wss_pending();
            return hr;
        }
        if (keep != 0) {
            memcpy(newdata, v->data, (size_t)keep * BAG_ENTRY_SIZE);
        }
    }

    if ((i32)newcap < (i32)v->count) {
        u32 i;
        for (i = newcap; i < v->count; i++) {
            BYTE *p = (BYTE *)v->data + (size_t)i * BAG_ENTRY_SIZE;
            if (p != NULL) {
                wss_value_clear((WssValue *)p);
            }
        }
    }

    if (v->data != NULL) {
        HeapFree(GetProcessHeap(), 0, v->data);
        v->data = NULL;
    }
    v->data = newdata;
    v->count = keep;
    v->cap = newcap;

    wss_pending();
    return S_OK;
}

/* ======================================================================== */
/* 0x403b68  bag_entry_assign(dst, src)                                      */
/*                                                                          */
/* Move-assign a private entry: the name block (freed as name-4) and the     */
/* owned data block are transferred, the previous ones are released.        */
/* ======================================================================== */
static void bag_entry_assign(BagEntry *dst, BagEntry *src)
{
    wchar_t *name = src->name;
    void *owned = src->owned;

    src->name = NULL;
    src->owned = NULL;

    dst->vt = src->vt;

    if (dst->name != NULL) {
        HeapFree(GetProcessHeap(), 0, (BYTE *)dst->name - 4);
        wss_pending();
        dst->name = NULL;
    }
    dst->name = name;

    dst->u.pv = src->u.pv;
    dst->cb = src->cb;

    if (dst->owned != NULL) {
        HeapFree(GetProcessHeap(), 0, dst->owned);
        dst->owned = NULL;
    }
    dst->owned = owned;
}

/* ======================================================================== */
/* 0x403e74  wss_value_clear(v)  -- release a private entry's owned blocks   */
/*                                                                          */
/* Order is exactly as in the original: the data block at +0x10 first, then  */
/* the counted name string at +0x04 (the allocation starts 4 bytes early).   */
/* ======================================================================== */
void wss_value_clear(WssValue *v_)
{
    BagEntry *v = (BagEntry *)v_;

    if (v->owned != NULL) {
        HeapFree(GetProcessHeap(), 0, v->owned);
        v->owned = NULL;
    }
    if (v->name != NULL) {
        HeapFree(GetProcessHeap(), 0, (BYTE *)v->name - 4);
        wss_pending();
        v->name = NULL;
    }
}

/* ======================================================================== */
/* 0x4037d4  bag_str_alloc(src, len, out)                                    */
/*                                                                          */
/* Allocates 4 + 2*(len+1) bytes, stores the wchar_t count in the first      */
/* dword, copies `len` wide characters and NUL-terminates.  src == NULL      */
/* yields *out == NULL and S_OK.                                            */
/* ======================================================================== */
static HRESULT bag_str_alloc(const wchar_t *src, u32 len, wchar_t **out)
{
    HRESULT hr = S_OK;
    u32 n;
    u32 bytes;
    u32 *buf;

    if (src == NULL) {
        *out = NULL;
        wss_pending();
        return S_OK;
    }

    n = len + 1;
    if (n < len) {
        hr = WSS_E_OVERFLOW;
        wss_check(hr);
        wss_pending();
        return hr;
    }
    wss_pending();

    bytes = n << 1;
    if ((n & 0x7fffffffu) != n) {
        hr = WSS_E_OVERFLOW;
        wss_check(hr);
        wss_pending();
        return hr;
    }
    wss_pending();

    /* room for the leading count dword: bytes + 4, overflow checked. */
    if (bytes + 4u < bytes) {
        hr = WSS_E_OVERFLOW;
        wss_check(hr);
        wss_pending();
        return hr;
    }
    bytes += 4u;
    wss_pending();

    buf = (u32 *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (buf == NULL) {
        hr = WSS_E_OUTOFMEMORY;
        wss_check(hr);
        wss_pending();
        return hr;
    }

    buf[0] = len;
    memcpy(buf + 1, src, (size_t)len * sizeof(wchar_t));
    ((wchar_t *)(buf + 1))[len] = 0;
    *out = (wchar_t *)(buf + 1);

    wss_pending();
    return S_OK;
}

/* ======================================================================== */
/* 0x4038d0  bag_entry_set_string(e, src, len)                               */
/*                                                                          */
/* Allocates the inline copy of a vt=2 string into the entry: u and cb are   */
/* set, the buffer is recorded in `owned` (after freeing any previous one).  */
/* ======================================================================== */
static HRESULT bag_entry_set_string(BagEntry *e, const wchar_t *src, u32 len)
{
    HRESULT hr = S_OK;
    u32 n;
    u32 bytes;
    void *buf;
    void *old;

    n = len + 1;
    if (n < len) {
        hr = WSS_E_OVERFLOW;
        wss_check(hr);
    }
    wss_pending();

    if (hr >= 0) {
        bytes = n << 1;
        if ((n & 0x7fffffffu) != n) {
            hr = WSS_E_OVERFLOW;
            wss_check(hr);
        }
        wss_pending();

        if (hr >= 0) {
            buf = HeapAlloc(GetProcessHeap(), 0, bytes);
            if (buf == NULL) {
                hr = WSS_E_OUTOFMEMORY;
            } else {
                /* copy everything but the final wchar_t, then NUL it. */
                u32 copy = bytes - 2u;
                if (copy < bytes) {
                    memcpy(buf, src, copy);
                    *(u16 *)((BYTE *)buf + (copy & 0xfffffffeu)) = 0;

                    e->u.pv = buf;
                    e->cb = bytes;

                    old = e->owned;
                    if (old != NULL) {
                        HeapFree(GetProcessHeap(), 0, old);
                        e->owned = NULL;
                    }
                    e->owned = buf;

                    wss_pending();
                    return S_OK;
                }
                hr = WSS_E_OVERFLOW;
            }
        }
    }

    wss_check(hr);
    wss_pending();
    return hr;
}

/* ======================================================================== */
/* 0x4039f0  wss_value_copy(dst, src)                                        */
/*                                                                          */
/* dst is a PRIVATE 0x14-byte entry, src is the public WssValue that the     */
/* caller hands to SetValue/Append/SetValueAt.  Deep copy: the name gets its  */
/* own counted allocation, vt=2 strings and vt=4 blobs get their own data    */
/* block, vt=1 / array-of-pointers just keep the value.  On success the      */
/* temporary is moved into dst (freeing dst's previous contents).           */
/* ======================================================================== */
HRESULT wss_value_copy(WssValue *dst_, const WssValue *src)
{
    BagEntry *dst = (BagEntry *)dst_;
    BagEntry  tmp;
    HRESULT   hr = S_OK;
    u32       len;
    void     *old;

    memset(&tmp, 0, sizeof(tmp));
    tmp.name = NULL;
    tmp.owned = NULL;

    tmp.vt = src->vt;

    /* ---- name: bounded wcslen, then counted copy -------------------- */
    len = 0;
    if (src->name != NULL) {
        const wchar_t *s = src->name;
        u32 i = 0;
        u32 limit = 0x7fffffffu;

        while ((i < limit) && (s[i] != 0)) {
            i++;
        }
        if (i == limit) {
            hr = WSS_E_INVALIDARG;
            wss_check(hr);
            wss_pending();
            if (hr < 0) {
                wss_check(hr);
            }
            wss_pending();
            wss_value_clear((WssValue *)&tmp);
            return hr;
        }
        len = i;
    }

    hr = bag_str_alloc(src->name, len, &tmp.name);
    if (hr < 0) {
        wss_check(hr);
    }
    old = tmp.owned;                    /* always NULL here (see 0x403a80) */

    wss_pending();
    if (hr < 0) {
        wss_check(hr);
        wss_pending();
        wss_value_clear((WssValue *)&tmp);
        return hr;
    }

    /* ---- payload, by vt -------------------------------------------- */
    switch (src->vt) {
    case 1:                             /* raw pointer / opaque value */
        tmp.u.pv = src->u.pv;
        break;

    case 2:                             /* wide string                */
        if (src->u.str != NULL) {
            hr = bag_entry_set_string(&tmp, src->u.str,
                                      (u32)wcslen(src->u.str));
            if (hr < 0) {
                wss_check(hr);
                wss_pending();
                wss_value_clear((WssValue *)&tmp);
                return hr;
            }
        }
        break;

    case 4:                             /* byte blob                  */
        if (src->cb != 0) {
            void *buf = HeapAlloc(GetProcessHeap(), 0, src->cb);

            if (old != NULL) {
                HeapFree(GetProcessHeap(), 0, old);
                tmp.owned = NULL;
            }
            tmp.owned = buf;
            if (buf == NULL) {
                hr = WSS_E_OUTOFMEMORY;
                wss_check(hr);
                wss_pending();
                wss_value_clear((WssValue *)&tmp);
                return hr;
            }
            memcpy(buf, src->u.pv, src->cb);
            tmp.u.pv = buf;
            tmp.cb = src->cb;
        }
        break;

    case WSS_VT_ARRAY_BLOB:             /* 0x80000004: array of blobs */
        tmp.cb = src->cb;
        tmp.u.pv = src->u.pv;
        break;

    case WSS_VT_ARRAY_PTR:              /* 0x80000002: array of ptrs  */
        tmp.u.pv = src->u.pv;
        break;

    default:
        hr = WSS_E_INVALIDARG;
        wss_check(hr);
        wss_pending();
        wss_value_clear((WssValue *)&tmp);
        return hr;
    }

    bag_entry_assign(dst, &tmp);

    wss_pending();
    wss_value_clear((WssValue *)&tmp);
    return hr;
}

/* ======================================================================== */
/* 0x4035cc  bag_vec_grow(v, needed)                                         */
/*                                                                          */
/* Doubles the capacity (starting at 0x20) until it covers `needed`, then    */
/* resizes.  Every step is overflow-checked with 0x80070216.                 */
/* ======================================================================== */
static HRESULT bag_vec_grow(BagVec *v, u32 needed)
{
    HRESULT hr;
    u32 cap;

    cap = v->cap;

    if ((i32)cap < 0) {
        hr = WSS_E_UNEXPECTED;
        wss_check(hr);
        wss_pending();
        return hr;
    }
    if ((i32)cap >= (i32)needed) {
        wss_pending();
        return S_OK;
    }

    for (;;) {
        u32 next;

        if (cap == 0) {
            next = BAG_INITIAL_CAP;
        } else {
            if ((i32)cap < 0) {
                hr = WSS_E_INVALIDARG;
                wss_check(hr);
                wss_pending();
                return hr;
            }
            next = cap << 1;
            if ((cap & 0x7fffffffu) != cap) {
                hr = WSS_E_OVERFLOW;
                wss_check(hr);
                wss_pending();
                return hr;
            }
            if ((i32)next < 0) {
                hr = WSS_E_OVERFLOW;
                wss_check(hr);
                wss_pending();
                return hr;
            }
            wss_pending();
        }
        cap = next;
        if ((i32)cap >= (i32)needed) {
            break;
        }
    }

    hr = bag_vec_resize(v, cap, 0);
    if (hr < 0) {
        wss_check(hr);
    }
    wss_pending();
    return hr;
}

/* ======================================================================== */
/* 0x403680  bag_vec_insert(v, index, src)                                   */
/*                                                                          */
/* Makes room at `index` (memmove of the tail), zeroes the slot, moves the    */
/* prepared entry into it and bumps the count.                               */
/* ======================================================================== */
static HRESULT bag_vec_insert(BagVec *v, i32 index, const BagEntry *src, u32 noise)
{
    HRESULT hr;
    u32 needed;

    (void)noise;

    hr = WSS_E_OVERFLOW;
    if ((i32)v->count < 0) {
        hr = WSS_E_UNEXPECTED;
        wss_check(hr);
        wss_pending();
        return hr;
    }

    needed = v->count + 1u;
    if (needed < v->count) {
        hr = WSS_E_OVERFLOW;
        wss_check(hr);
        wss_pending();
        return hr;
    }
    wss_pending();

    if ((i32)needed < 0) {
        hr = WSS_E_OVERFLOW;
        wss_check(hr);
        wss_pending();
        return hr;
    }
    wss_pending();

    hr = bag_vec_grow(v, needed);
    if (hr < 0) {
        wss_check(hr);
        wss_pending();
        return hr;
    }

    if (index < (i32)v->count) {
        BYTE *d = (BYTE *)v->data;
        memmove(d + (size_t)index * BAG_ENTRY_SIZE + BAG_ENTRY_SIZE,
                d + (size_t)index * BAG_ENTRY_SIZE,
                (size_t)(v->count - (u32)index) * BAG_ENTRY_SIZE);
    }

    {
        BYTE *e = (BYTE *)v->data + (size_t)index * BAG_ENTRY_SIZE;

        memset(e, 0, BAG_ENTRY_SIZE);
        if (e != NULL) {
            memset(e, 0, BAG_ENTRY_SIZE);
            *(u32 *)(e + 4) = 0;
            *(u32 *)(e + 0x10) = 0;
        }
        bag_entry_assign((BagEntry *)e, (BagEntry *)src);
    }

    v->count = v->count + 1u;
    hr = S_OK;

    wss_pending();
    return hr;
}

/* ======================================================================== */
/* 0x403d0c  bag_vec_remove(v, index)                                        */
/*                                                                          */
/* Clears the entry, closes the gap, then halves the capacity again when it  */
/* is more than twice the count (floor 0x20).                               */
/* ======================================================================== */
static void bag_vec_remove(BagVec *v, i32 index, u32 noise1, u32 noise2)
{
    BYTE *base;
    u32 count;
    u32 cap;
    u32 newcap;

    (void)noise1;
    (void)noise2;

    base = (BYTE *)v->data;
    if (base + (size_t)index * BAG_ENTRY_SIZE != NULL) {
        wss_value_clear((WssValue *)(base + (size_t)index * BAG_ENTRY_SIZE));
    }

    v->count = v->count - 1u;
    count = v->count;

    if (index < (i32)count) {
        BYTE *d = (BYTE *)v->data;
        memmove(d + (size_t)index * BAG_ENTRY_SIZE,
                d + (size_t)index * BAG_ENTRY_SIZE + BAG_ENTRY_SIZE,
                (size_t)(count - (u32)index) * BAG_ENTRY_SIZE);
    }

    /* shrink policy of 0x403d0c: floor 0x20, otherwise halve while
       capacity > 2 * count. */
    cap = v->cap;
    if ((i32)count < (i32)BAG_INITIAL_CAP + 1) {
        newcap = BAG_INITIAL_CAP;
    } else {
        u32 twice;

        if ((i32)count < 0) {
            newcap = cap;
        } else {
            twice = count << 1;
            if ((count & 0x7fffffffu) != count) {
                newcap = cap;
            } else if ((i32)cap <= (i32)twice) {
                newcap = cap;
            } else {
                newcap = cap / 2u;
            }
        }
    }

    if (newcap != v->cap) {
        HRESULT hr = bag_vec_resize(v, newcap, 0);
        if (hr < 0) {
            wss_check(hr);
        }
    }
    wss_pending();
}

/* ======================================================================== */
/* 0x403360  bag_entry_copy_block(e, out)                                    */
/*                                                                          */
/* Builds the self-contained 0x40-byte-aligned block that GetEntry returns:  */
/*   +0x00 name pointer (into the block)                                    */
/*   +0x04 vt                                                               */
/*   +0x08 payload pointer (inline data, or the external pv for vt 1/arrays) */
/*   +0x0c cb                                                               */
/*   +0x10 inline payload (vt 2/4), then the copied name string.             */
/*                                                                          */
/* For scalar types the block size includes the (cb+1)&~1 inline area; array  */
/* types (vt & 0x80000000) do not, but the name offset still uses the        */
/* rounded cb.  One LocalAlloc(LPTR) block, caller frees it with LocalFree.   */
/* ======================================================================== */
static HRESULT bag_entry_copy_block(const BagEntry *e, void **out, u32 noise)
{
    HRESULT hr = S_OK;
    u32 total = 0x10u;
    u32 nameCount;
    u32 nameBytes;
    BYTE *block = NULL;

    (void)noise;

    if ((e->vt & 0x80000000u) == 0) {
        total = (((e->cb + 1u) & 0xfffffffeu) + 0x10u);
        if (total < 0x10u) {
            hr = WSS_E_OVERFLOW;
            wss_check(hr);
            goto fail;
        }
        wss_pending();
    }

    /* --- counted name: wchar_t count at name[-1] ---------------------- */
    nameCount = (e->name != NULL) ? ((const u32 *)e->name)[-1] : 0u;
    wss_pending();

    nameBytes = 0;
    if (nameCount != 0) {
        if (nameCount + 1u < nameCount) {
            hr = WSS_E_OVERFLOW;
            wss_check(hr);
            goto fail;
        }
        nameCount = nameCount + 1u;
        wss_pending();

        nameBytes = nameCount << 1;
        if ((nameCount & 0x7fffffffu) != nameCount) {
            hr = WSS_E_OVERFLOW;
            wss_check(hr);
            goto fail;
        }
        wss_pending();
    }
    wss_pending();

    if (nameBytes + total < total) {
        hr = WSS_E_OVERFLOW;
        wss_check(hr);
        goto fail;
    }
    total += nameBytes;
    wss_pending();

    block = (BYTE *)LocalAlloc(LPTR, total);
    if (block == NULL) {
        hr = WSS_E_OUTOFMEMORY;
        wss_check(hr);
        goto fail;
    }

    *(u32 *)(block + 0x04) = e->vt;
    *(u32 *)(block + 0x0c) = e->cb;

    switch (e->vt) {
    case 1:
    case WSS_VT_ARRAY_PTR:
    case WSS_VT_ARRAY_BLOB:
        *(const void **)(block + 0x08) = e->u.pv;
        break;

    case 2:
    case 4:
        if (e->cb != 0) {
            *(void **)(block + 0x08) = block + 0x10;
            memcpy(block + 0x10, e->owned, e->cb);
        }
        break;

    default:
        hr = WSS_E_UNEXPECTED;
        wss_check(hr);
        goto fail;
    }

    if (nameBytes != 0) {
        BYTE *np = block + (((e->cb + 1u) & 0xfffffffeu) + 0x10u);
        *(BYTE **)(block) = np;
        memcpy(np, e->name, nameBytes);
    }

    *out = block;
    block = NULL;
    wss_pending();
    return hr;

fail:
    wss_check(hr);
    wss_pending();
    if (block != NULL) {
        LocalFree(block);
    }
    return hr;
}

/* ======================================================================== */
/* 0x40315c  AddRef                                                          */
/* ======================================================================== */
static u32 __stdcall bag_AddRef(WssBag *bag)
{
    return (u32)InterlockedIncrement((volatile LONG *)&bag->refs);
}

/* ======================================================================== */
/* 0x4030cc  Release                                                         */
/*                                                                          */
/* Drops the atomic refcount; on zero it also drops the module-wide object   */
/* counter, empties the entry vector and frees the object.  Returns the new  */
/* refcount.                                                                */
/* ======================================================================== */
static u32 __stdcall bag_Release(WssBag *bag)
{
    LONG refs = InterlockedDecrement((volatile LONG *)&bag->refs);

    if (refs == 0) {
        InterlockedDecrement(&g_bagObjects);
        if (bag != NULL) {
            bag_vec_resize(&bag->vec, 0, 0);
            if (bag->vec.data != NULL) {
                HeapFree(GetProcessHeap(), 0, bag->vec.data);
                bag->vec.data = NULL;
            }
            HeapFree(GetProcessHeap(), 0, bag);
        }
    }
    return (u32)refs;
}

/* ======================================================================== */
/* 0x4030b4 / 0x4030c0 / 0x403150  adjustor thunks for the second interface  */
/*                                                                          */
/* They exist because the second interface pointer is object+4 while the     */
/* refcount and the implementation live at object+0.                         */
/* ======================================================================== */
static HRESULT __stdcall bag2_QueryInterface(void *self, const GUID *riid, void **out)
{
    return bag_QueryInterface((WssBag *)((BYTE *)self - 4), riid, out);
}

static u32 __stdcall bag2_AddRef(void *self)
{
    return bag_AddRef((WssBag *)((BYTE *)self - 4));
}

static u32 __stdcall bag2_Release(void *self)
{
    return bag_Release((WssBag *)((BYTE *)self - 4));
}

/* ======================================================================== */
/* 0x403178  QueryInterface                                                  */
/*                                                                          */
/* Knows IID_IUnknown itself, everything else goes to 0x403c5c (vtable      */
/* slot +0x24).                                                             */
/* ======================================================================== */
static HRESULT __stdcall bag_QueryInterface(WssBag *bag, const GUID *riid, void **out)
{
    HRESULT hr;

    if (out == NULL) {
        hr = WSS_E_POINTER;
        wss_check(hr);
    } else {
        if (memcmp(riid, &g_iidUnknown, sizeof(GUID)) == 0) {
            bag->vtbl->AddRef(bag);
            *out = bag;
            hr = S_OK;
        } else {
            hr = bag->vtbl->QueryInterface2(bag, riid, out);
            if (hr < 0) {
                wss_check(hr);
            }
        }
    }
    wss_pending();
    return hr;
}

/* ======================================================================== */
/* 0x403c5c  QueryInterface2 (vtable +0x24)                                  */
/*                                                                          */
/* Three more iids resolve to the object itself; g_iidSecond resolves to     */
/* the second interface at object+4 (after AddRef).  Anything else sets      */
/* *out = NULL and fails with E_NOINTERFACE.                                */
/* ======================================================================== */
static HRESULT __stdcall bag_QueryInterface2(WssBag *bag, const GUID *riid, void **out)
{
    HRESULT hr = WSS_E_NOINTERFACE;

    if (out == NULL) {
        hr = WSS_E_POINTER;
        wss_check(hr);
        return hr;
    }

    if (memcmp(riid, &g_iidExtraA, sizeof(GUID)) != 0) {
        if (memcmp(riid, &g_iidSecond, sizeof(GUID)) == 0) {
            BYTE *p = (BYTE *)bag + 4;
            const BagVtbl2 *v2 = *(const BagVtbl2 **)p;

            v2->AddRef(p);
            *out = p;
            return S_OK;
        }
        if ((memcmp(riid, &g_iidExtraB, sizeof(GUID)) != 0) &&
            (memcmp(riid, &g_iidBag, sizeof(GUID)) != 0)) {
            *out = NULL;
            wss_check(hr);
            return hr;
        }
    }

    bag->vtbl->AddRef(bag);
    *out = bag;
    return S_OK;
}

/* ======================================================================== */
/* 0x4032bc  GetCount                                                        */
/* ======================================================================== */
static HRESULT __stdcall bag_GetCount(WssBag *bag, u32 *count)
{
    *count = bag->vec.count;
    wss_pending();
    return S_OK;
}

/* ======================================================================== */
/* 0x4032d4  GetValue(index, out)                                            */
/*                                                                          */
/* Returns a BORROWED view of the stored entry (no deep copy): the name and  */
/* the payload still belong to the bag.  Only the four meaningful fields of  */
/* the public WssValue are written; +0x10 (owned) is cleared here so a       */
/* caller never sees an indeterminate pointer, but such a value must still   */
/* not be passed to wss_value_clear().                                      */
/* ======================================================================== */
static HRESULT __stdcall bag_GetValue(WssBag *bag, u32 index, WssValue *out)
{
    HRESULT hr = S_OK;
    const BagEntry *e;
    const wchar_t *name;
    const void *pv;
    u32 cb;

    if (bag->vec.count <= index) {
        hr = WSS_E_INVALIDARG;
        wss_check(hr);
        wss_pending();
        return hr;
    }

    e = (const BagEntry *)((const BYTE *)bag->vec.data + (size_t)index * BAG_ENTRY_SIZE);

    name = e->name;
    if (name == NULL) {
        name = NULL;                    /* 0x403306: NULL canonicalisation */
    }

    switch (e->vt) {
    case 1:                             /* opaque pointer: no length */
        pv = e->u.pv;
        cb = 0;
        break;
    case 2:                             /* string: no length either */
        pv = e->u.pv;
        cb = 0;
        break;
    case 4:                             /* blob */
        pv = e->u.pv;
        cb = e->cb;
        break;
    case WSS_VT_ARRAY_PTR:
        pv = e->u.pv;
        cb = 0;
        break;
    case WSS_VT_ARRAY_BLOB:
        pv = e->u.pv;
        cb = e->cb;
        break;
    default:
        hr = WSS_E_UNEXPECTED;
        wss_check(hr);
        wss_pending();
        return hr;
    }

    out->name = name;
    out->vt = e->vt;
    out->u.pv = pv;
    out->cb = cb;
    out->owned = NULL;                  /* original leaves +0x10 untouched */

    wss_pending();
    return hr;
}

/* ======================================================================== */
/* 0x4034f0  GetEntry(index, out)                                            */
/*                                                                          */
/* Hands back the deep-copied block built by 0x403360 (LocalAlloc'd; the     */
/* caller owns it and frees it with LocalFree).                              */
/* ======================================================================== */
static HRESULT __stdcall bag_GetEntry(WssBag *bag, u32 index, void **out)
{
    HRESULT hr = WSS_E_INVALIDARG;
    void *block = NULL;
    void *keep = NULL;

    if (index < bag->vec.count) {
        const BYTE *base = (const BYTE *)bag->vec.data;

        hr = bag_entry_copy_block((const BagEntry *)(base + (size_t)index * BAG_ENTRY_SIZE),
                                  &block, 0);
        if (hr < 0) {
            wss_check(hr);
            keep = block;
        } else {
            keep = NULL;
            *out = block;
        }
    } else {
        wss_check(hr);
    }

    wss_pending();
    if (keep != NULL) {
        LocalFree(keep);
    }
    return hr;
}

/* ======================================================================== */
/* 0x403558  SetValue(tag, value)   (vtable +0x18)                           */
/*                                                                          */
/* `tag` is the insertion index: it must be <= count.  A read-only bag       */
/* refuses with E_ACCESSDENIED.                                             */
/* ======================================================================== */
static HRESULT __stdcall bag_SetValue(WssBag *bag, u32 tag, const WssValue *value)
{
    BagEntry val;
    HRESULT  hr;

    memset(&val, 0, sizeof(val));
    val.name = NULL;
    val.owned = NULL;

    if (bag->vec.count < tag) {
        hr = WSS_E_INVALIDARG;
        wss_check(hr);
    } else if (bag->locked != 0) {
        hr = WSS_E_ACCESSDENIED;
        wss_check(hr);
    } else {
        hr = wss_value_copy((WssValue *)&val, value);
        if (hr >= 0) {
            hr = bag_vec_insert(&bag->vec, (i32)tag, &val, 0);
            if (hr < 0) {
                wss_check(hr);
            }
        } else {
            wss_check(hr);
        }
    }

    wss_pending();
    wss_value_clear((WssValue *)&val);
    return hr;
}

/* ======================================================================== */
/* 0x403770  Append(value)   (vtable +0x1c)                                  */
/* ======================================================================== */
static HRESULT __stdcall bag_Append(WssBag *bag, const WssValue *value)
{
    BagEntry val;
    HRESULT  hr;

    memset(&val, 0, sizeof(val));
    val.name = NULL;
    val.owned = NULL;

    if (bag->locked != 0) {
        hr = WSS_E_ACCESSDENIED;
        wss_check(hr);
    } else {
        hr = wss_value_copy((WssValue *)&val, value);
        if (hr >= 0) {
            hr = bag_vec_insert(&bag->vec, (i32)bag->vec.count, &val, 0);
        }
        if (hr < 0) {
            wss_check(hr);
        }
    }

    wss_pending();
    wss_value_clear((WssValue *)&val);
    return hr;
}

/* ======================================================================== */
/* 0x403be4  SetValueAt(index, value)   (vtable +0x20)                       */
/*                                                                          */
/* Replaces an existing entry in place (no shifting, count unchanged).       */
/* ======================================================================== */
static HRESULT __stdcall bag_SetValueAt(WssBag *bag, u32 index, const WssValue *value)
{
    BagEntry val;
    HRESULT  hr;

    memset(&val, 0, sizeof(val));
    val.name = NULL;
    val.owned = NULL;

    if (bag->vec.count <= index) {
        hr = WSS_E_INVALIDARG;
        wss_check(hr);
    } else if (bag->locked != 0) {
        hr = WSS_E_ACCESSDENIED;
        wss_check(hr);
    } else {
        hr = wss_value_copy((WssValue *)&val, value);
        if (hr >= 0) {
            BYTE *e = (BYTE *)bag->vec.data + (size_t)index * BAG_ENTRY_SIZE;
            bag_entry_assign((BagEntry *)e, &val);
        } else {
            wss_check(hr);
        }
    }

    wss_pending();
    wss_value_clear((WssValue *)&val);
    return hr;
}

/* ======================================================================== */
/* 0x403cf4  Lock   (second vtable +0x0c): make the bag read-only            */
/* ======================================================================== */
static HRESULT __stdcall bag2_Lock(void *self)
{
    WssBag *bag = (WssBag *)self;

    bag->locked = 1;
    wss_pending();
    return S_OK;
}

/* ======================================================================== */
/* 0x403e08  RemoveAt(index)   (second vtable +0x10)                         */
/* ======================================================================== */
static HRESULT __stdcall bag2_RemoveAt(void *self, u32 index)
{
    WssBag *bag = (WssBag *)self;
    HRESULT hr = S_OK;

    if (bag->locked != 0) {
        hr = WSS_E_ACCESSDENIED;
        wss_check(hr);
    } else if (index < bag->vec.count) {
        bag_vec_remove(&bag->vec, (i32)index, 0, 0);
    } else {
        hr = WSS_E_INVALIDARG;
        wss_check(hr);
    }

    wss_pending();
    return hr;
}

/* ======================================================================== */
/* 0x403e44  Clear   (second vtable +0x14): empty the bag when writable      */
/* ======================================================================== */
static HRESULT __stdcall bag2_Clear(void *self)
{
    WssBag *bag = (WssBag *)self;
    HRESULT hr = S_OK;

    if (bag->locked == 0) {
        bag_vec_resize(&bag->vec, 0, 0);
    } else {
        hr = WSS_E_ACCESSDENIED;
        wss_check(hr);
    }

    wss_pending();
    return hr;
}

/* ======================================================================== */
/* Public API                                                                */
/* ======================================================================== */

/* ------------------------------------------------------------------------
 * 0x4031d0  wss_bag_create(out)
 *
 * HeapAlloc(0x1c), zero the first 0x18 bytes, install both vtables, set the
 * refcount to 1, bump the module object counter, then QueryInterface for
 * g_iidBag so the caller gets a +4-adjusted interface pointer if the object
 * is asked for one at all; the creation reference is released before return.
 * --------------------------------------------------------------------- */
HRESULT wss_bag_create(WssBag **out)
{
    WssBag *bag;
    WssBag *keep = NULL;
    HRESULT hr = S_OK;

    bag = (WssBag *)HeapAlloc(GetProcessHeap(), 0, sizeof(struct WssBag));
    if (bag == NULL) {
        bag = NULL;
    } else {
        memset(bag, 0, 0x18);
        bag->vec.cap = 0;
        bag->vec.count = 0;
        bag->vec.data = NULL;
        bag->vtbl = &g_bagVtbl;
        bag->vtbl2 = &g_bagVtbl2;
        bag->refs = 1;
        InterlockedIncrement(&g_bagObjects);
    }

    if (bag == NULL) {
        hr = WSS_E_OUTOFMEMORY;
        wss_check(hr);
    } else {
        keep = bag;
        bag = NULL;
    }
    wss_pending();
    if (bag != NULL) {
        bag->vtbl->Release(bag);
    }

    if (hr >= 0) {
        hr = keep->vtbl->QueryInterface(keep, &g_iidBag, (void **)out);
        if (hr < 0) {
            wss_check(hr);
        }
    }
    wss_pending();

    if (keep != NULL) {
        keep->vtbl->Release(keep);
    }
    return hr;
}

/* 0x4032bc */
HRESULT wss_bag_get_count(WssBag *bag, u32 *count)
{
    return bag_GetCount(bag, count);
}

/* 0x4032d4 */
HRESULT wss_bag_get_value(WssBag *bag, u32 index, WssValue *out)
{
    return bag_GetValue(bag, index, out);
}

/* 0x403558 */
HRESULT wss_bag_set_value(WssBag *bag, u32 tag, const WssValue *value)
{
    return bag_SetValue(bag, tag, value);
}

/* 0x4030cc */
void wss_bag_release(WssBag *bag)
{
    if (bag != NULL) {
        bag->vtbl->Release(bag);
    }
}

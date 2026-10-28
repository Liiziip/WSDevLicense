/*
 * serialize.c - the property-bag wire serializer of arm.exe
 *               ("Utility for Windows Store Developer Licensing", WinRT 2011).
 *
 * This is a straight transcription of the ARM/Thumb-2 code at
 *   0x4258  writer factory (HeapAlloc 0x24, vtable 0x401ba8, refcount at +0x20)
 *   0x423c  AddRef            (vtable +0x04)
 *   0x41c0  Release           (vtable +0x08)
 *   0x4338  SetBuffer         (vtable +0x0c)
 *   0x4414  GetBuffer         (vtable +0x10)
 *   0x4448  GetPosition       (vtable +0x14)
 *   0x4460  ReadRecord        (vtable +0x18)
 *   0x48ac  CopyRecord        (vtable +0x1c)
 *   0x4b4c  Add               (vtable +0x20)
 *   0x4e70  QueryInterface #2 (vtable +0x24)
 *   0x4448/0x4488           buffer-range accessor
 *   0x4968, 0x49ec, 0x4528  string length, buffer growth, record parser
 *   0x4ebc  checked writer factory
 *   0x4f20  wss_bag_serialize      (bag -> bytes)
 *   0x3ff0 + 0x40dc  wss_bag_deserialize (bytes -> bag)
 *   0x5030  bag-holder -> bytes    (used by the RPC layer)
 *
 * ---------------------------------------------------------------------------
 * WIRE FORMAT  (little endian, all offsets/fields 32 bit)
 * ---------------------------------------------------------------------------
 * A serialized bag is the concatenation of records, one per property value,
 * in bag order.  There is no file header, no record count and no terminator:
 * the buffer length is the only end marker.
 *
 *   record:
 *     +0x00  u32  checksum          CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF,
 *                                   final xor 0xFFFFFFFF, table at 0x401bd0)
 *                                   over the 16 header bytes *with this field
 *                                   taken as zero*, then the name bytes, then
 *                                   the data bytes.  Padding is NOT covered.
 *     +0x04  u32  vt                WSS_VT_* of the value
 *     +0x08  u32  nameLen           bytes, including the NUL wchar (0 = no name)
 *     +0x0c  u32  dataLen           bytes, including the NUL wchar for strings
 *     +0x10  ...  name              nameLen bytes, zero padded to 8
 *     ...         data              dataLen bytes, zero padded to 8
 *
 * Padding: every name block and every data block is rounded up to a multiple
 * of 8 with zero bytes, so record length = 16 + align8(nameLen) +
 * align8(dataLen) and every record starts 8-byte aligned.  Note that a 20-byte
 * WssValue is not what goes on the wire; only {vt, name, data} do.
 *
 *   vt = 1          data = the 4-byte pointer value stored in WssValue.u
 *   vt = 2          data = the NUL terminated UTF-16 string, dataLen = 2*n+2
 *   0x80000002      array of strings, serialized exactly like vt == 2
 *   vt = 4          data = WssValue.u.pv, dataLen = WssValue.cb
 *   0x80000004      array of blobs, serialized exactly like vt == 4
 *
 * The parser (0x4528) only accepts those five types.  Two quirks of the
 * original are preserved verbatim below: for vt == 1 the parser reports
 * dataLen/cb == 0 (although it consumed 4 bytes and checksummed them), and the
 * bag deserializer (0x3ff0) rejects any record whose vt is not exactly 4.
 *
 * Everything here is plain C (MSVC 2019, /TC).  The WssWriter layout is the
 * original's 0x24 bytes; the bag API is only called, never defined.
 */

#include "wss.h"
#include <string.h>

/* ------------------------------------------------------------------------- */
/* HRESULTs: the constants the original loads from .rdata                    */
/* ------------------------------------------------------------------------- */
#define WSS_HR_OUTOFMEMORY   ((HRESULT)0x8007000E)  /* 0x404324, 0x404960, ... */
#define WSS_HR_INVALIDDATA   ((HRESULT)0x8007000D)  /* 0x404408, 0x40451C, ... */
#define WSS_HR_ARITH         ((HRESULT)0x80070216)  /* HRESULT_FROM_WIN32(534) */
#define WSS_HR_NOMOREITEMS   ((HRESULT)0x80070103)  /* HRESULT_FROM_WIN32(259) */
#define WSS_HR_CRC           ((HRESULT)0x80070017)  /* HRESULT_FROM_WIN32(23)  */
#define WSS_HR_INSUFFICIENT  ((HRESULT)0x8007007A)  /* HRESULT_FROM_WIN32(122) */
#define WSS_HR_ACCESSDENIED  ((HRESULT)0x80070005)
#define WSS_HR_INVALIDARG    ((HRESULT)0x80070057)
#define WSS_HR_POINTER       ((HRESULT)0x80004003)
#define WSS_HR_NOINTERFACE   ((HRESULT)0x80004002)

/* The two interface ids the vtable methods compare against. */
static const u8 g_iid_main[16] = {                    /* 0x4011B0, used by 0x403178 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46
};
static const u8 g_iid_writer[16] = {                  /* 0x4017C8, used by 0x4E70 */
    0xA2, 0x37, 0x21, 0xDA, 0xA9, 0xDE, 0x5C, 0x4C,
    0xA7, 0xCE, 0x7E, 0xE1, 0x43, 0x58, 0x90, 0xD6
};

/* ------------------------------------------------------------------------- */
/* CRC-32: the table that lives at 0x401BD0 in the original .text            */
/* ------------------------------------------------------------------------- */
static const u32 g_crcTable[256] = {
    0x00000000u, 0x04C11DB7u, 0x09823B6Eu, 0x0D4326D9u,
    0x130476DCu, 0x17C56B6Bu, 0x1A864DB2u, 0x1E475005u,
    0x2608EDB8u, 0x22C9F00Fu, 0x2F8AD6D6u, 0x2B4BCB61u,
    0x350C9B64u, 0x31CD86D3u, 0x3C8EA00Au, 0x384FBDBDu,
    0x4C11DB70u, 0x48D0C6C7u, 0x4593E01Eu, 0x4152FDA9u,
    0x5F15ADACu, 0x5BD4B01Bu, 0x569796C2u, 0x52568B75u,
    0x6A1936C8u, 0x6ED82B7Fu, 0x639B0DA6u, 0x675A1011u,
    0x791D4014u, 0x7DDC5DA3u, 0x709F7B7Au, 0x745E66CDu,
    0x9823B6E0u, 0x9CE2AB57u, 0x91A18D8Eu, 0x95609039u,
    0x8B27C03Cu, 0x8FE6DD8Bu, 0x82A5FB52u, 0x8664E6E5u,
    0xBE2B5B58u, 0xBAEA46EFu, 0xB7A96036u, 0xB3687D81u,
    0xAD2F2D84u, 0xA9EE3033u, 0xA4AD16EAu, 0xA06C0B5Du,
    0xD4326D90u, 0xD0F37027u, 0xDDB056FEu, 0xD9714B49u,
    0xC7361B4Cu, 0xC3F706FBu, 0xCEB42022u, 0xCA753D95u,
    0xF23A8028u, 0xF6FB9D9Fu, 0xFBB8BB46u, 0xFF79A6F1u,
    0xE13EF6F4u, 0xE5FFEB43u, 0xE8BCCD9Au, 0xEC7DD02Du,
    0x34867077u, 0x30476DC0u, 0x3D044B19u, 0x39C556AEu,
    0x278206ABu, 0x23431B1Cu, 0x2E003DC5u, 0x2AC12072u,
    0x128E9DCFu, 0x164F8078u, 0x1B0CA6A1u, 0x1FCDBB16u,
    0x018AEB13u, 0x054BF6A4u, 0x0808D07Du, 0x0CC9CDCAu,
    0x7897AB07u, 0x7C56B6B0u, 0x71159069u, 0x75D48DDEu,
    0x6B93DDDBu, 0x6F52C06Cu, 0x6211E6B5u, 0x66D0FB02u,
    0x5E9F46BFu, 0x5A5E5B08u, 0x571D7DD1u, 0x53DC6066u,
    0x4D9B3063u, 0x495A2DD4u, 0x44190B0Du, 0x40D816BAu,
    0xACA5C697u, 0xA864DB20u, 0xA527FDF9u, 0xA1E6E04Eu,
    0xBFA1B04Bu, 0xBB60ADFCu, 0xB6238B25u, 0xB2E29692u,
    0x8AAD2B2Fu, 0x8E6C3698u, 0x832F1041u, 0x87EE0DF6u,
    0x99A95DF3u, 0x9D684044u, 0x902B669Du, 0x94EA7B2Au,
    0xE0B41DE7u, 0xE4750050u, 0xE9362689u, 0xEDF73B3Eu,
    0xF3B06B3Bu, 0xF771768Cu, 0xFA325055u, 0xFEF34DE2u,
    0xC6BCF05Fu, 0xC27DEDE8u, 0xCF3ECB31u, 0xCBFFD686u,
    0xD5B88683u, 0xD1799B34u, 0xDC3ABDEDu, 0xD8FBA05Au,
    0x690CE0EEu, 0x6DCDFD59u, 0x608EDB80u, 0x644FC637u,
    0x7A089632u, 0x7EC98B85u, 0x738AAD5Cu, 0x774BB0EBu,
    0x4F040D56u, 0x4BC510E1u, 0x46863638u, 0x42472B8Fu,
    0x5C007B8Au, 0x58C1663Du, 0x558240E4u, 0x51435D53u,
    0x251D3B9Eu, 0x21DC2629u, 0x2C9F00F0u, 0x285E1D47u,
    0x36194D42u, 0x32D850F5u, 0x3F9B762Cu, 0x3B5A6B9Bu,
    0x0315D626u, 0x07D4CB91u, 0x0A97ED48u, 0x0E56F0FFu,
    0x1011A0FAu, 0x14D0BD4Du, 0x19939B94u, 0x1D528623u,
    0xF12F560Eu, 0xF5EE4BB9u, 0xF8AD6D60u, 0xFC6C70D7u,
    0xE22B20D2u, 0xE6EA3D65u, 0xEBA91BBCu, 0xEF68060Bu,
    0xD727BBB6u, 0xD3E6A601u, 0xDEA580D8u, 0xDA649D6Fu,
    0xC423CD6Au, 0xC0E2D0DDu, 0xCDA1F604u, 0xC960EBB3u,
    0xBD3E8D7Eu, 0xB9FF90C9u, 0xB4BCB610u, 0xB07DABA7u,
    0xAE3AFBA2u, 0xAAFBE615u, 0xA7B8C0CCu, 0xA379DD7Bu,
    0x9B3660C6u, 0x9FF77D71u, 0x92B45BA8u, 0x9675461Fu,
    0x8832161Au, 0x8CF30BADu, 0x81B02D74u, 0x857130C3u,
    0x5D8A9099u, 0x594B8D2Eu, 0x5408ABF7u, 0x50C9B640u,
    0x4E8EE645u, 0x4A4FFBF2u, 0x470CDD2Bu, 0x43CDC09Cu,
    0x7B827D21u, 0x7F436096u, 0x7200464Fu, 0x76C15BF8u,
    0x68860BFDu, 0x6C47164Au, 0x61043093u, 0x65C52D24u,
    0x119B4BE9u, 0x155A565Eu, 0x18197087u, 0x1CD86D30u,
    0x029F3D35u, 0x065E2082u, 0x0B1D065Bu, 0x0FDC1BECu,
    0x3793A651u, 0x3352BBE6u, 0x3E119D3Fu, 0x3AD08088u,
    0x2497D08Du, 0x2056CD3Au, 0x2D15EBE3u, 0x29D4F654u,
    0xC5A92679u, 0xC1683BCEu, 0xCC2B1D17u, 0xC8EA00A0u,
    0xD6AD50A5u, 0xD26C4D12u, 0xDF2F6BCBu, 0xDBEE767Cu,
    0xE3A1CBC1u, 0xE760D676u, 0xEA23F0AFu, 0xEEE2ED18u,
    0xF0A5BD1Du, 0xF464A0AAu, 0xF9278673u, 0xFDE69BC4u,
    0x89B8FD09u, 0x8D79E0BEu, 0x803AC667u, 0x84FBDBD0u,
    0x9ABC8BD5u, 0x9E7D9662u, 0x933EB0BBu, 0x97FFAD0Cu,
    0xAFB010B1u, 0xAB710D06u, 0xA6322BDFu, 0xA2F33668u,
    0xBCB4666Du, 0xB8757BDAu, 0xB5365D03u, 0xB1F740B4u
};

/* One CRC step sequence, exactly as 0x4b4c / 0x4528 run it:
 *   crc = table[((crc >> 24) ^ byte) & 0xff] ^ (crc << 8)
 * The routines keep the running value un-complemented and toggle with ~ at
 * the boundaries, which the callers below reproduce. */
static u32 wss_crc32(u32 crc, const void *pv, u32 cb)
{
    const u8 *p = (const u8 *)pv;

    while (cb != 0) {
        crc = g_crcTable[(crc >> 24) ^ *p] ^ (crc << 8);
        p++;
        cb--;
    }
    return crc;
}

/* ------------------------------------------------------------------------- */
/* small helpers                                                             */
/* ------------------------------------------------------------------------- */
static u32 wss_align8(u32 n)
{
    return (n + 7u) & ~7u;
}

static u32 wss_rd32(const void *p)
{
    u32 v;
    memcpy(&v, p, sizeof v);
    return v;
}

static u16 wss_rd16(const void *p)
{
    u16 v;
    memcpy(&v, p, sizeof v);
    return v;
}

static void wss_wr32(void *p, u32 v)
{
    memcpy(p, &v, sizeof v);
}

/* eight zero bytes: the padding source used by Add (locals at sp+8..sp+0xf) */
static const u8 g_zeros[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

/* ------------------------------------------------------------------------- */
/* objects                                                                   */
/* ------------------------------------------------------------------------- */
typedef struct WssBuf {
    u8 *pv;                  /* +0x00 */
    u32 len;                 /* +0x04: bytes in use                          */
    u32 cap;                 /* +0x08: bytes allocated, not counting the NUL */
} WssBuf;

typedef struct WssWriter WssWriter;

struct WssWriter {
    const struct WssWriterVt *vt;   /* +0x00: 0x401BA8 in the original        */
    u32    fExt;                    /* +0x04: !=0 -> pExt/cbExt are the buffer */
    const u8 *pExt;                 /* +0x08                                  */
    u32    cbExt;                   /* +0x0c                                  */
    WssBuf buf;                     /* +0x10: pv, +0x14: len, +0x18: cap      */
    u32    pos;                     /* +0x1c: parse cursor (0x4448 reports it)*/
    LONG   refs;                    /* +0x20: reference count                 */
};

/* The 16-byte record the parser produces and the deserializer consumes. */
typedef struct WssRecord {
    const wchar_t *name;            /* +0x00: points into the wire buffer     */
    u32            vt;              /* +0x04                                  */
    const void    *data;            /* +0x08                                  */
    u32            cb;              /* +0x0c                                  */
} WssRecord;

struct WssWriterVt {
    HRESULT (*qi)(WssWriter *, const void *, void **);          /* +0x00 */
    LONG    (*addref)(WssWriter *);                             /* +0x04 */
    LONG    (*release)(WssWriter *);                            /* +0x08 */
    HRESULT (*set_buffer)(WssWriter *, const void *, u32, int); /* +0x0c */
    HRESULT (*get_buffer)(WssWriter *, void **, u32 *);         /* +0x10 */
    HRESULT (*get_pos)(WssWriter *, u32 *);                     /* +0x14 */
    HRESULT (*read_record)(WssWriter *, WssRecord *);           /* +0x18 */
    HRESULT (*copy_record)(WssWriter *, void **);               /* +0x1c */
    HRESULT (*add)(WssWriter *, const WssValue *);              /* +0x20 */
    HRESULT (*qi2)(WssWriter *, const void *, void **);         /* +0x24 */
};

static HRESULT wss_writer_qi(WssWriter *w, const void *riid, void **ppv);
static LONG    wss_writer_addref(WssWriter *w);
static LONG    wss_writer_release_obj(WssWriter *w);
static HRESULT wss_writer_set_buffer(WssWriter *w, const void *pv, u32 cb, int copy);
HRESULT wss_writer_get_buffer(WssWriter *w, void **ppv, u32 *pcb);
static HRESULT wss_writer_get_pos(WssWriter *w, u32 *pcb);
static HRESULT wss_writer_read_record(WssWriter *w, WssRecord *rec);
static HRESULT wss_writer_copy_record(WssWriter *w, void **ppv);
HRESULT wss_writer_add(WssWriter *w, const WssValue *value);
static HRESULT wss_writer_qi2(WssWriter *w, const void *riid, void **ppv);

/* vtable 0x401BA8; only the +0x00 field of the object refers to it, every
 * call site below goes through the static functions directly. */
static const struct WssWriterVt g_writerVt = {
    wss_writer_qi,
    wss_writer_addref,
    wss_writer_release_obj,
    wss_writer_set_buffer,
    wss_writer_get_buffer,
    wss_writer_get_pos,
    wss_writer_read_record,
    wss_writer_copy_record,
    wss_writer_add,
    wss_writer_qi2
};

/* ------------------------------------------------------------------------- */
/* buffer helpers                                                            */
/* ------------------------------------------------------------------------- */

/* 0x4968: byte length of a NUL terminated UTF-16 string, NUL included.
 * Returns E_INVALIDARG for NULL, i.e. callers always pre-check for NULL. */
static HRESULT wss_str_bytes(const wchar_t *s, u32 *pcb)
{
    u32 n = 0;
    u32 cb;

    if (s == NULL) {
        wss_check(WSS_HR_INVALIDARG);
        return WSS_HR_INVALIDARG;
    }
    while (n < 0x3FFFFFFFu && s[n] != 0)
        n++;
    if (n == 0x3FFFFFFFu) {
        wss_check(WSS_HR_INVALIDARG);
        return WSS_HR_INVALIDARG;
    }
    cb = n * 2u;
    if (cb + 2u < cb) {
        wss_check(WSS_HR_ARITH);
        return WSS_HR_ARITH;
    }
    *pcb = cb + 2u;
    return S_OK;
}

/* 0x49ec: make room for b->len + add bytes in the owned buffer.
 * Growth policy is the original's: need = len + add, round up by 0x400, cap
 * at 0x2000000, always allocate one extra byte for the trailing NUL. */
static HRESULT wss_buf_reserve(WssBuf *b, u32 add)
{
    u32 need;
    u32 want;
    u32 allocSize;
    u8 *p;

    need = b->len + add;
    if (need < add) {
        wss_check(WSS_HR_ARITH);
        return WSS_HR_ARITH;
    }
    wss_pending();

    if (need <= b->cap && b->pv != NULL)
        return S_OK;

    need = b->len + add;
    if (need < b->len) {
        wss_check(WSS_HR_ARITH);
        return WSS_HR_ARITH;
    }
    wss_pending();

    want = need + 0x400u;
    if (want < need) {
        wss_check(WSS_HR_ARITH);
        return WSS_HR_ARITH;
    }
    need = want;
    wss_pending();

    if (need >= 0x2000000u) {
        wss_check(WSS_HR_INSUFFICIENT);
        return WSS_HR_INSUFFICIENT;
    }

    allocSize = need + 1u;
    if (allocSize < need) {
        wss_check(WSS_HR_ARITH);
        return WSS_HR_ARITH;
    }
    wss_pending();

    p = (u8 *)HeapAlloc(GetProcessHeap(), 0, allocSize);
    if (p == NULL) {
        wss_check(WSS_HR_OUTOFMEMORY);
        return WSS_HR_OUTOFMEMORY;
    }
    if (b->pv != NULL && b->len != 0)
        memcpy(p, b->pv, b->len);
    p[b->len] = 0;
    b->cap = need;
    if (b->pv != NULL) {
        HeapFree(GetProcessHeap(), 0, b->pv);
        b->pv = NULL;
    }
    b->pv = p;
    wss_pending();
    return S_OK;
}

/* The append idiom repeated by 0x4338 and 0x4b4c: reserve, copy, bump len,
 * re-terminate.  On a reserve failure the copy is skipped, as in the original. */
static HRESULT wss_buf_append(WssWriter *w, const void *pv, u32 cb)
{
    HRESULT hr;

    hr = wss_buf_reserve(&w->buf, cb);
    if (hr < 0) {
        wss_check(hr);
        return hr;
    }
    if (cb != 0)
        memcpy(w->buf.pv + w->buf.len, pv, cb);
    w->buf.len += cb;
    w->buf.pv[w->buf.len] = 0;
    return S_OK;
}

static const u8 *wss_writer_base(const WssWriter *w)
{
    if (w->fExt != 0)
        return w->pExt;
    return w->buf.pv;
}

static u32 wss_writer_len(const WssWriter *w)
{
    if (w->fExt != 0)
        return w->cbExt;
    return w->buf.len;
}

/* 0x4488: bounds check [start, start+size) against the current length and hand
 * back base+start plus the new "end" offset. */
static HRESULT wss_writer_range(WssWriter *w, u32 start, u32 size,
                                void **ppv, u32 *pend)
{
    u32 end;
    HRESULT hr;

    if (start == wss_writer_len(w) && size != 0) {
        wss_check(WSS_HR_NOMOREITEMS);
        return WSS_HR_NOMOREITEMS;
    }
    wss_pending();

    end = start + size;
    if (end < start) {
        hr = WSS_HR_ARITH;
        end = start;
    } else {
        hr = S_OK;
    }
    wss_pending();
    if (hr < 0) {
        wss_check(hr);
        return hr;
    }
    if (end > wss_writer_len(w)) {
        wss_check(WSS_HR_INVALIDDATA);
        return WSS_HR_INVALIDDATA;
    }
    if (ppv != NULL)
        *ppv = (void *)((ULONG_PTR)wss_writer_base(w) + start);
    if (pend != NULL)
        *pend = end;
    wss_pending();
    return S_OK;
}

/* ------------------------------------------------------------------------- */
/* record parser - 0x4528                                                    */
/* ------------------------------------------------------------------------- */
/*
 * Reads the record at w->pos, verifies its checksum, leaves w->pos at the next
 * record and fills *rec with {name, vt, data, cb}.  Returns
 * 0x80070103 when the cursor sits exactly on the end of the buffer.
 *
 * pnameSz (0x4528's 3rd arg) receives align8(header name length); precSz (the
 * 4th) receives the whole record size, which for vt == 1 excludes the four
 * data bytes - that is what the original computes.
 */
static HRESULT wss_writer_parse(WssWriter *w, WssRecord *rec,
                                u32 *pnameSz, u32 *precSz)
{
    u32 start = w->pos;
    u32 total = wss_writer_len(w);
    u32 end;
    u32 namesz;
    u32 datalen;
    u32 recsz;
    u32 crc;
    u32 st;
    u32 zero = 0;
    const u8 *base;
    const u8 *hdr;
    const u8 *name = NULL;
    const u8 *data = NULL;
    HRESULT hr;

    rec->name = NULL;
    rec->vt = 0;
    rec->data = NULL;
    rec->cb = 0;
    if (pnameSz != NULL)
        *pnameSz = 0;
    if (precSz != NULL)
        *precSz = 0;

    if (start == total) {
        wss_check(WSS_HR_NOMOREITEMS);
        return WSS_HR_NOMOREITEMS;
    }
    wss_pending();

    /* header must fit */
    if (start + 0x10u < start) {
        wss_check(WSS_HR_ARITH);
        return WSS_HR_ARITH;
    }
    end = start + 0x10u;
    if (end > total) {
        wss_check(WSS_HR_INVALIDDATA);
        return WSS_HR_INVALIDDATA;
    }
    base = wss_writer_base(w);
    hdr = base + start;

    rec->vt = wss_rd32(hdr + 4);
    namesz = wss_rd32(hdr + 8);
    datalen = wss_rd32(hdr + 12);

    /* checksum: the stored dword is hashed as zero */
    st = wss_crc32(0xFFFFFFFFu, &zero, 4);
    st = wss_crc32(st, hdr + 4, 12);
    crc = ~st;

    /* name block, record+0x10 for align8(nameLen) bytes */
    hr = wss_writer_range(w, start + 0x10u, wss_align8(namesz),
                          (void **)&name, &end);
    if (hr < 0)
        return hr;

    recsz = 0x10u + (end - (start + 0x10u));
    if (pnameSz != NULL)
        *pnameSz = end - (start + 0x10u);
    if (recsz < 0x10u) {
        wss_check(WSS_HR_ARITH);
        return WSS_HR_ARITH;
    }

    if (end != start + 0x10u) {
        u32 off = namesz & ~1u;

        /* the string must end in a NUL wchar */
        if (off < 2u || wss_rd16(name + (off - 2u)) != 0) {
            wss_check(WSS_HR_INVALIDDATA);
            return WSS_HR_INVALIDDATA;
        }
        crc = ~wss_crc32(~crc, name, namesz);
    } else {
        name = NULL;
    }
    rec->name = (const wchar_t *)name;

    if (rec->vt == WSS_VT_PTR) {
        /* 0x4046ce: the four bytes at the data offset are the value itself */
        u32 dstart = end;
        u32 dend;
        u32 val;

        total = wss_writer_len(w);
        if (dstart == total) {
            wss_check(WSS_HR_NOMOREITEMS);
            return WSS_HR_NOMOREITEMS;
        }
        wss_pending();

        if (dstart + 4u < dstart) {
            wss_check(WSS_HR_ARITH);
            return WSS_HR_ARITH;
        }
        dend = dstart + 4u;
        if (dend > total) {
            wss_check(WSS_HR_INVALIDDATA);
            return WSS_HR_INVALIDDATA;
        }
        val = wss_rd32(base + dstart);
        crc = ~wss_crc32(~crc, &val, 4);

        data = (const void *)(ULONG_PTR)val;
        datalen = 0;        /* quirk: the original reports cb == 0 here */
        end = dend;
    } else if (rec->vt == WSS_VT_STRING || rec->vt == WSS_VT_BLOB ||
               rec->vt == WSS_VT_ARRAY_PTR || rec->vt == WSS_VT_ARRAY_BLOB) {
        datalen = wss_rd32(hdr + 12);

        if (recsz + datalen < recsz) {
            wss_check(WSS_HR_ARITH);
            return WSS_HR_ARITH;
        }
        recsz += datalen;

        if (datalen == 0) {
            data = NULL;
        } else {
            hr = wss_writer_range(w, end, datalen, (void **)&data, &end);
            if (hr < 0)
                return hr;
            crc = ~wss_crc32(~crc, data, datalen);
        }

        if ((rec->vt & 2u) != 0u && datalen != 0) {
            u32 off = datalen & ~1u;

            if (off < 2u || wss_rd16(data + (off - 2u)) != 0) {
                wss_check(WSS_HR_INVALIDDATA);
                return WSS_HR_INVALIDDATA;
            }
        }

        {
            u32 pad = wss_align8(datalen) - datalen;

            if (recsz + pad < recsz) {
                wss_check(WSS_HR_ARITH);
                return WSS_HR_ARITH;
            }
            recsz += pad;
            if (pad != 0) {
                hr = wss_writer_range(w, end, pad, NULL, &end);
                if (hr < 0)
                    return hr;
            }
        }
        rec->cb = datalen;
    } else {
        wss_check(WSS_HR_INVALIDDATA);
        return WSS_HR_INVALIDDATA;
    }

    if (crc != wss_rd32(base + start)) {
        wss_check(WSS_HR_CRC);
        return WSS_HR_CRC;
    }

    w->pos = end;
    rec->data = data;
    rec->cb = datalen;      /* vt == 1 keeps the 0 set above */
    if (precSz != NULL)
        *precSz = recsz;
    wss_pending();
    return S_OK;
}

/* 0x4460, vtable +0x18: "give me the next record" */
static HRESULT wss_writer_read_record(WssWriter *w, WssRecord *rec)
{
    HRESULT hr;

    hr = wss_writer_parse(w, rec, NULL, NULL);
    if (hr < 0)
        wss_check(hr);
    wss_pending();
    return hr;
}

/* 0x48ac, vtable +0x1c: parse the next record and return a self contained
 * LocalAlloc'ed copy laid out as {name, vt, data, cb} followed by the name and
 * data bytes (so the result survives the wire buffer). */
static HRESULT wss_writer_copy_record(WssWriter *w, void **ppv)
{
    WssRecord rec;
    u32 namesz = 0;
    u32 total = 0;
    u32 *p;
    u8 *dst;
    HRESULT hr;

    hr = wss_writer_parse(w, &rec, &namesz, &total);
    if (hr < 0) {
        wss_check(hr);
        wss_pending();
        return hr;
    }

    p = (u32 *)LocalAlloc(LPTR, total);
    if (p == NULL) {
        wss_check(WSS_HR_OUTOFMEMORY);
        wss_pending();
        return WSS_HR_OUTOFMEMORY;
    }

    dst = (u8 *)(p + 4);
    p[0] = (u32)(ULONG_PTR)rec.name;
    p[1] = rec.vt;
    p[2] = (u32)(ULONG_PTR)rec.data;
    p[3] = rec.cb;

    if (namesz == 0) {
        p[0] = 0;
    } else {
        memcpy(dst, rec.name, namesz);
        p[0] = (u32)(ULONG_PTR)dst;
        dst += namesz;
    }
    if (rec.cb != 0) {
        memcpy(dst, rec.data, rec.cb);
        p[2] = (u32)(ULONG_PTR)dst;
        if (rec.vt == WSS_VT_STRING)        /* strings carry no length */
            p[3] = 0;
    }

    *ppv = p;
    wss_pending();
    return S_OK;
}

/* ------------------------------------------------------------------------- */
/* writer methods                                                            */
/* ------------------------------------------------------------------------- */

/* 0x4338, vtable +0x0c: install a buffer.  copy == 0 wraps the caller's bytes
 * (they must be 8-byte aligned), copy != 0 duplicates them into the owned
 * buffer.  Every path resets the parse cursor. */
static HRESULT wss_writer_set_buffer(WssWriter *w, const void *pv, u32 cb, int copy)
{
    HRESULT hr;

    if ((cb & 7u) != 0) {
        wss_check(WSS_HR_INVALIDDATA);
        wss_pending();
        return WSS_HR_INVALIDDATA;
    }

    w->fExt = 0;
    w->pExt = NULL;
    w->cbExt = 0;
    if (w->buf.pv != NULL) {
        HeapFree(GetProcessHeap(), 0, w->buf.pv);
        w->buf.pv = NULL;
    }
    w->buf.cap = 0;
    w->buf.len = 0;
    w->pos = 0;

    if (pv != NULL) {
        if (copy == 0) {
            if (((ULONG_PTR)pv & 7u) != 0) {
                wss_check(WSS_HR_INVALIDDATA);
                wss_pending();
                return WSS_HR_INVALIDDATA;
            }
            w->pExt = (const u8 *)pv;
            w->cbExt = cb;
        } else {
            hr = wss_buf_append(w, pv, cb);
            if (hr < 0)
                wss_check(hr);
            wss_pending();
            if (hr < 0)
                return hr;
        }
    }

    w->fExt = (copy == 0) ? 1u : 0u;
    wss_pending();
    return S_OK;
}

/* 0x4414, vtable +0x10.  Declared in wss.h; returns a pointer into the
 * writer's own storage, so the caller must copy it before releasing. */
HRESULT wss_writer_get_buffer(WssWriter *w, void **ppv, u32 *pcb)
{
    u32 size;
    HRESULT hr;

    size = (w->fExt == 0) ? w->buf.len : w->cbExt;
    hr = wss_writer_range(w, 0, size, ppv, pcb);
    if (hr < 0)
        wss_check(hr);
    wss_pending();
    return hr;
}

/* 0x4448, vtable +0x14 */
static HRESULT wss_writer_get_pos(WssWriter *w, u32 *pcb)
{
    *pcb = w->pos;
    wss_pending();
    return S_OK;
}

/* 0x423c, vtable +0x04.  The original returns the new count, not the object. */
static LONG wss_writer_addref(WssWriter *w)
{
    return InterlockedIncrement(&w->refs);
}

/* 0x41c0, vtable +0x08 */
static LONG wss_writer_release_obj(WssWriter *w)
{
    LONG n;

    n = InterlockedDecrement(&w->refs);
    if (n == 0) {
        if (w != NULL) {
            if (w->buf.pv != NULL) {
                HeapFree(GetProcessHeap(), 0, w->buf.pv);
                w->buf.pv = NULL;
            }
            HeapFree(GetProcessHeap(), 0, w);
        }
    }
    return n;
}

/* 0x403178, vtable +0x00: first interface id, otherwise delegate to the
 * object specific QueryInterface in slot +0x24. */
static HRESULT wss_writer_qi(WssWriter *w, const void *riid, void **ppv)
{
    HRESULT hr;

    if (ppv == NULL) {
        wss_check(WSS_HR_POINTER);
        return WSS_HR_POINTER;
    }
    if (memcmp(riid, g_iid_main, sizeof g_iid_main) == 0) {
        wss_writer_addref(w);
        *ppv = w;
        hr = S_OK;
    } else {
        hr = wss_writer_qi2(w, riid, ppv);
    }
    wss_pending();
    return hr;
}

/* 0x4e70, vtable +0x24: second interface id */
static HRESULT wss_writer_qi2(WssWriter *w, const void *riid, void **ppv)
{
    HRESULT hr;

    if (ppv == NULL) {
        wss_check(WSS_HR_POINTER);
        return WSS_HR_POINTER;
    }
    if (memcmp(riid, g_iid_writer, sizeof g_iid_writer) == 0) {
        wss_writer_addref(w);
        *ppv = w;
        return S_OK;
    }
    *ppv = NULL;
    hr = WSS_HR_NOINTERFACE;
    wss_check(hr);
    return hr;
}

/* 0x4b4c, vtable +0x20: append one property value to a self-grown writer.
 * Declared in wss.h. */
HRESULT wss_writer_add(WssWriter *w, const WssValue *value)
{
    u32 vt;
    u32 datalen = 0;
    u32 namelen = 0;
    u32 nameAligned = 0;
    u32 dataAligned = 0;
    u32 hdr[4];
    u32 crc;
    u32 total;
    u32 pad;
    const void *data = NULL;
    const u8 *base;
    HRESULT hr;

    /* Add is only valid while the writer owns its buffer. */
    if (w->fExt != 0) {
        wss_check(WSS_HR_ACCESSDENIED);
        wss_pending();
        return WSS_HR_ACCESSDENIED;
    }

    vt = value->vt;
    if (vt == WSS_VT_ARRAY_PTR) {
        if (value->u.str != NULL) {
            hr = wss_str_bytes(value->u.str, &datalen);
            if (hr < 0)
                goto failed;
        }
        data = value->u.pv;
    } else if (vt == WSS_VT_ARRAY_BLOB) {
        datalen = value->cb;
        data = value->u.pv;
    } else if (vt == WSS_VT_PTR) {
        data = &value->u;               /* the four bytes of the value itself */
        datalen = 4;
    } else if (vt == WSS_VT_STRING) {
        if (value->u.str != NULL) {
            hr = wss_str_bytes(value->u.str, &datalen);
            if (hr < 0)
                goto failed;
        }
        data = value->u.pv;
    } else if (vt == WSS_VT_BLOB) {
        datalen = value->cb;
        data = value->u.pv;
    } else {
        hr = WSS_HR_INVALIDARG;
        goto failed;
    }

    if (value->name != NULL) {
        hr = wss_str_bytes(value->name, &namelen);
        if (hr < 0)
            goto failed;
    }

    hdr[0] = 0;                         /* checksum placeholder */
    hdr[1] = vt;
    hdr[2] = namelen;
    hdr[3] = datalen;

    hr = wss_buf_append(w, hdr, 0x10);
    if (hr < 0)
        goto failed;

    crc = ~wss_crc32(0xFFFFFFFFu, hdr, 0x10);

    if (namelen != 0) {
        hr = wss_buf_append(w, value->name, namelen);
        if (hr < 0)
            goto failed;
        crc = ~wss_crc32(~crc, value->name, namelen);

        nameAligned = wss_align8(namelen);
        pad = nameAligned - namelen;
        hr = wss_buf_append(w, g_zeros, pad);
        if (hr < 0)
            goto failed;
    }

    if (data != NULL && datalen != 0) {
        hr = wss_buf_append(w, data, datalen);
        if (hr < 0)
            goto failed;
        crc = ~wss_crc32(~crc, data, datalen);

        dataAligned = wss_align8(datalen);
        pad = dataAligned - datalen;
        hr = wss_buf_append(w, g_zeros, pad);
        if (hr < 0)
            goto failed;
    }

    /* store the checksum back into the first dword of the record */
    total = 0x10u + dataAligned;
    if (total < 0x10u) {
        hr = WSS_HR_ARITH;
        goto failed;
    }
    if (total + nameAligned < total) {
        hr = WSS_HR_ARITH;
        goto failed;
    }
    total += nameAligned;
    wss_pending();

    base = wss_writer_base(w);
    wss_wr32((void *)((ULONG_PTR)base + (wss_writer_len(w) - total)), crc);
    wss_pending();
    return S_OK;

failed:
    wss_check(hr);
    wss_pending();
    return hr;
}

/* ------------------------------------------------------------------------- */
/* factories                                                                 */
/* ------------------------------------------------------------------------- */

/* 0x4258: create the 0x24-byte writer.  The original allocates with
 * HeapAlloc(GetProcessHeap(), 0, 0x24), zeroes the first 0x20 bytes, stores
 * vtable 0x401ba8 and sets refcount (+0x20) to 1, then hands the object out
 * through its QueryInterface.  The net result is a single reference and
 * +0x04 == 0 (owned buffer). */
HRESULT wss_writer_create(WssWriter **out)
{
    WssWriter *w;

    if (out == NULL)
        return WSS_HR_POINTER;

    w = (WssWriter *)HeapAlloc(GetProcessHeap(), 0, sizeof(WssWriter));
    if (w == NULL) {
        wss_check(WSS_HR_OUTOFMEMORY);
        wss_pending();
        return WSS_HR_OUTOFMEMORY;
    }
    memset(w, 0, 0x20);
    w->vt = &g_writerVt;
    w->refs = 1;
    *out = w;

    wss_pending();
    return S_OK;
}

/* 0x4ebc: checked factory used by both bag directions */
static HRESULT wss_writer_create_checked(WssWriter **out)
{
    WssWriter *w = NULL;
    HRESULT hr;

    hr = wss_writer_create(&w);
    if (hr < 0) {
        wss_check(hr);
        wss_pending();
        return hr;
    }
    *out = w;
    wss_pending();
    return S_OK;
}

void wss_writer_release(WssWriter *w)
{
    if (w != NULL)
        wss_writer_release_obj(w);
}

/* ------------------------------------------------------------------------- */
/* bag -> bytes                                                              */
/* ------------------------------------------------------------------------- */

/*
 * 0x4f20: create a writer, Add every bag entry through the bag vtable
 * (count = +0x0c, entry = +0x10, each a 20-byte WssValue), then copy the
 * writer's buffer into a LocalAlloc(LPTR) block owned by the caller.
 */
HRESULT wss_bag_serialize(WssBag *bag, void **ppv, u32 *pcb)
{
    WssWriter *w = NULL;
    void *src = NULL;
    void *dst = NULL;
    u32 srclen = 0;
    u32 count = 0;
    u32 i;
    WssValue value;
    HRESULT hr;

    hr = wss_writer_create_checked(&w);
    if (hr < 0)
        goto failed;

    hr = wss_bag_get_count(bag, &count);
    if (hr < 0) {
        wss_check(hr);
        goto failed;
    }

    for (i = 0; i < count; i++) {
        /* get_value fills name/vt/u/cb; +0x10 is never touched by the
         * original, so the zeroing only keeps the compiler quiet. */
        memset(&value, 0, sizeof value);
        hr = wss_bag_get_value(bag, i, &value);
        if (hr < 0) {
            wss_check(hr);
            break;
        }
        hr = wss_writer_add(w, &value);
        if (hr < 0) {
            wss_check(hr);
            break;
        }
    }
    wss_pending();
    if (hr < 0)
        goto failed;

    hr = wss_writer_get_buffer(w, &src, &srclen);
    if (hr < 0)
        goto failed;

    if (srclen != 0) {
        dst = LocalAlloc(LPTR, srclen);
        if (dst == NULL) {
            hr = WSS_HR_OUTOFMEMORY;
            goto failed;
        }
        memcpy(dst, src, srclen);
    }
    *ppv = dst;
    *pcb = srclen;
    dst = NULL;

failed:
    wss_pending();
    if (hr < 0)
        wss_check(hr);
    wss_pending();
    if (dst != NULL)
        LocalFree(dst);
    if (w != NULL)
        wss_writer_release_obj(w);
    return hr;
}

/* ------------------------------------------------------------------------- */
/* bytes -> bag                                                              */
/* ------------------------------------------------------------------------- */

/*
 * 0x3ff0 with 0x40dc in front: wrap the caller's bytes in a writer (no copy),
 * read records until 0x80070103, and append each one to a fresh bag.
 *
 * Notes reproduced from the original:
 *   - pv == NULL returns S_OK and leaves *out untouched (cb is not checked).
 *   - cb == 0 (with pv != NULL) yields an empty bag.
 *   - only records whose vt is exactly 4 are accepted; anything else fails
 *     with 0x8007000d.
 *   - the value handed to the bag still points into the caller's buffer; the
 *     bag's set path is expected to deep copy it (0x403770 -> 0x39f0).
 */
HRESULT wss_bag_deserialize(WssBag **out, const void *pv, u32 cb)
{
    WssWriter *w = NULL;
    WssBag *bag = NULL;
    u32 index = 0;
    HRESULT hr = S_OK;

    if (out == NULL)
        return WSS_HR_POINTER;
    if (pv == NULL)
        return S_OK;

    if (cb != 0) {
        hr = wss_writer_create_checked(&w);
        if (hr < 0)
            goto failed;
        hr = wss_writer_set_buffer(w, pv, cb, 0);
        if (hr < 0) {
            wss_check(hr);
            goto failed;
        }
    }

    hr = wss_bag_create(&bag);
    if (hr < 0) {
        wss_check(hr);
        goto failed;
    }

    if (w != NULL) {
        for (;;) {
            WssRecord rec;
            WssValue value;

            hr = wss_writer_read_record(w, &rec);
            if (hr == WSS_HR_NOMOREITEMS) {
                hr = S_OK;
                break;
            }
            if (hr < 0)
                break;
            if (rec.vt != WSS_VT_BLOB) {
                hr = WSS_HR_INVALIDDATA;
                break;
            }

            memset(&value, 0, sizeof value);
            value.name = rec.name;
            value.vt = rec.vt;
            value.u.pv = rec.data;
            value.cb = rec.cb;

            hr = wss_bag_set_value(bag, index, &value);
            if (hr < 0)
                break;
            index++;
        }
        if (hr < 0)
            wss_check(hr);
        wss_pending();
    }

failed:
    if (w != NULL) {
        wss_writer_release_obj(w);
        w = NULL;
    }
    wss_pending();

    if (hr < 0) {
        wss_check(hr);
        if (bag != NULL)
            wss_bag_release(bag);
        return hr;
    }

    {
        WssBag *old = *out;

        if (old != NULL) {
            wss_bag_release(old);
            *out = NULL;
        }
        *out = bag;
    }
    return hr;
}

/* ------------------------------------------------------------------------- */
/* bag holder -> bytes (0x5030)                                              */
/* ------------------------------------------------------------------------- */
/*
 * Same as wss_bag_serialize but takes the "WssBag **" holder the RPC layer
 * keeps: a NULL holder serializes to {NULL, 0} and S_OK.  Not declared in
 * wss.h because the public path uses wss_bag_serialize.
 */
HRESULT wss_bag_get_buffer(WssBag **ppbag, void **ppv, u32 *pcb)
{
    void *buf = NULL;
    u32 len = 0;
    HRESULT hr = S_OK;
    void *orphan = NULL;

    if (ppv == NULL || pcb == NULL) {
        wss_check(WSS_HR_INVALIDARG);
        hr = WSS_HR_INVALIDARG;
    } else if (ppbag == NULL) {
        wss_check(WSS_HR_POINTER);
        hr = WSS_HR_POINTER;
    } else if (*ppbag == NULL) {
        *ppv = NULL;
        *pcb = 0;
    } else {
        hr = wss_bag_serialize(*ppbag, &buf, &len);
        if (hr >= 0) {
            *ppv = buf;
            *pcb = len;
        } else {
            wss_check(hr);
            orphan = buf;
        }
    }
    wss_pending();
    if (orphan != NULL)
        LocalFree(orphan);
    return hr;
}

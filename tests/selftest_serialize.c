/* Self-test: build the install request exactly like wss_install_license() does and
 * hex-dump it.  Pure in-memory: no service, no RPC, no file system access. */
#include "wss.h"
#include "app.h"
#include <stdio.h>

static void dump(const char *tag, const unsigned char *p, u32 n)
{
    u32 i;
    printf("%s: %u bytes\n", tag, n);
    for (i = 0; i < n; i++) {
        printf("%02x%s", p[i], ((i % 16) == 15) ? "\n" : " ");
    }
    if (n % 16)
        printf("\n");
}

int main(void)
{
    WssBag *bag = NULL, *result = NULL;
    void *wire = NULL;
    u32 wirelen = 0;
    u32 size;
    u32 magic = 6;
    unsigned char fake[0x20];
    HRESULT hr;
    int i;

    for (i = 0; i < (int)sizeof(fake); i++)
        fake[i] = (unsigned char)(0x41 + (i % 26));
    size = sizeof(fake);

    hr = wss_bag_create(&bag);
    printf("create hr=0x%08x bag=%p\n", (u32)hr, (void *)bag);
    if (hr < 0)
        return 1;

    {
        WssValue v;
        memset(&v, 0, sizeof(v));
        v.vt = WSS_VT_BLOB;
        v.u.pv = &size;
        v.cb = 4;
        hr = wss_bag_set_value(bag, 0, &v);
        printf("set[0] hr=0x%08x\n", (u32)hr);

        memset(&v, 0, sizeof(v));
        v.vt = WSS_VT_BLOB;
        v.u.pv = fake;
        v.cb = size;
        hr = wss_bag_set_value(bag, 1, &v);
        printf("set[1] hr=0x%08x\n", (u32)hr);

        memset(&v, 0, sizeof(v));
        v.vt = WSS_VT_BLOB;
        v.u.pv = &magic;
        v.cb = 4;
        hr = wss_bag_set_value(bag, 2, &v);
        printf("set[2] hr=0x%08x\n", (u32)hr);
    }

    {
        u32 cnt = 0;
        hr = wss_bag_get_count(bag, &cnt);
        printf("count hr=0x%08x n=%u\n", (u32)hr, cnt);
    }

    hr = wss_bag_serialize(bag, &wire, &wirelen);
    printf("serialize hr=0x%08x len=%u pv=%p\n", (u32)hr, wirelen, wire);
    if (hr >= 0 && wire != NULL)
        dump("request", (const unsigned char *)wire, wirelen);

    if (wire)
        LocalFree(wire);
    if (bag)
        wss_bag_release(bag);
    if (result)
        wss_bag_release(result);
    return 0;
}

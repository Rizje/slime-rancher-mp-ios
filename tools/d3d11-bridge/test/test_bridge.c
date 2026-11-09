/*
 * Test for sr2-d3d12-bridge, run under Wine by test/run.sh.
 *
 * It loads d3d11.dll from its own folder (the bridge) and hands its
 * D3D11On12CreateDevice a fake D3D12 device built like madeira-d3d12's: one
 * object answering ID3D12Device..ID3D12Device8, a 79-slot function table, and
 * E_INVALIDARG for feature queries it does not know. The table sits in a
 * read-only page, as a reminder that the bridge must never write to the D3D12
 * layer's memory: in Madeira that table lives in a JIT pool where changing
 * page protection wiped it (bridge version 2 crashed calling Release through
 * it). The test checks that:
 *   - the bridge, not the system d3d11.dll, answered;
 *   - the D3D12 layer's table is unchanged and still read-only;
 *   - the device now points at the bridge's copy, which keeps all 79 slots;
 *   - the guard turns an unknown capability query (TIGHT_ALIGNMENT, 54) into
 *     "not supported" with zeroed data;
 *   - SHADER_MODEL and FORMAT_INFO errors pass through untouched;
 *   - answers that succeed pass through untouched;
 *   - a fence with flags, which madeira-d3d12 refuses with E_NOTIMPL, is made
 *     again without them;
 *   - other failures (a bundle command allocator here) pass through unchanged
 *     and are logged by name;
 *   - a second D3D11On12CreateDevice does not wrap twice;
 *   - references stay balanced;
 *   - SR2_BRIDGE_GUARD=0 leaves a device alone;
 *   - the exports are exactly DXMT's.
 * With "expect-device" (run.sh's default, against test/fake_d3d11.c) it also
 * checks the standalone device that comes back: device, context, feature
 * level, the debug flag dropped, and the retry when only 12_x levels are
 * asked for. Against Wine's own d3d11.dll the result depends on the
 * machine's graphics and is only reported.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <d3d12.h>

#define FAKE_SLOTS 79   /* madeira-d3d12's device table: ID3D12Device10 */

typedef HRESULT (WINAPI *on12_fn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *,
                                  UINT, UINT, ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

/* IUnknown, then ID3D12Device and ID3D12Device1..8: what madeira-d3d12 answers. */
static const GUID fake_iids[] =
{
    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } },
    { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } },
    { 0x77acce80, 0x638e, 0x4e65, { 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e } },
    { 0x30baa41e, 0xb15b, 0x475c, { 0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28 } },
    { 0x81dadc15, 0x2bad, 0x4392, { 0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98 } },
    { 0xe865df17, 0xa9ee, 0x46f9, { 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5 } },
    { 0x8b4f173b, 0x2fea, 0x4b80, { 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d } },
    { 0xc70b221b, 0x40e4, 0x4a17, { 0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc } },
    { 0x5c014b53, 0x68a1, 0x4b9b, { 0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b } },
    { 0x9218e6bb, 0xf944, 0x4f7e, { 0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3 } },
};

static int failures;
static LONG fake_refs = 1;
static int fake_cfs_calls;
static UINT fake_fence_flags[4];
static int fake_fence_calls;

#define CHECK(cond, ...) do { if (cond) printf("ok    "); else { printf("FAIL  "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

struct fake_object { void **vtbl; };
static struct fake_object fake_device, fake_device_unguarded;
static void **fake_table;
static void *fake_snapshot[FAKE_SLOTS];

static HRESULT STDMETHODCALLTYPE fake_qi(struct fake_object *self, REFIID iid, void **out)
{
    size_t i;

    for (i = 0; i < sizeof(fake_iids) / sizeof(fake_iids[0]); i++)
        if (IsEqualGUID(iid, &fake_iids[i]))
        {
            InterlockedIncrement(&fake_refs);
            *out = self;
            return S_OK;
        }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fake_addref(struct fake_object *self) { (void)self; return InterlockedIncrement(&fake_refs); }
static ULONG STDMETHODCALLTYPE fake_release(struct fake_object *self) { (void)self; return InterlockedDecrement(&fake_refs); }
static void STDMETHODCALLTYPE fake_last_slot(struct fake_object *self) { (void)self; }

/* Like madeira-d3d12: fences with flags and bundle lists are refused, silently. */
static HRESULT STDMETHODCALLTYPE fake_create_fence(struct fake_object *self, UINT64 initial, UINT flags,
                                                   REFIID riid, void **out)
{
    (void)self; (void)initial; (void)riid;
    if (fake_fence_calls < 4) fake_fence_flags[fake_fence_calls] = flags;
    fake_fence_calls++;
    if (flags) return E_NOTIMPL;
    *out = (void *)&fake_fence_calls;   /* any non-NULL pointer */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fake_create_allocator(struct fake_object *self, UINT type, REFIID riid, void **out)
{
    (void)self; (void)riid;
    *out = NULL;
    return type == 1 ? E_NOTIMPL : E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE fake_removed_reason(struct fake_object *self) { (void)self; return S_OK; }

typedef HRESULT (STDMETHODCALLTYPE *create_fence_fn)(void *, UINT64, UINT, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *create_allocator_fn)(void *, UINT, REFIID, void **);
typedef HRESULT (STDMETHODCALLTYPE *removed_reason_fn)(void *);

static int log_contains(const char *text)
{
    char line[1024];
    int found = 0;
    FILE *f = fopen("C:\\sr2-d3d12-bridge.log", "r");

    if (!f) return 0;
    while (!found && fgets(line, sizeof(line), f)) found = strstr(line, text) != NULL;
    fclose(f);
    return found;
}

/* Like madeira-d3d12: known features answered, unknown ones E_INVALIDARG. */
static HRESULT STDMETHODCALLTYPE fake_cfs(struct fake_object *self, D3D12_FEATURE feature, void *data, UINT size)
{
    (void)self;
    fake_cfs_calls++;
    if (!data) return E_INVALIDARG;
    switch ((UINT)feature)
    {
    case 27:   /* OPTIONS5: a real answer */
        memset(data, 0x11, size);
        return S_OK;
    case 7:    /* SHADER_MODEL with a model this runtime refuses */
    case 5:    /* FORMAT_INFO for a format it does not have */
    default:   /* 54 TIGHT_ALIGNMENT and anything newer */
        return E_INVALIDARG;
    }
}

static void make_fake_table(void)
{
    DWORD old;

    fake_table = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    fake_table[0] = (void *)fake_qi;
    fake_table[1] = (void *)fake_addref;
    fake_table[2] = (void *)fake_release;
    fake_table[9] = (void *)fake_create_allocator;
    fake_table[13] = (void *)fake_cfs;
    fake_table[36] = (void *)fake_create_fence;
    fake_table[37] = (void *)fake_removed_reason;
    fake_table[FAKE_SLOTS - 1] = (void *)fake_last_slot;
    memcpy(fake_snapshot, fake_table, sizeof(fake_snapshot));
    VirtualProtect(fake_table, 4096, PAGE_READONLY, &old);
    fake_device.vtbl = fake_table;
    fake_device_unguarded.vtbl = fake_table;
}

static DWORD page_protection(void *p)
{
    MEMORY_BASIC_INFORMATION info;

    if (!VirtualQuery(p, &info, sizeof(info))) return 0;
    return info.Protect;
}

static void dump_log(void)
{
    char line[1024];
    FILE *f = fopen("C:\\sr2-d3d12-bridge.log", "r");

    printf("---- C:\\sr2-d3d12-bridge.log\n");
    if (!f) { printf("(no log)\n"); return; }
    while (fgets(line, sizeof(line), f)) fputs(line, stdout);
    fclose(f);
    printf("----\n");
}

int main(int argc, char **argv)
{
    ID3D12Device *device = (ID3D12Device *)&fake_device;
    HMODULE bridge;
    on12_fn on12;
    char path[MAX_PATH];
    unsigned char buf[16];
    ID3D11Device *d11 = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL only_12[] = { D3D_FEATURE_LEVEL_12_0 };
    D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
    int expect_device = argc > 1 && !strcmp(argv[1], "expect-device");
    char flags_seen[32];
    void **copy;
    HRESULT hr;

    DeleteFileA("C:\\sr2-d3d12-bridge.log");
    make_fake_table();

    bridge = LoadLibraryA("d3d11.dll");
    CHECK(bridge != NULL, "d3d11.dll loads (error %lu)", bridge ? 0ul : GetLastError());
    if (!bridge) return 1;
    GetModuleFileNameA(bridge, path, sizeof(path));
    CHECK(strstr(path, "windows") == NULL && strstr(path, "Windows") == NULL,
          "d3d11.dll by name is the bridge in the program's folder: %s", path);

    on12 = (on12_fn)(void *)GetProcAddress(bridge, "D3D11On12CreateDevice");
    CHECK(on12 != NULL, "the bridge exports D3D11On12CreateDevice");
    if (!on12) return 1;
    CHECK(GetProcAddress(bridge, "D3D11CreateDevice") && GetProcAddress(bridge, "D3D11CreateDeviceAndSwapChain") &&
          GetProcAddress(bridge, "D3D11CoreCreateDevice") &&
          !GetProcAddress(bridge, "CreateDirect3D11DeviceFromDXGIDevice") &&
          !GetProcAddress(bridge, "CreateDirect3D11SurfaceFromDXGISurface"),
          "the exports are DXMT's four, no more");

    hr = on12((IUnknown *)device, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, levels, 3,
              NULL, 0, 0, &d11, &context, &chosen);
    printf("info  D3D11On12CreateDevice -> %#lx, feature level %#x\n", (unsigned long)hr, (unsigned)chosen);
    if (expect_device)
    {
        CHECK(hr == S_OK && d11 != NULL && context != NULL, "a standalone device and context came back");
        CHECK(chosen == D3D_FEATURE_LEVEL_11_1, "feature level 11_1 reported to the caller (%#x)", (unsigned)chosen);
        if (!GetEnvironmentVariableA("FAKE_D3D11_FLAGS", flags_seen, sizeof(flags_seen))) strcpy(flags_seen, "?");
        CHECK(!strcmp(flags_seen, "0x20"), "the debug flag was dropped, BGRA kept (real d3d11 saw %s)", flags_seen);
    }
    if (SUCCEEDED(hr))
    {
        if (context) ID3D11DeviceContext_Release(context);
        if (d11) ID3D11Device_Release(d11);
    }
    d11 = NULL; context = NULL;

    if (expect_device)
    {
        chosen = (D3D_FEATURE_LEVEL)0;
        hr = on12((IUnknown *)device, 0, only_12, 1, NULL, 0, 0, &d11, &context, &chosen);
        CHECK(hr == S_OK && d11 != NULL && chosen == D3D_FEATURE_LEVEL_11_1,
              "only 12_0 asked for: retried with the defaults (hr %#lx, level %#x)", (unsigned long)hr, (unsigned)chosen);
        d11 = NULL; context = NULL;
    }

    CHECK(!memcmp(fake_table, fake_snapshot, sizeof(fake_snapshot)), "the D3D12 layer's table was not written");
    CHECK(page_protection(fake_table) == PAGE_READONLY, "its page is still read-only (protection %#lx)",
          page_protection(fake_table));
    copy = fake_device.vtbl;
    CHECK(copy != fake_table, "the device now points at the bridge's copy of its table");
    CHECK(copy[0] == (void *)fake_qi && copy[2] == (void *)fake_release && copy[FAKE_SLOTS - 1] == (void *)fake_last_slot,
          "the copy keeps QueryInterface, Release and the last of the %d slots", FAKE_SLOTS);
    CHECK(copy[13] != (void *)fake_cfs && copy[36] != (void *)fake_create_fence &&
          copy[9] != (void *)fake_create_allocator,
          "the copy's CheckFeatureSupport, CreateFence and CreateCommandAllocator are the bridge's");

    memset(buf, 0xab, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(device, (D3D12_FEATURE)54, buf, 4);
    CHECK(hr == S_OK && buf[0] == 0 && buf[3] == 0 && buf[4] == 0xab,
          "TIGHT_ALIGNMENT (54): \"not supported\", 4 bytes zeroed, nothing past them (hr %#lx)", (unsigned long)hr);

    memset(buf, 0xab, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, buf, 4);
    CHECK(hr == E_INVALIDARG && buf[0] == 0xab, "SHADER_MODEL error passes through, data untouched (hr %#lx)",
          (unsigned long)hr);

    memset(buf, 0xab, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_FORMAT_INFO, buf, 8);
    CHECK(hr == E_INVALIDARG && buf[0] == 0xab, "FORMAT_INFO error passes through, data untouched (hr %#lx)",
          (unsigned long)hr);

    memset(buf, 0, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS5, buf, 12);
    CHECK(hr == S_OK && buf[0] == 0x11 && buf[11] == 0x11, "OPTIONS5's real answer passes through (hr %#lx)",
          (unsigned long)hr);

    hr = ID3D12Device_CheckFeatureSupport(device, (D3D12_FEATURE)54, NULL, 4);
    CHECK(hr == E_INVALIDARG, "a NULL buffer is still refused (hr %#lx)", (unsigned long)hr);

    {
        static const GUID iid_fence = { 0x0a753dcf, 0xc4d8, 0x4b91, { 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76 } };
        void *object = NULL;

        fake_fence_calls = 0;
        hr = ((create_fence_fn)fake_device.vtbl[36])(device, 0, 1 /* D3D12_FENCE_FLAG_SHARED */, &iid_fence, &object);
        CHECK(hr == S_OK && object && fake_fence_calls == 2 && fake_fence_flags[0] == 1 && fake_fence_flags[1] == 0,
              "a shared fence the layer refuses is made without the flag (hr %#lx, %d calls, flags %u then %u)",
              (unsigned long)hr, fake_fence_calls, fake_fence_flags[0], fake_fence_flags[1]);
        fake_fence_calls = 0;
        hr = ((create_fence_fn)fake_device.vtbl[36])(device, 0, 0, &iid_fence, &object);
        CHECK(hr == S_OK && fake_fence_calls == 1, "an ordinary fence is one call (hr %#lx, %d calls)",
              (unsigned long)hr, fake_fence_calls);
        hr = ((create_allocator_fn)fake_device.vtbl[9])(device, 1 /* BUNDLE */, &iid_fence, &object);
        CHECK(hr == E_NOTIMPL && log_contains("FAILED CreateCommandAllocator(type 1) -> 0x80004001"),
              "a refused bundle allocator passes through and is logged by name (hr %#lx)", (unsigned long)hr);
        hr = ((removed_reason_fn)fake_device.vtbl[37])(device);
        CHECK(hr == S_OK, "GetDeviceRemovedReason passes through (hr %#lx)", (unsigned long)hr);
        CHECK(log_contains("CreateFence(flags 0x1) was refused (E_NOTIMPL): made an ordinary fence instead -> 0"),
              "the fence substitution is logged");
    }

    on12((IUnknown *)device, 0, NULL, 0, NULL, 0, 0, &d11, &context, NULL);
    if (d11) ID3D11Device_Release(d11);
    if (context) ID3D11DeviceContext_Release(context);
    d11 = NULL; context = NULL;
    fake_cfs_calls = 0;
    ID3D12Device_CheckFeatureSupport(device, (D3D12_FEATURE)54, buf, 4);
    CHECK(fake_cfs_calls == 1 && fake_device.vtbl == copy,
          "a second D3D11On12CreateDevice does not wrap twice (%d real calls per query)", fake_cfs_calls);
    CHECK(fake_refs == 1, "the fake device's references are balanced (%ld)", (long)fake_refs);

    SetEnvironmentVariableA("SR2_BRIDGE_GUARD", "0");
    on12((IUnknown *)&fake_device_unguarded, 0, NULL, 0, NULL, 0, 0, &d11, &context, NULL);
    if (d11) ID3D11Device_Release(d11);
    if (context) ID3D11DeviceContext_Release(context);
    CHECK(fake_device_unguarded.vtbl == fake_table, "SR2_BRIDGE_GUARD=0 leaves a device alone");

    dump_log();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}

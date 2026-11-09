/*
 * Test for sr2-d3d12-bridge, run under Wine by test/run.sh.
 *
 * It loads d3d11.dll from its own folder (the bridge), hands its
 * D3D11On12CreateDevice a fake D3D12 device whose CheckFeatureSupport behaves
 * like madeira-d3d12's (E_INVALIDARG for features it does not know), and
 * checks what the bridge did:
 *   - the bridge, not the system d3d11.dll, answered;
 *   - the guard turns an unknown capability query (TIGHT_ALIGNMENT, 54) into
 *     "not supported" with zeroed data;
 *   - SHADER_MODEL and FORMAT_INFO errors pass through untouched;
 *   - answers that succeed pass through untouched;
 *   - a second D3D11On12CreateDevice does not hook the table twice.
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

typedef HRESULT (WINAPI *on12_fn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *,
                                  UINT, UINT, ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

static const GUID iid_d3d12_device =
    { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };
static const GUID iid_unknown =
    { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

static int failures;
static LONG fake_refs = 1;
static int fake_cfs_calls;

#define CHECK(cond, ...) do { if (cond) printf("ok    "); else { printf("FAIL  "); failures++; } \
                              printf(__VA_ARGS__); printf("\n"); } while (0)

static HRESULT STDMETHODCALLTYPE fake_qi(ID3D12Device *self, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &iid_d3d12_device) || IsEqualGUID(iid, &iid_unknown))
    {
        InterlockedIncrement(&fake_refs);
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fake_addref(ID3D12Device *self) { (void)self; return InterlockedIncrement(&fake_refs); }
static ULONG STDMETHODCALLTYPE fake_release(ID3D12Device *self) { (void)self; return InterlockedDecrement(&fake_refs); }

/* Like madeira-d3d12: known features answered, unknown ones E_INVALIDARG. */
static HRESULT STDMETHODCALLTYPE fake_cfs(ID3D12Device *self, D3D12_FEATURE feature, void *data, UINT size)
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

static ID3D12DeviceVtbl fake_vtbl;   /* writable, as madeira-d3d12's is */
static ID3D12Device fake_device = { &fake_vtbl };

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
    HMODULE bridge;
    on12_fn on12;
    char path[MAX_PATH];
    unsigned char buf[16];
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL chosen = (D3D_FEATURE_LEVEL)0;
    D3D_FEATURE_LEVEL only_12[] = { D3D_FEATURE_LEVEL_12_0 };
    int expect_device = argc > 1 && !strcmp(argv[1], "expect-device");
    char flags_seen[32];
    void *original_cfs;
    HRESULT hr;

    DeleteFileA("C:\\sr2-d3d12-bridge.log");
    fake_vtbl.QueryInterface = fake_qi;
    fake_vtbl.AddRef = fake_addref;
    fake_vtbl.Release = fake_release;
    fake_vtbl.CheckFeatureSupport = fake_cfs;
    original_cfs = (void *)fake_vtbl.CheckFeatureSupport;

    bridge = LoadLibraryA("d3d11.dll");
    CHECK(bridge != NULL, "d3d11.dll loads (error %lu)", bridge ? 0ul : GetLastError());
    if (!bridge) return 1;
    GetModuleFileNameA(bridge, path, sizeof(path));
    CHECK(strstr(path, "windows") == NULL && strstr(path, "Windows") == NULL,
          "d3d11.dll by name is the bridge in the program's folder: %s", path);

    on12 = (on12_fn)(void *)GetProcAddress(bridge, "D3D11On12CreateDevice");
    CHECK(on12 != NULL, "the bridge exports D3D11On12CreateDevice");
    if (!on12) return 1;

    hr = on12((IUnknown *)&fake_device, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, levels, 3,
              NULL, 0, 0, &device, &context, &chosen);
    printf("info  D3D11On12CreateDevice -> %#lx, feature level %#x\n", (unsigned long)hr, (unsigned)chosen);
    if (expect_device)
    {
        CHECK(hr == S_OK && device != NULL && context != NULL, "a standalone device and context came back");
        CHECK(chosen == D3D_FEATURE_LEVEL_11_1, "feature level 11_1 reported to the caller (%#x)", (unsigned)chosen);
        if (!GetEnvironmentVariableA("FAKE_D3D11_FLAGS", flags_seen, sizeof(flags_seen))) strcpy(flags_seen, "?");
        CHECK(!strcmp(flags_seen, "0x20"), "the debug flag was dropped, BGRA kept (real d3d11 saw %s)", flags_seen);
    }
    if (SUCCEEDED(hr))
    {
        if (context) ID3D11DeviceContext_Release(context);
        if (device) ID3D11Device_Release(device);
    }
    device = NULL; context = NULL;

    if (expect_device)
    {
        chosen = (D3D_FEATURE_LEVEL)0;
        hr = on12((IUnknown *)&fake_device, 0, only_12, 1, NULL, 0, 0, &device, &context, &chosen);
        CHECK(hr == S_OK && device != NULL && chosen == D3D_FEATURE_LEVEL_11_1,
              "only 12_0 asked for: retried with the defaults (hr %#lx, level %#x)", (unsigned long)hr, (unsigned)chosen);
        device = NULL; context = NULL;
    }

    CHECK((void *)fake_vtbl.CheckFeatureSupport != original_cfs, "the guard replaced CheckFeatureSupport");

    memset(buf, 0xab, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(&fake_device, (D3D12_FEATURE)54, buf, 4);
    CHECK(hr == S_OK && buf[0] == 0 && buf[3] == 0 && buf[4] == 0xab,
          "TIGHT_ALIGNMENT (54): \"not supported\", 4 bytes zeroed, nothing past them (hr %#lx)", (unsigned long)hr);

    memset(buf, 0xab, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(&fake_device, D3D12_FEATURE_SHADER_MODEL, buf, 4);
    CHECK(hr == E_INVALIDARG && buf[0] == 0xab, "SHADER_MODEL error passes through, data untouched (hr %#lx)",
          (unsigned long)hr);

    memset(buf, 0xab, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(&fake_device, D3D12_FEATURE_FORMAT_INFO, buf, 8);
    CHECK(hr == E_INVALIDARG && buf[0] == 0xab, "FORMAT_INFO error passes through, data untouched (hr %#lx)",
          (unsigned long)hr);

    memset(buf, 0, sizeof(buf));
    hr = ID3D12Device_CheckFeatureSupport(&fake_device, D3D12_FEATURE_D3D12_OPTIONS5, buf, 12);
    CHECK(hr == S_OK && buf[0] == 0x11 && buf[11] == 0x11, "OPTIONS5's real answer passes through (hr %#lx)",
          (unsigned long)hr);

    hr = ID3D12Device_CheckFeatureSupport(&fake_device, (D3D12_FEATURE)54, NULL, 4);
    CHECK(hr == E_INVALIDARG, "a NULL buffer is still refused (hr %#lx)", (unsigned long)hr);

    on12((IUnknown *)&fake_device, 0, NULL, 0, NULL, 0, 0, &device, &context, NULL);
    if (device) ID3D11Device_Release(device);
    if (context) ID3D11DeviceContext_Release(context);
    fake_cfs_calls = 0;
    ID3D12Device_CheckFeatureSupport(&fake_device, (D3D12_FEATURE)54, buf, 4);
    CHECK(fake_cfs_calls == 1, "a second D3D11On12CreateDevice does not hook twice (%d real calls per query)",
          fake_cfs_calls);
    CHECK(fake_refs == 1, "the fake device's references are balanced (%ld)", (long)fake_refs);

    dump_log();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}

/*
 * Stand-in for the real d3d11.dll in test/run.sh: answers like DXMT does
 * (no 11-on-12, D3D11 devices up to feature level 11_1) without needing a GPU.
 * What the bridge asked for is left in environment variables for the test.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <d3d11.h>

typedef struct { void *vtbl; } fake_object;
static HRESULT STDMETHODCALLTYPE fake_qi(void *self, REFIID iid, void **out) { (void)self; (void)iid; *out = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE fake_addref(void *self) { (void)self; return 1; }
static ULONG STDMETHODCALLTYPE fake_release(void *self) { (void)self; return 0; }
static void *fake_vtbl[3] = { (void *)fake_qi, (void *)fake_addref, (void *)fake_release };
static fake_object fake_device = { fake_vtbl }, fake_context = { fake_vtbl };

HRESULT WINAPI fake_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
                                      UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count,
                                      UINT sdk_version, ID3D11Device **device,
                                      D3D_FEATURE_LEVEL *chosen_level, ID3D11DeviceContext **context)
{
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    char text[32];
    UINT i;

    (void)adapter; (void)software; (void)sdk_version;
    snprintf(text, sizeof(text), "%#x", flags);
    SetEnvironmentVariableA("FAKE_D3D11_FLAGS", text);
    if (driver_type != D3D_DRIVER_TYPE_HARDWARE) return E_INVALIDARG;
    if (!levels || !level_count) got = D3D_FEATURE_LEVEL_11_1;
    for (i = 0; levels && i < level_count && !got; i++)
        if (levels[i] <= D3D_FEATURE_LEVEL_11_1) got = levels[i];
    if (!got) return E_INVALIDARG;   /* only 12_x asked for */
    if (device) *device = (ID3D11Device *)&fake_device;
    if (context) *context = (ID3D11DeviceContext *)&fake_context;
    if (chosen_level) *chosen_level = got;
    return S_OK;
}

HRESULT WINAPI fake_D3D11On12CreateDevice(void)
{
    return E_NOTIMPL;
}

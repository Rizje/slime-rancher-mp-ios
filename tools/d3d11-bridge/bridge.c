/*
 * sr2-d3d12-bridge: a d3d11.dll placed in Slime Rancher 2's game folder.
 *
 * Why it exists
 *   Unity's Direct3D 12 renderer creates a "D3D11On12" device right after its
 *   D3D12 device. Madeira's d3d11.dll (DXMT) answers D3D11On12CreateDevice with
 *   E_NOTIMPL, so Unity logs
 *       d3d12: failed to create D3D11On12 device (0x80004001).
 *   and falls back to Direct3D 11, where its "split" graphics jobs are not
 *   available ("Reverting to kGfxThreadingModeClientWorkerJobs").
 *
 * What it does
 *   - D3D11CreateDevice, D3D11CreateDeviceAndSwapChain and the other ordinary
 *     entry points go straight to the real d3d11.dll.
 *   - D3D11On12CreateDevice first asks the real d3d11.dll. When that cannot make
 *     an 11-on-12 device, it returns a standalone D3D11 device instead. Unity
 *     only uses the 11-on-12 device briefly while it starts.
 *   - The D3D12 device passed to D3D11On12CreateDevice gets a guard on
 *     CheckFeatureSupport. A capability query the D3D12 layer does not know
 *     (E_INVALIDARG) is answered "not supported" (S_OK, zeroed data): Unity 6
 *     treats E_INVALIDARG for D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT as fatal.
 *     Queries whose E_INVALIDARG is a real answer about the caller's input
 *     (formats, shader model, root signature version, feature level list) are
 *     left alone, because a zeroed answer to those would be wrong.
 *
 * Where the real d3d11.dll is
 *   The system directory first, as Windows and Wine search. Madeira runs x64
 *   games next to ARM64 system DLLs: there, system32 holds the ARM64 build,
 *   which an x64 process cannot load, and the x64-compatible (ARM64EC) build is
 *   in C:\windows\sysx64, where Madeira's own loader looks next. The bridge
 *   follows the same order and never takes itself or a DLL without
 *   D3D11CreateDevice.
 *
 *   Everything it does is written to C:\sr2-d3d12-bridge.log (at most a few
 *   hundred lines). Delete the DLL from the game folder to undo it.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <d3d11.h>
#include <d3d12.h>

#define BRIDGE_VERSION "2"
#define BRIDGE_LOG_PATH "C:\\sr2-d3d12-bridge.log"
#define BRIDGE_LOG_MAX_LINES 300
#define BRIDGE_MAX_TABLES 8
#define BRIDGE_MAX_GUARDED_SIZE 4096

/* ID3D12Device's IID, written out so no import library is needed. */
static const GUID bridge_iid_d3d12_device =
    { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } };

/* CheckFeatureSupport's slot in every ID3D12Device* vtable (they all extend ID3D12Device). */
#define BRIDGE_CFS_SLOT (offsetof(ID3D12DeviceVtbl, CheckFeatureSupport) / sizeof(void *))

typedef HRESULT (WINAPI *bridge_on12_fn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                         IUnknown *const *, UINT, UINT, ID3D11Device **,
                                         ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);
typedef HRESULT (WINAPI *bridge_core_create_fn)(IDXGIFactory *, IDXGIAdapter *, UINT,
                                                const D3D_FEATURE_LEVEL *, UINT, ID3D11Device **);
typedef HRESULT (WINAPI *bridge_from_dxgi_device_fn)(IDXGIDevice *, IUnknown **);
typedef HRESULT (WINAPI *bridge_from_dxgi_surface_fn)(IDXGISurface *, IUnknown **);
typedef HRESULT (STDMETHODCALLTYPE *bridge_cfs_fn)(ID3D12Device *, D3D12_FEATURE, void *, UINT);

static HMODULE bridge_self;
static HMODULE bridge_real;
static INIT_ONCE bridge_once = INIT_ONCE_STATIC_INIT;

static SRWLOCK bridge_log_lock = SRWLOCK_INIT;
static LONG bridge_log_lines;

static SRWLOCK bridge_table_lock = SRWLOCK_INIT;
static struct { void **table; bridge_cfs_fn real; } bridge_tables[BRIDGE_MAX_TABLES];
static volatile LONG bridge_ntables;

static volatile LONG bridge_noted_features[64];
static volatile LONG bridge_nnoted;

static void bridge_log(const char *fmt, ...)
{
    char line[700];
    SYSTEMTIME now;
    va_list args;
    int used, more;
    HANDLE file;
    DWORD written;

    if (InterlockedIncrement(&bridge_log_lines) > BRIDGE_LOG_MAX_LINES) return;

    GetLocalTime(&now);
    used = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [pid %lu tid %lu] ",
                    now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                    GetCurrentProcessId(), GetCurrentThreadId());
    if (used < 0) return;
    va_start(args, fmt);
    more = vsnprintf(line + used, sizeof(line) - used - 2, fmt, args);
    va_end(args);
    if (more < 0) return;
    used += more;
    if (used > (int)sizeof(line) - 2) used = sizeof(line) - 2;
    line[used++] = '\r';
    line[used++] = '\n';

    AcquireSRWLockExclusive(&bridge_log_lock);
    file = CreateFileA(BRIDGE_LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteFile(file, line, (DWORD)used, &written, NULL);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&bridge_log_lock);
}

/* ---- the real d3d11.dll ------------------------------------------------- */

static HMODULE bridge_try_load(const WCHAR *path)
{
    char narrow[MAX_PATH * 3];
    HMODULE module;
    DWORD error;

    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), NULL, NULL))
        strcpy(narrow, "(path)");
    module = LoadLibraryW(path);
    error = GetLastError();
    if (!module)
    {
        bridge_log("could not load %s (error %lu)", narrow, error);
        return NULL;
    }
    if (module == bridge_self)
    {
        /* The loader handed this DLL back: calling "the real one" would recurse. */
        bridge_log("%s resolved to this bridge itself; skipped", narrow);
        FreeLibrary(module);
        return NULL;
    }
    if (!GetProcAddress(module, "D3D11CreateDevice"))
    {
        bridge_log("%s has no D3D11CreateDevice; skipped", narrow);
        FreeLibrary(module);
        return NULL;
    }
    if (!GetModuleFileNameA(module, narrow, sizeof(narrow))) strcpy(narrow, "(unknown path)");
    bridge_log("real d3d11.dll: %s at %p", narrow, (void *)module);
    return module;
}

static BOOL CALLBACK bridge_load_real(PINIT_ONCE once, void *param, void **context)
{
    WCHAR path[MAX_PATH + 32];
    char exe[MAX_PATH];
    UINT len;

    (void)once; (void)param; (void)context;
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) strcpy(exe, "(unknown program)");
    bridge_log("sr2-d3d12-bridge %s in %s", BRIDGE_VERSION, exe);

    len = GetSystemDirectoryW(path, MAX_PATH);
    if (len && len < MAX_PATH)
    {
        lstrcatW(path, L"\\d3d11.dll");
        bridge_real = bridge_try_load(path);
    }
    if (!bridge_real)
    {
        len = GetWindowsDirectoryW(path, MAX_PATH);
        if (len && len < MAX_PATH)
        {
            lstrcatW(path, L"\\sysx64\\d3d11.dll");
            bridge_real = bridge_try_load(path);
        }
    }
    if (!bridge_real) bridge_log("NO real d3d11.dll found: Direct3D 11 calls will fail");
    return TRUE;
}

static FARPROC bridge_real_proc(const char *name)
{
    FARPROC proc;

    InitOnceExecuteOnce(&bridge_once, bridge_load_real, NULL, NULL);
    if (!bridge_real) return NULL;
    proc = GetProcAddress(bridge_real, name);
    if (!proc) bridge_log("the real d3d11.dll has no %s", name);
    return proc;
}

/* GetProcAddress returns a generic FARPROC; go through void * so the
 * conversion to the real prototype is explicit. */
#define BRIDGE_REAL(type, name) ((type)(void *)bridge_real_proc(name))

static BOOL bridge_first_time(const char *what, LONG *counter)
{
    LONG n = InterlockedIncrement(counter);
    if (n <= 3) return TRUE;
    if (n == 4) bridge_log("%s: further calls not logged", what);
    return FALSE;
}

/* ---- CheckFeatureSupport guard ------------------------------------------ */

/* Queries whose all-zero answer would be wrong rather than "not supported":
 * their E_INVALIDARG says something about the caller's input (an unknown
 * format, a shader model or root signature version newer than the runtime
 * knows, a node index, a list) and the caller is expected to react to it,
 * as on Windows. Numbers are D3D12_FEATURE values. */
static BOOL bridge_feature_keeps_errors(UINT feature)
{
    switch (feature)
    {
    case 1:   /* ARCHITECTURE (NodeIndex) */
    case 2:   /* FEATURE_LEVELS (the caller's list; zero is not a level) */
    case 3:   /* FORMAT_SUPPORT (Format) */
    case 4:   /* MULTISAMPLE_QUALITY_LEVELS (Format, SampleCount) */
    case 5:   /* FORMAT_INFO (Format; zero planes is not an answer) */
    case 7:   /* SHADER_MODEL (the caller lowers its request and asks again) */
    case 12:  /* ROOT_SIGNATURE (likewise) */
    case 16:  /* ARCHITECTURE1 (NodeIndex) */
    case 31:  /* QUERY_META_COMMAND (caller's buffers) */
    case 34:  /* PROTECTED_RESOURCE_SESSION_TYPES (caller's list) */
        return TRUE;
    }
    return FALSE;
}

static void bridge_note_feature(UINT feature, UINT size)
{
    LONG i, n = bridge_nnoted;

    for (i = 0; i < n && i < 64; i++) if ((UINT)bridge_noted_features[i] == feature) return;
    n = InterlockedIncrement(&bridge_nnoted) - 1;
    if (n < 64) bridge_noted_features[n] = (LONG)feature;
    bridge_log("CheckFeatureSupport(feature %u, %u bytes): the D3D12 layer does not know it; "
               "answered \"not supported\"", feature, size);
}

static bridge_cfs_fn bridge_real_cfs_for(void **table)
{
    LONG i, n = bridge_ntables;

    for (i = 0; i < n; i++) if (bridge_tables[i].table == table) return bridge_tables[i].real;
    return NULL;
}

static HRESULT STDMETHODCALLTYPE bridge_check_feature_support(ID3D12Device *device, D3D12_FEATURE feature,
                                                              void *data, UINT size)
{
    bridge_cfs_fn real = bridge_real_cfs_for(*(void ***)device);
    HRESULT hr;

    if (!real) return E_INVALIDARG;   /* only installed in tables listed above */
    hr = real(device, feature, data, size);
    if (hr == E_INVALIDARG && data && size && size <= BRIDGE_MAX_GUARDED_SIZE &&
        !bridge_feature_keeps_errors((UINT)feature))
    {
        memset(data, 0, size);
        bridge_note_feature((UINT)feature, size);
        return S_OK;
    }
    return hr;
}

static void bridge_guard_table(void **table)
{
    DWORD old_protect;
    bridge_cfs_fn real;
    LONG i, n;

    AcquireSRWLockExclusive(&bridge_table_lock);
    n = bridge_ntables;
    for (i = 0; i < n; i++) if (bridge_tables[i].table == table) break;
    if (i < n || table[BRIDGE_CFS_SLOT] == (void *)bridge_check_feature_support)
    {
        ReleaseSRWLockExclusive(&bridge_table_lock);
        return;
    }
    if (n >= BRIDGE_MAX_TABLES)
    {
        ReleaseSRWLockExclusive(&bridge_table_lock);
        bridge_log("too many D3D12 device tables; table %p left unguarded", (void *)table);
        return;
    }
    if (!VirtualProtect(&table[BRIDGE_CFS_SLOT], sizeof(void *), PAGE_READWRITE, &old_protect))
    {
        DWORD error = GetLastError();
        ReleaseSRWLockExclusive(&bridge_table_lock);
        bridge_log("VirtualProtect on table %p failed (error %lu); guard not installed",
                   (void *)table, error);
        return;
    }
    real = (bridge_cfs_fn)table[BRIDGE_CFS_SLOT];
    bridge_tables[n].table = table;
    bridge_tables[n].real = real;
    MemoryBarrier();
    bridge_ntables = n + 1;   /* published before the hook can be reached */
    MemoryBarrier();
    table[BRIDGE_CFS_SLOT] = (void *)bridge_check_feature_support;
    VirtualProtect(&table[BRIDGE_CFS_SLOT], sizeof(void *), old_protect, &old_protect);
    ReleaseSRWLockExclusive(&bridge_table_lock);
    bridge_log("feature-query guard installed on D3D12 device table %p (real CheckFeatureSupport %p)",
               (void *)table, (void *)real);
}

static void bridge_guard_device(IUnknown *unknown)
{
    ID3D12Device *device = NULL;

    if (!unknown)
    {
        bridge_log("D3D11On12CreateDevice got no D3D12 device");
        return;
    }
    if (FAILED(IUnknown_QueryInterface(unknown, &bridge_iid_d3d12_device, (void **)&device)) || !device)
    {
        bridge_log("the object passed to D3D11On12CreateDevice is not an ID3D12Device");
        return;
    }
    /* Unity calls through the interface it passed, which may be a later
     * ID3D12DeviceN with its own table; guard that one and ID3D12Device's. */
    bridge_guard_table(*(void ***)unknown);
    if ((void *)device != (void *)unknown) bridge_guard_table(*(void ***)device);
    ID3D12Device_Release(device);
}

/* ---- exports --------------------------------------------------------------- */

HRESULT WINAPI bridge_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
                                        UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count,
                                        UINT sdk_version, ID3D11Device **device,
                                        D3D_FEATURE_LEVEL *chosen_level, ID3D11DeviceContext **context)
{
    static LONG calls;
    PFN_D3D11_CREATE_DEVICE real = BRIDGE_REAL(PFN_D3D11_CREATE_DEVICE, "D3D11CreateDevice");
    HRESULT hr;

    if (!real) return E_FAIL;
    hr = real(adapter, driver_type, software, flags, levels, level_count, sdk_version,
              device, chosen_level, context);
    if (bridge_first_time("D3D11CreateDevice", &calls))
        bridge_log("D3D11CreateDevice(driver type %d, flags %#x, %u levels) -> %#lx",
                   (int)driver_type, flags, level_count, (unsigned long)hr);
    return hr;
}

HRESULT WINAPI bridge_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type,
                                                    HMODULE software, UINT flags,
                                                    const D3D_FEATURE_LEVEL *levels, UINT level_count,
                                                    UINT sdk_version, const DXGI_SWAP_CHAIN_DESC *desc,
                                                    IDXGISwapChain **swap_chain, ID3D11Device **device,
                                                    D3D_FEATURE_LEVEL *chosen_level,
                                                    ID3D11DeviceContext **context)
{
    static LONG calls;
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN real =
        BRIDGE_REAL(PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN, "D3D11CreateDeviceAndSwapChain");
    HRESULT hr;

    if (!real) return E_FAIL;
    hr = real(adapter, driver_type, software, flags, levels, level_count, sdk_version, desc,
              swap_chain, device, chosen_level, context);
    if (bridge_first_time("D3D11CreateDeviceAndSwapChain", &calls))
        bridge_log("D3D11CreateDeviceAndSwapChain(flags %#x) -> %#lx", flags, (unsigned long)hr);
    return hr;
}

HRESULT WINAPI bridge_D3D11On12CreateDevice(IUnknown *d3d12_device, UINT flags, const D3D_FEATURE_LEVEL *levels,
                                            UINT level_count, IUnknown *const *queues, UINT queue_count,
                                            UINT node_mask, ID3D11Device **device,
                                            ID3D11DeviceContext **context, D3D_FEATURE_LEVEL *chosen_level)
{
    bridge_on12_fn real_on12 = BRIDGE_REAL(bridge_on12_fn, "D3D11On12CreateDevice");
    PFN_D3D11_CREATE_DEVICE create = BRIDGE_REAL(PFN_D3D11_CREATE_DEVICE, "D3D11CreateDevice");
    UINT plain_flags = flags & ~(UINT)D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    char wanted[96] = "";
    HRESULT hr = E_NOTIMPL;
    UINT i;

    for (i = 0; levels && i < level_count && i < 8; i++)
    {
        size_t len = strlen(wanted);
        snprintf(wanted + len, sizeof(wanted) - len, "%s%#x", i ? " " : "", (unsigned)levels[i]);
    }
    bridge_log("D3D11On12CreateDevice(d3d12 device %p, flags %#x, levels [%s], %u queues, node %#x)",
               (void *)d3d12_device, flags, wanted, queue_count, node_mask);
    bridge_guard_device(d3d12_device);

    if (real_on12)
    {
        hr = real_on12(d3d12_device, flags, levels, level_count, queues, queue_count, node_mask,
                       device, context, chosen_level);
        if (SUCCEEDED(hr))
        {
            bridge_log("the real D3D11On12CreateDevice succeeded; using it");
            return hr;
        }
        bridge_log("the real D3D11On12CreateDevice failed (%#lx); making a standalone D3D11 device",
                   (unsigned long)hr);
    }
    if (!create) return hr;

    hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, plain_flags, levels, level_count,
                D3D11_SDK_VERSION, device, &got, context);
    if (FAILED(hr) && levels)
    {
        bridge_log("standalone device with the requested levels failed (%#lx); retrying with the defaults",
                   (unsigned long)hr);
        hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, plain_flags, NULL, 0,
                    D3D11_SDK_VERSION, device, &got, context);
    }
    if (FAILED(hr) && plain_flags != (plain_flags & D3D11_CREATE_DEVICE_BGRA_SUPPORT))
    {
        bridge_log("standalone device with flags %#x failed (%#lx); retrying with BGRA support only",
                   plain_flags, (unsigned long)hr);
        hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, plain_flags & D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    NULL, 0, D3D11_SDK_VERSION, device, &got, context);
    }
    if (chosen_level) *chosen_level = got;
    bridge_log("standalone D3D11 device for D3D11On12CreateDevice -> %#lx (feature level %#x)",
               (unsigned long)hr, (unsigned)got);
    return hr;
}

HRESULT WINAPI bridge_D3D11CoreCreateDevice(IDXGIFactory *factory, IDXGIAdapter *adapter, UINT flags,
                                            const D3D_FEATURE_LEVEL *levels, UINT level_count,
                                            ID3D11Device **device)
{
    bridge_core_create_fn real = BRIDGE_REAL(bridge_core_create_fn, "D3D11CoreCreateDevice");

    if (!real) return E_NOTIMPL;
    return real(factory, adapter, flags, levels, level_count, device);
}

HRESULT WINAPI bridge_CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice *dxgi_device, IUnknown **graphics_device)
{
    bridge_from_dxgi_device_fn real =
        BRIDGE_REAL(bridge_from_dxgi_device_fn, "CreateDirect3D11DeviceFromDXGIDevice");

    if (!real) return E_NOTIMPL;
    return real(dxgi_device, graphics_device);
}

HRESULT WINAPI bridge_CreateDirect3D11SurfaceFromDXGISurface(IDXGISurface *dxgi_surface, IUnknown **graphics_surface)
{
    bridge_from_dxgi_surface_fn real =
        BRIDGE_REAL(bridge_from_dxgi_surface_fn, "CreateDirect3D11SurfaceFromDXGISurface");

    if (!real) return E_NOTIMPL;
    return real(dxgi_surface, graphics_surface);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        bridge_self = instance;
        DisableThreadLibraryCalls(instance);
        /* Tells "the bridge was loaded but Unity never asked for 11-on-12"
         * apart from "the bridge was never loaded". */
        bridge_log("sr2-d3d12-bridge %s loaded", BRIDGE_VERSION);
    }
    return TRUE;
}

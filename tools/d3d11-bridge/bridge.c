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
 *   - D3D11CreateDevice, D3D11CreateDeviceAndSwapChain and D3D11CoreCreateDevice
 *     go straight to the real d3d11.dll. The bridge exports exactly what DXMT's
 *     d3d11.dll exports, so a program that probes for an export sees the same
 *     answer with or without it.
 *   - D3D11On12CreateDevice first asks the real d3d11.dll. When that cannot make
 *     an 11-on-12 device, it returns a standalone D3D11 device instead. Unity
 *     only uses the 11-on-12 device briefly while it starts.
 *   - The D3D12 device passed to D3D11On12CreateDevice gets a few of its calls
 *     wrapped:
 *       CheckFeatureSupport: a capability query the D3D12 layer does not know
 *         (E_INVALIDARG) is answered "not supported" (S_OK, zeroed data):
 *         Unity 6 treats E_INVALIDARG for D3D12_FEATURE_D3D12_TIGHT_ALIGNMENT
 *         as fatal. Queries whose E_INVALIDARG is a real answer about the
 *         caller's input (formats, shader model, root signature version,
 *         feature level list) are left alone, because a zeroed answer to those
 *         would be wrong.
 *       CreateFence: madeira-d3d12 refuses fences with flags (shared,
 *         cross-adapter, non-monitored) with E_NOTIMPL and no log line. Such a
 *         fence is made without the flags instead; it works the same inside
 *         the process and only cannot be shared.
 *       The other object-creating calls (heaps, resources, descriptor heaps,
 *         root signatures, command allocators and lists, pipeline states,
 *         query heaps, command signatures, shared handles, residency) are
 *         passed through unchanged, and a failure is written to the log with
 *         its arguments. Unity stops with "d3d12: Device failed error (...)"
 *         and "Unrecoverable GPU device error!" when one of them fails, and
 *         madeira-d3d12 does not log every refusal, so this names the call.
 *
 *     The bridge never writes to the D3D12 layer's memory. It copies the
 *     device's function table into this DLL, replaces those calls in the copy
 *     and points the device object at the copy. Patching the table in place
 *     does not work in Madeira: madeira-d3d12 runs from a copy of its DLL in
 *     Madeira's JIT pool, and changing the protection of a page there
 *     (VirtualProtect) left the whole table reading as zeros, so the next call
 *     through it jumped to address 0 (version 2 of this bridge).
 *     Setting SR2_BRIDGE_GUARD=0 in the environment leaves the device alone.
 *
 * Where the real d3d11.dll is
 *   The system directory first, as Windows and Wine search. Madeira runs x64
 *   games next to ARM64 system DLLs: there, system32 holds the ARM64 build,
 *   which an x64 process cannot load, and the x64-compatible (ARM64EC) build is
 *   in C:\windows\sysx64, where Madeira's own loader looks next. The bridge
 *   follows the same order and never takes itself or a DLL without
 *   D3D11CreateDevice.
 *
 * Log
 *   Everything it does is written to C:\sr2-d3d12-bridge.log (at most a few
 *   hundred lines per run) and to the debugger output (OutputDebugStringA),
 *   which Madeira copies into its own log, so "Get Help" files include it.
 *   Delete the DLL from the game folder to undo everything.
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

#define BRIDGE_VERSION "4"
#define BRIDGE_LOG_PATH "C:\\sr2-d3d12-bridge.log"
#define BRIDGE_LOG_MAX_LINES 300
#define BRIDGE_MAX_TABLES 8
#define BRIDGE_MAX_GUARDED_SIZE 4096

/* Slots copied at least: madeira-d3d12's device table is an ID3D12Device10
 * table (79 slots) while it answers QueryInterface only up to ID3D12Device8. */
#define BRIDGE_TABLE_MIN 79
#define BRIDGE_TABLE_MAX 104   /* ID3D12Device16 */

/* Slots in every ID3D12Device* function table, from Microsoft's d3d12.h
 * (DirectX-Headers). The ID3D12Device and ID3D12Device1 ones are checked
 * against MinGW's header below. */
enum
{
    SLOT_CreateCommandQueue = 8,
    SLOT_CreateCommandAllocator = 9,
    SLOT_CreateGraphicsPipelineState = 10,
    SLOT_CreateComputePipelineState = 11,
    SLOT_CreateCommandList = 12,
    SLOT_CheckFeatureSupport = 13,
    SLOT_CreateDescriptorHeap = 14,
    SLOT_CreateRootSignature = 16,
    SLOT_CreateCommittedResource = 27,
    SLOT_CreateHeap = 28,
    SLOT_CreatePlacedResource = 29,
    SLOT_CreateReservedResource = 30,
    SLOT_CreateSharedHandle = 31,
    SLOT_OpenSharedHandle = 32,
    SLOT_MakeResident = 34,
    SLOT_Evict = 35,
    SLOT_CreateFence = 36,
    SLOT_GetDeviceRemovedReason = 37,
    SLOT_CreateQueryHeap = 39,
    SLOT_CreateCommandSignature = 41,
    SLOT_CreatePipelineLibrary = 44,      /* ID3D12Device1 */
    SLOT_CreatePipelineState = 47,        /* ID3D12Device2 */
    SLOT_CreateCommandList1 = 51,         /* ID3D12Device4 */
    SLOT_CreateCommittedResource1 = 53,
    SLOT_CreateHeap1 = 54,
    SLOT_CreateCommittedResource2 = 69,   /* ID3D12Device8 */
    SLOT_CreatePlacedResource1 = 70,
};
#define BRIDGE_SLOT_OF(method) (offsetof(ID3D12Device1Vtbl, method) / sizeof(void *))
_Static_assert(BRIDGE_SLOT_OF(CreateCommandQueue) == SLOT_CreateCommandQueue, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateCommandAllocator) == SLOT_CreateCommandAllocator, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateGraphicsPipelineState) == SLOT_CreateGraphicsPipelineState, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateComputePipelineState) == SLOT_CreateComputePipelineState, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateCommandList) == SLOT_CreateCommandList, "slot");
_Static_assert(BRIDGE_SLOT_OF(CheckFeatureSupport) == SLOT_CheckFeatureSupport, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateDescriptorHeap) == SLOT_CreateDescriptorHeap, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateRootSignature) == SLOT_CreateRootSignature, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateCommittedResource) == SLOT_CreateCommittedResource, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateHeap) == SLOT_CreateHeap, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreatePlacedResource) == SLOT_CreatePlacedResource, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateReservedResource) == SLOT_CreateReservedResource, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateSharedHandle) == SLOT_CreateSharedHandle, "slot");
_Static_assert(BRIDGE_SLOT_OF(OpenSharedHandle) == SLOT_OpenSharedHandle, "slot");
_Static_assert(BRIDGE_SLOT_OF(MakeResident) == SLOT_MakeResident, "slot");
_Static_assert(BRIDGE_SLOT_OF(Evict) == SLOT_Evict, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateFence) == SLOT_CreateFence, "slot");
_Static_assert(BRIDGE_SLOT_OF(GetDeviceRemovedReason) == SLOT_GetDeviceRemovedReason, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateQueryHeap) == SLOT_CreateQueryHeap, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreateCommandSignature) == SLOT_CreateCommandSignature, "slot");
_Static_assert(BRIDGE_SLOT_OF(CreatePipelineLibrary) == SLOT_CreatePipelineLibrary, "slot");

/* ID3D12Device and its later versions with the size of each one's function
 * table, from Microsoft's DirectX-Headers (d3d12.h), written out so no import
 * library is needed. */
static const struct { GUID iid; UINT slots; } bridge_device_versions[] =
{
    { { 0x189819f1, 0x1db6, 0x4b57, { 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7 } }, 44 },
    { { 0x77acce80, 0x638e, 0x4e65, { 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e } }, 47 },
    { { 0x30baa41e, 0xb15b, 0x475c, { 0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28 } }, 48 },
    { { 0x81dadc15, 0x2bad, 0x4392, { 0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98 } }, 51 },
    { { 0xe865df17, 0xa9ee, 0x46f9, { 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5 } }, 57 },
    { { 0x8b4f173b, 0x2fea, 0x4b80, { 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d } }, 65 },
    { { 0xc70b221b, 0x40e4, 0x4a17, { 0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc } }, 66 },
    { { 0x5c014b53, 0x68a1, 0x4b9b, { 0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b } }, 68 },
    { { 0x9218e6bb, 0xf944, 0x4f7e, { 0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3 } }, 73 },
    { { 0x4c80e962, 0xf032, 0x4f60, { 0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c } }, 76 },
    { { 0x517f8718, 0xaa66, 0x49f9, { 0xb0, 0x2b, 0xa7, 0xab, 0x89, 0xc0, 0x60, 0x31 } }, 79 },
    { { 0x5405c344, 0xd457, 0x444e, { 0xb4, 0xdd, 0x23, 0x66, 0xe4, 0x5a, 0xee, 0x39 } }, 80 },
    { { 0x5af5c532, 0x4c91, 0x4cd0, { 0xb5, 0x41, 0x15, 0xa4, 0x05, 0x39, 0x5f, 0xc5 } }, 81 },
    { { 0x14eecffc, 0x4df8, 0x40f7, { 0xa1, 0x18, 0x5c, 0x81, 0x6f, 0x45, 0x69, 0x5e } }, 82 },
    { { 0x5f6e592d, 0xd895, 0x44c2, { 0x8e, 0x4a, 0x88, 0xad, 0x49, 0x26, 0xd3, 0x23 } }, 83 },
    { { 0x76cff76f, 0x1e9b, 0x4450, { 0x8c, 0xdc, 0x34, 0xf1, 0xaf, 0x78, 0x8e, 0x5b } }, 94 },
    { { 0xb34a50dd, 0xe102, 0x428f, { 0x82, 0xa0, 0x82, 0x29, 0x4a, 0x27, 0xbb, 0xe6 } }, 104 },
};
#define bridge_iid_d3d12_device (bridge_device_versions[0].iid)

typedef HRESULT (WINAPI *bridge_on12_fn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                         IUnknown *const *, UINT, UINT, ID3D11Device **,
                                         ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);
typedef HRESULT (WINAPI *bridge_core_create_fn)(IDXGIFactory *, IDXGIAdapter *, UINT,
                                                const D3D_FEATURE_LEVEL *, UINT, ID3D11Device **);

static HMODULE bridge_self;
static HMODULE bridge_real;
static INIT_ONCE bridge_once = INIT_ONCE_STATIC_INIT;

static SRWLOCK bridge_log_lock = SRWLOCK_INIT;
static LONG bridge_log_lines;

/* A D3D12 device table this bridge copied: the implementation's own table is
 * only read, never written. */
struct bridge_table
{
    void **original;
    UINT count;                          /* slots copied */
    void *real_slots[BRIDGE_TABLE_MAX];  /* the implementation's functions, as copied */
    void *slots[BRIDGE_TABLE_MAX];       /* the table the device object points at */
};
static SRWLOCK bridge_table_lock = SRWLOCK_INIT;
static struct bridge_table bridge_tables[BRIDGE_MAX_TABLES];
static volatile LONG bridge_ntables;

static volatile LONG bridge_noted_features[64];
static volatile LONG bridge_nnoted;

static void bridge_log_v(BOOL to_debugger, const char *fmt, va_list args)
{
    char message[600], line[700];
    SYSTEMTIME now;
    int used;
    HANDLE file;
    DWORD written;

    if (InterlockedIncrement(&bridge_log_lines) > BRIDGE_LOG_MAX_LINES) return;
    if (vsnprintf(message, sizeof(message), fmt, args) < 0) return;

    GetLocalTime(&now);
    used = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [pid %lu tid %lu] %s\r\n",
                    now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                    GetCurrentProcessId(), GetCurrentThreadId(), message);
    if (used < 0) return;
    if (used >= (int)sizeof(line))
    {
        used = sizeof(line) - 1;
        line[used - 2] = '\r';
        line[used - 1] = '\n';
    }

    AcquireSRWLockExclusive(&bridge_log_lock);
    file = CreateFileA(BRIDGE_LOG_PATH, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        WriteFile(file, line, (DWORD)used, &written, NULL);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&bridge_log_lock);

    if (to_debugger)
    {
        snprintf(line, sizeof(line), "[sr2-d3d12-bridge] %s\n", message);
        OutputDebugStringA(line);
    }
}

static void bridge_log(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    bridge_log_v(TRUE, fmt, args);
    va_end(args);
}

/* For DllMain: the file only. */
static void bridge_log_quiet(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    bridge_log_v(FALSE, fmt, args);
    va_end(args);
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

/* ---- the copied D3D12 device table -------------------------------------- */

static struct bridge_table *bridge_table_of(void **slots)
{
    LONG i, n = bridge_ntables;

    for (i = 0; i < n; i++) if (bridge_tables[i].slots == slots) return &bridge_tables[i];
    return NULL;
}

/* The implementation's own function for a slot of this object's table. */
static void *bridge_real_slot(void *object, UINT slot)
{
    struct bridge_table *table = bridge_table_of(*(void ***)object);

    return table && slot < table->count ? table->real_slots[slot] : NULL;
}

/* Declares `real` (the implementation's function) and `hr`, and calls it. */
#define BRIDGE_FORWARD(slot, fn_type, call_args)                         \
    fn_type real = (fn_type)bridge_real_slot((void *)device, (slot));    \
    HRESULT hr;                                                          \
    if (!real) return E_UNEXPECTED;                                      \
    hr = real call_args;

/* At most `limit` lines for each kind of call. */
static BOOL bridge_count(LONG *counter, LONG limit)
{
    return InterlockedIncrement(counter) <= limit;
}

static const char *bridge_desc(char *out, size_t size, const D3D12_RESOURCE_DESC *desc)
{
    if (!desc)
        snprintf(out, size, "no description");
    else
        snprintf(out, size, "dimension %d, %llux%ux%u, %u mips, format %d, %u samples, layout %d, flags %#x",
                 (int)desc->Dimension, (unsigned long long)desc->Width, desc->Height,
                 (unsigned)desc->DepthOrArraySize, (unsigned)desc->MipLevels, (int)desc->Format,
                 desc->SampleDesc.Count, (int)desc->Layout, (unsigned)desc->Flags);
    return out;
}

/* ---- CheckFeatureSupport ---- */

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

typedef HRESULT (STDMETHODCALLTYPE *bridge_cfs_fn)(ID3D12Device *, D3D12_FEATURE, void *, UINT);
static HRESULT STDMETHODCALLTYPE bridge_CheckFeatureSupport(ID3D12Device *device, D3D12_FEATURE feature,
                                                            void *data, UINT size)
{
    BRIDGE_FORWARD(SLOT_CheckFeatureSupport, bridge_cfs_fn, (device, feature, data, size))
    if (hr == E_INVALIDARG && data && size && size <= BRIDGE_MAX_GUARDED_SIZE &&
        !bridge_feature_keeps_errors((UINT)feature))
    {
        memset(data, 0, size);
        bridge_note_feature((UINT)feature, size);
        return S_OK;
    }
    return hr;
}

/* ---- CreateFence ---- */

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_fence_fn)(ID3D12Device *, UINT64, D3D12_FENCE_FLAGS,
                                                            REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateFence(ID3D12Device *device, UINT64 initial, D3D12_FENCE_FLAGS flags,
                                                    REFIID riid, void **out)
{
    static LONG fails, retries;
    BRIDGE_FORWARD(SLOT_CreateFence, bridge_create_fence_fn, (device, initial, flags, riid, out))
    if (hr == E_NOTIMPL && flags != D3D12_FENCE_FLAG_NONE)
    {
        HRESULT plain = real(device, initial, D3D12_FENCE_FLAG_NONE, riid, out);
        if (bridge_count(&retries, 6))
            bridge_log("CreateFence(flags %#x) was refused (E_NOTIMPL): made an ordinary fence instead -> %#lx",
                       (unsigned)flags, (unsigned long)plain);
        return plain;
    }
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateFence(initial %llu, flags %#x) -> %#lx",
                   (unsigned long long)initial, (unsigned)flags, (unsigned long)hr);
    return hr;
}

/* ---- calls that are only watched ---- */

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_queue_fn)(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *,
                                                            REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateCommandQueue(ID3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
                                                           REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateCommandQueue, bridge_create_queue_fn, (device, desc, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateCommandQueue(type %d, flags %#x) -> %#lx", desc ? (int)desc->Type : -1,
                   desc ? (unsigned)desc->Flags : 0u, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_allocator_fn)(ID3D12Device *, D3D12_COMMAND_LIST_TYPE, REFIID,
                                                                void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateCommandAllocator(ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type,
                                                               REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateCommandAllocator, bridge_create_allocator_fn, (device, type, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateCommandAllocator(type %d) -> %#lx", (int)type, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_graphics_pso_fn)(ID3D12Device *,
                                                                   const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                                   REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateGraphicsPipelineState(ID3D12Device *device,
                                                                    const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                                    REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateGraphicsPipelineState, bridge_create_graphics_pso_fn, (device, desc, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreateGraphicsPipelineState(VS %lu bytes, PS %lu bytes, %u targets) -> %#lx",
                   desc ? (unsigned long)desc->VS.BytecodeLength : 0ul,
                   desc ? (unsigned long)desc->PS.BytecodeLength : 0ul,
                   desc ? desc->NumRenderTargets : 0u, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_compute_pso_fn)(ID3D12Device *,
                                                                  const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
                                                                  REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateComputePipelineState(ID3D12Device *device,
                                                                   const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                                   REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateComputePipelineState, bridge_create_compute_pso_fn, (device, desc, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreateComputePipelineState(CS %lu bytes) -> %#lx",
                   desc ? (unsigned long)desc->CS.BytecodeLength : 0ul, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_list_fn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE,
                                                           ID3D12CommandAllocator *, ID3D12PipelineState *,
                                                           REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateCommandList(ID3D12Device *device, UINT node, D3D12_COMMAND_LIST_TYPE type,
                                                          ID3D12CommandAllocator *allocator, ID3D12PipelineState *pso,
                                                          REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateCommandList, bridge_create_list_fn, (device, node, type, allocator, pso, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateCommandList(type %d) -> %#lx", (int)type, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_descriptor_heap_fn)(ID3D12Device *,
                                                                      const D3D12_DESCRIPTOR_HEAP_DESC *,
                                                                      REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateDescriptorHeap(ID3D12Device *device, const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                                             REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateDescriptorHeap, bridge_create_descriptor_heap_fn, (device, desc, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateDescriptorHeap(type %d, %u descriptors, flags %#x) -> %#lx",
                   desc ? (int)desc->Type : -1, desc ? desc->NumDescriptors : 0u,
                   desc ? (unsigned)desc->Flags : 0u, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_root_signature_fn)(ID3D12Device *, UINT, const void *, SIZE_T,
                                                                     REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateRootSignature(ID3D12Device *device, UINT node, const void *blob,
                                                            SIZE_T size, REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateRootSignature, bridge_create_root_signature_fn, (device, node, blob, size, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateRootSignature(%lu bytes) -> %#lx", (unsigned long)size, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_committed_fn)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
                                                                D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                                                D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                                                REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateCommittedResource(ID3D12Device *device, const D3D12_HEAP_PROPERTIES *props,
                                                                D3D12_HEAP_FLAGS heap_flags,
                                                                const D3D12_RESOURCE_DESC *desc,
                                                                D3D12_RESOURCE_STATES state,
                                                                const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    static LONG fails;
    char text[200];
    BRIDGE_FORWARD(SLOT_CreateCommittedResource, bridge_create_committed_fn,
                   (device, props, heap_flags, desc, state, clear, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreateCommittedResource(heap type %d, heap flags %#x, %s, state %#x) -> %#lx",
                   props ? (int)props->Type : -1, (unsigned)heap_flags, bridge_desc(text, sizeof(text), desc),
                   (unsigned)state, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_heap_fn)(ID3D12Device *, const D3D12_HEAP_DESC *, REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateHeap(ID3D12Device *device, const D3D12_HEAP_DESC *desc, REFIID riid,
                                                   void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateHeap, bridge_create_heap_fn, (device, desc, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateHeap(%llu bytes, heap type %d, alignment %llu, flags %#x) -> %#lx",
                   desc ? (unsigned long long)desc->SizeInBytes : 0ull, desc ? (int)desc->Properties.Type : -1,
                   desc ? (unsigned long long)desc->Alignment : 0ull, desc ? (unsigned)desc->Flags : 0u,
                   (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_placed_fn)(ID3D12Device *, ID3D12Heap *, UINT64,
                                                             const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                                             const D3D12_CLEAR_VALUE *, REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreatePlacedResource(ID3D12Device *device, ID3D12Heap *heap, UINT64 offset,
                                                             const D3D12_RESOURCE_DESC *desc,
                                                             D3D12_RESOURCE_STATES state,
                                                             const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    static LONG fails;
    char text[200];
    BRIDGE_FORWARD(SLOT_CreatePlacedResource, bridge_create_placed_fn,
                   (device, heap, offset, desc, state, clear, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreatePlacedResource(offset %llu, %s, state %#x) -> %#lx", (unsigned long long)offset,
                   bridge_desc(text, sizeof(text), desc), (unsigned)state, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_reserved_fn)(ID3D12Device *, const D3D12_RESOURCE_DESC *,
                                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                                               REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateReservedResource(ID3D12Device *device, const D3D12_RESOURCE_DESC *desc,
                                                               D3D12_RESOURCE_STATES state,
                                                               const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    static LONG fails;
    char text[200];
    BRIDGE_FORWARD(SLOT_CreateReservedResource, bridge_create_reserved_fn, (device, desc, state, clear, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateReservedResource(%s) -> %#lx", bridge_desc(text, sizeof(text), desc),
                   (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_shared_handle_fn)(ID3D12Device *, ID3D12DeviceChild *,
                                                                    const SECURITY_ATTRIBUTES *, DWORD,
                                                                    const WCHAR *, HANDLE *);
static HRESULT STDMETHODCALLTYPE bridge_CreateSharedHandle(ID3D12Device *device, ID3D12DeviceChild *object,
                                                           const SECURITY_ATTRIBUTES *attributes, DWORD access,
                                                           const WCHAR *name, HANDLE *handle)
{
    static LONG calls;
    BRIDGE_FORWARD(SLOT_CreateSharedHandle, bridge_create_shared_handle_fn,
                   (device, object, attributes, access, name, handle))
    if (bridge_count(&calls, 6))
        bridge_log("CreateSharedHandle(object %p, access %#lx, %s) -> %#lx", (void *)object, (unsigned long)access,
                   name ? "named" : "unnamed", (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_open_shared_handle_fn)(ID3D12Device *, HANDLE, REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_OpenSharedHandle(ID3D12Device *device, HANDLE handle, REFIID riid, void **out)
{
    static LONG calls;
    BRIDGE_FORWARD(SLOT_OpenSharedHandle, bridge_open_shared_handle_fn, (device, handle, riid, out))
    if (bridge_count(&calls, 6))
        bridge_log("OpenSharedHandle(%p) -> %#lx", (void *)handle, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_residency_fn)(ID3D12Device *, UINT, ID3D12Pageable *const *);
static HRESULT STDMETHODCALLTYPE bridge_MakeResident(ID3D12Device *device, UINT count, ID3D12Pageable *const *objects)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_MakeResident, bridge_residency_fn, (device, count, objects))
    if (FAILED(hr) && bridge_count(&fails, 4))
        bridge_log("FAILED MakeResident(%u objects) -> %#lx", count, (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE bridge_Evict(ID3D12Device *device, UINT count, ID3D12Pageable *const *objects)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_Evict, bridge_residency_fn, (device, count, objects))
    if (FAILED(hr) && bridge_count(&fails, 4))
        bridge_log("FAILED Evict(%u objects) -> %#lx", count, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_removed_reason_fn)(ID3D12Device *);
static HRESULT STDMETHODCALLTYPE bridge_GetDeviceRemovedReason(ID3D12Device *device)
{
    static LONG said;
    BRIDGE_FORWARD(SLOT_GetDeviceRemovedReason, bridge_removed_reason_fn, (device))
    if (hr != S_OK && bridge_count(&said, 3))
        bridge_log("GetDeviceRemovedReason -> %#lx", (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_query_heap_fn)(ID3D12Device *, const D3D12_QUERY_HEAP_DESC *,
                                                                 REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateQueryHeap(ID3D12Device *device, const D3D12_QUERY_HEAP_DESC *desc,
                                                        REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateQueryHeap, bridge_create_query_heap_fn, (device, desc, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateQueryHeap(type %d, %u queries) -> %#lx", desc ? (int)desc->Type : -1,
                   desc ? desc->Count : 0u, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_command_signature_fn)(ID3D12Device *,
                                                                        const D3D12_COMMAND_SIGNATURE_DESC *,
                                                                        ID3D12RootSignature *, REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateCommandSignature(ID3D12Device *device,
                                                               const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                               ID3D12RootSignature *root_signature,
                                                               REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateCommandSignature, bridge_create_command_signature_fn,
                   (device, desc, root_signature, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateCommandSignature(stride %u, %u arguments, root signature %p) -> %#lx",
                   desc ? desc->ByteStride : 0u, desc ? desc->NumArgumentDescs : 0u, (void *)root_signature,
                   (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_pipeline_library_fn)(ID3D12Device *, const void *, SIZE_T,
                                                                       REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreatePipelineLibrary(ID3D12Device *device, const void *blob, SIZE_T size,
                                                              REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreatePipelineLibrary, bridge_create_pipeline_library_fn, (device, blob, size, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 3))
        bridge_log("FAILED CreatePipelineLibrary(%lu-byte blob) -> %#lx", (unsigned long)size, (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_pipeline_state_fn)(ID3D12Device *, const void *, REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreatePipelineState(ID3D12Device *device, const void *stream, REFIID riid,
                                                            void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreatePipelineState, bridge_create_pipeline_state_fn, (device, stream, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreatePipelineState (stream) -> %#lx", (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_list1_fn)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, UINT,
                                                            REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateCommandList1(ID3D12Device *device, UINT node,
                                                           D3D12_COMMAND_LIST_TYPE type, UINT flags,
                                                           REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateCommandList1, bridge_create_list1_fn, (device, node, type, flags, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateCommandList1(type %d, flags %#x) -> %#lx", (int)type, flags, (unsigned long)hr);
    return hr;
}

/* The protected-session pointer and D3D12_RESOURCE_DESC1 are not in MinGW's
 * header; DESC1 starts with D3D12_RESOURCE_DESC's fields, which is all the
 * log reads. */
typedef HRESULT (STDMETHODCALLTYPE *bridge_create_committed1_fn)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
                                                                 D3D12_HEAP_FLAGS, const void *,
                                                                 D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                                                 void *, REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateCommittedResource1(ID3D12Device *device,
                                                                 const D3D12_HEAP_PROPERTIES *props,
                                                                 D3D12_HEAP_FLAGS heap_flags, const void *desc,
                                                                 D3D12_RESOURCE_STATES state,
                                                                 const D3D12_CLEAR_VALUE *clear, void *session,
                                                                 REFIID riid, void **out)
{
    static LONG fails;
    char text[200];
    BRIDGE_FORWARD(SLOT_CreateCommittedResource1, bridge_create_committed1_fn,
                   (device, props, heap_flags, desc, state, clear, session, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreateCommittedResource1(heap type %d, heap flags %#x, %s, protected %s) -> %#lx",
                   props ? (int)props->Type : -1, (unsigned)heap_flags,
                   bridge_desc(text, sizeof(text), (const D3D12_RESOURCE_DESC *)desc), session ? "yes" : "no",
                   (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_heap1_fn)(ID3D12Device *, const D3D12_HEAP_DESC *, void *,
                                                            REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreateHeap1(ID3D12Device *device, const D3D12_HEAP_DESC *desc, void *session,
                                                    REFIID riid, void **out)
{
    static LONG fails;
    BRIDGE_FORWARD(SLOT_CreateHeap1, bridge_create_heap1_fn, (device, desc, session, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 6))
        bridge_log("FAILED CreateHeap1(%llu bytes, heap type %d, flags %#x, protected %s) -> %#lx",
                   desc ? (unsigned long long)desc->SizeInBytes : 0ull, desc ? (int)desc->Properties.Type : -1,
                   desc ? (unsigned)desc->Flags : 0u, session ? "yes" : "no", (unsigned long)hr);
    return hr;
}

static HRESULT STDMETHODCALLTYPE bridge_CreateCommittedResource2(ID3D12Device *device,
                                                                 const D3D12_HEAP_PROPERTIES *props,
                                                                 D3D12_HEAP_FLAGS heap_flags, const void *desc,
                                                                 D3D12_RESOURCE_STATES state,
                                                                 const D3D12_CLEAR_VALUE *clear, void *session,
                                                                 REFIID riid, void **out)
{
    static LONG fails;
    char text[200];
    BRIDGE_FORWARD(SLOT_CreateCommittedResource2, bridge_create_committed1_fn,
                   (device, props, heap_flags, desc, state, clear, session, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreateCommittedResource2(heap type %d, heap flags %#x, %s, protected %s) -> %#lx",
                   props ? (int)props->Type : -1, (unsigned)heap_flags,
                   bridge_desc(text, sizeof(text), (const D3D12_RESOURCE_DESC *)desc), session ? "yes" : "no",
                   (unsigned long)hr);
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *bridge_create_placed1_fn)(ID3D12Device *, ID3D12Heap *, UINT64, const void *,
                                                              D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
                                                              REFIID, void **);
static HRESULT STDMETHODCALLTYPE bridge_CreatePlacedResource1(ID3D12Device *device, ID3D12Heap *heap, UINT64 offset,
                                                              const void *desc, D3D12_RESOURCE_STATES state,
                                                              const D3D12_CLEAR_VALUE *clear, REFIID riid, void **out)
{
    static LONG fails;
    char text[200];
    BRIDGE_FORWARD(SLOT_CreatePlacedResource1, bridge_create_placed1_fn,
                   (device, heap, offset, desc, state, clear, riid, out))
    if (FAILED(hr) && bridge_count(&fails, 8))
        bridge_log("FAILED CreatePlacedResource1(offset %llu, %s) -> %#lx", (unsigned long long)offset,
                   bridge_desc(text, sizeof(text), (const D3D12_RESOURCE_DESC *)desc), (unsigned long)hr);
    return hr;
}

static const struct { UINT slot; void *wrapper; } bridge_wrappers[] =
{
    { SLOT_CheckFeatureSupport, (void *)bridge_CheckFeatureSupport },
    { SLOT_CreateFence, (void *)bridge_CreateFence },
    { SLOT_CreateCommandQueue, (void *)bridge_CreateCommandQueue },
    { SLOT_CreateCommandAllocator, (void *)bridge_CreateCommandAllocator },
    { SLOT_CreateGraphicsPipelineState, (void *)bridge_CreateGraphicsPipelineState },
    { SLOT_CreateComputePipelineState, (void *)bridge_CreateComputePipelineState },
    { SLOT_CreateCommandList, (void *)bridge_CreateCommandList },
    { SLOT_CreateDescriptorHeap, (void *)bridge_CreateDescriptorHeap },
    { SLOT_CreateRootSignature, (void *)bridge_CreateRootSignature },
    { SLOT_CreateCommittedResource, (void *)bridge_CreateCommittedResource },
    { SLOT_CreateHeap, (void *)bridge_CreateHeap },
    { SLOT_CreatePlacedResource, (void *)bridge_CreatePlacedResource },
    { SLOT_CreateReservedResource, (void *)bridge_CreateReservedResource },
    { SLOT_CreateSharedHandle, (void *)bridge_CreateSharedHandle },
    { SLOT_OpenSharedHandle, (void *)bridge_OpenSharedHandle },
    { SLOT_MakeResident, (void *)bridge_MakeResident },
    { SLOT_Evict, (void *)bridge_Evict },
    { SLOT_GetDeviceRemovedReason, (void *)bridge_GetDeviceRemovedReason },
    { SLOT_CreateQueryHeap, (void *)bridge_CreateQueryHeap },
    { SLOT_CreateCommandSignature, (void *)bridge_CreateCommandSignature },
    { SLOT_CreatePipelineLibrary, (void *)bridge_CreatePipelineLibrary },
    { SLOT_CreatePipelineState, (void *)bridge_CreatePipelineState },
    { SLOT_CreateCommandList1, (void *)bridge_CreateCommandList1 },
    { SLOT_CreateCommittedResource1, (void *)bridge_CreateCommittedResource1 },
    { SLOT_CreateHeap1, (void *)bridge_CreateHeap1 },
    { SLOT_CreateCommittedResource2, (void *)bridge_CreateCommittedResource2 },
    { SLOT_CreatePlacedResource1, (void *)bridge_CreatePlacedResource1 },
};

/* ---- installing the copy ---- */

/* How many slots the object's table has at least: the newest ID3D12Device
 * version it answers for with the same pointer, and never fewer than
 * madeira-d3d12's own table. */
static UINT bridge_table_size(IUnknown *object, int *newest)
{
    UINT i, slots = BRIDGE_TABLE_MIN;

    *newest = -1;
    for (i = 0; i < sizeof(bridge_device_versions) / sizeof(bridge_device_versions[0]); i++)
    {
        void *other = NULL;

        if (FAILED(IUnknown_QueryInterface(object, &bridge_device_versions[i].iid, &other)) || !other) continue;
        if (other == (void *)object)
        {
            *newest = (int)i;
            if (bridge_device_versions[i].slots > slots) slots = bridge_device_versions[i].slots;
        }
        IUnknown_Release((IUnknown *)other);
    }
    return slots;
}

/* Points the object at a copy of its function table with the wrappers above
 * in it. Only the object's own first field (its table pointer, in ordinary
 * heap memory) is written. */
static void bridge_guard_object(IUnknown *object)
{
    void **original = *(void ***)object;
    struct bridge_table *table;
    char name[8] = "";
    int newest;
    UINT count, k, wrapped = 0;
    LONG i, n;

    if (bridge_table_of(original)) return;   /* already guarded */
    count = bridge_table_size(object, &newest);

    AcquireSRWLockExclusive(&bridge_table_lock);
    n = bridge_ntables;
    for (i = 0; i < n; i++) if (bridge_tables[i].original == original && bridge_tables[i].count >= count) break;
    if (i < n) table = &bridge_tables[i];
    else if (n >= BRIDGE_MAX_TABLES) table = NULL;
    else
    {
        table = &bridge_tables[n];
        table->original = original;
        table->count = count;
        memcpy(table->real_slots, original, count * sizeof(void *));
        memcpy(table->slots, original, count * sizeof(void *));
        for (k = 0; k < sizeof(bridge_wrappers) / sizeof(bridge_wrappers[0]); k++)
            if (bridge_wrappers[k].slot < count) table->slots[bridge_wrappers[k].slot] = bridge_wrappers[k].wrapper;
        MemoryBarrier();
        bridge_ntables = n + 1;   /* published before the object can reach the copy */
        MemoryBarrier();
    }
    if (table) InterlockedExchangePointer((void *volatile *)object, table->slots);
    ReleaseSRWLockExclusive(&bridge_table_lock);

    if (!table)
    {
        bridge_log("too many D3D12 device tables; device %p left unguarded", (void *)object);
        return;
    }
    for (k = 0; k < sizeof(bridge_wrappers) / sizeof(bridge_wrappers[0]); k++)
        if (bridge_wrappers[k].slot < table->count) wrapped++;
    if (newest > 0) snprintf(name, sizeof(name), "%d", newest);
    bridge_log("D3D12 device %p: its table %p copied to %p (%u slots; it answers up to ID3D12Device%s); "
               "%u calls wrapped", (void *)object, (void *)original, (void *)table->slots, table->count, name,
               wrapped);
}

static void bridge_guard_device(IUnknown *unknown)
{
    ID3D12Device *device = NULL;
    char value[8];

    if (GetEnvironmentVariableA("SR2_BRIDGE_GUARD", value, sizeof(value)) && value[0] == '0')
    {
        bridge_log("D3D12 device left alone (SR2_BRIDGE_GUARD=0)");
        return;
    }
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
    /* Unity calls through the interface it passed; guard that object and,
     * if QueryInterface returned a different one, that one too. */
    bridge_guard_object(unknown);
    if ((void *)device != (void *)unknown) bridge_guard_object((IUnknown *)device);
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

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        bridge_self = instance;
        DisableThreadLibraryCalls(instance);
        /* Tells "the bridge was loaded but Unity never asked for 11-on-12"
         * apart from "the bridge was never loaded". */
        bridge_log_quiet("sr2-d3d12-bridge %s loaded", BRIDGE_VERSION);
    }
    return TRUE;
}

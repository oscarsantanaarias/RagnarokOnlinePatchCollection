// Forces D3DPRESENT_INTERVAL_IMMEDIATE so Present stops waiting for vblank.
//
// This is where the frame rate ceiling actually lives. Nothing inside the client
// limits it: every Sleep, QueryPerformanceCounter, QueryPerformanceFrequency and
// timeGetTime call site was checked, and none of them paces frames. The render
// thread only burns ~13% of a core while frames take 7 ms, and it spends the
// difference blocked inside Present - which is exactly what vsync looks like.
//
// The interval cannot be changed after the fact: it is part of the presentation
// parameters passed to CreateDeviceEx, and re-applied on every Reset. So both are
// intercepted and the field is overwritten on the way through.
//
// Hooking Direct3DCreate9Ex is what makes this version independent - no address
// from the client is involved at all. The vtable slots are the stable part of the
// COM ABI:
//      IDirect3D9Ex::CreateDeviceEx   index 20  -> +0x50
//      IDirect3DDevice9::Reset        index 16  -> +0x40

#include <windows.h>
#include <d3d9.h>
#include <detours.h>

#include "../patch.h"

#pragma comment(lib, "d3d9.lib")

namespace {

typedef HRESULT (WINAPI* Create9Ex_t)(UINT, IDirect3D9Ex**);
typedef HRESULT (STDMETHODCALLTYPE* CreateDeviceEx_t)(
    IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
typedef HRESULT (STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

Create9Ex_t      o_create9ex      = nullptr;
CreateDeviceEx_t o_createdeviceex = nullptr;
Reset_t          o_reset          = nullptr;

void ForceImmediate(D3DPRESENT_PARAMETERS* pp, const char* where)
{
    if (!pp) return;
    UINT before = pp->PresentationInterval;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    Log("  vsync_off: %s interval 0x%08X -> IMMEDIATE (windowed=%d, %uHz)",
        where, before, pp->Windowed, pp->FullScreen_RefreshRateInHz);
}

// Replaces one pointer in a COM vtable. The vtable is shared by every object of
// the class, so this only has to happen once.
bool HookSlot(void* obj, int byte_offset, void* replacement, void** original)
{
    void** vtbl = *(void***)obj;
    void** slot = vtbl + (byte_offset / sizeof(void*));
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = *slot;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    ForceImmediate(pp, "Reset");
    return o_reset(dev, pp);
}

HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(
    IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
    D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    ForceImmediate(pp, "CreateDeviceEx");

    HRESULT hr = o_createdeviceex(self, adapter, type, focus, flags, pp, mode, out);

    if (SUCCEEDED(hr) && out && *out && !o_reset) {
        if (HookSlot(*out, 0x40, (void*)hkReset, (void**)&o_reset))
            Log("  vsync_off: Reset hooked, mode changes stay uncapped");
    }
    return hr;
}

HRESULT WINAPI hkCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    HRESULT hr = o_create9ex(sdk, out);
    if (SUCCEEDED(hr) && out && *out && !o_createdeviceex) {
        if (HookSlot(*out, 0x50, (void*)hkCreateDeviceEx, (void**)&o_createdeviceex))
            Log("  vsync_off: CreateDeviceEx hooked");
    }
    return hr;
}

bool Locate()
{
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) { Log("  vsync_off: d3d9.dll not loaded"); return false; }

    o_create9ex = (Create9Ex_t)GetProcAddress(d3d9, "Direct3DCreate9Ex");
    if (!o_create9ex) { Log("  vsync_off: Direct3DCreate9Ex not exported"); return false; }

    // Hooked here rather than in Apply(): the client creates its device while it
    // starts up, long before anyone can press the apply key. Waiting would mean
    // the interval is already baked in and only a Reset could change it.
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)o_create9ex, hkCreate9Ex);
    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) { Log("  vsync_off: DetourAttach failed (%ld)", err); return false; }

    Log("  vsync_off: Direct3DCreate9Ex at %p, hooked before device creation", o_create9ex);
    return true;
}

void Apply()
{
    // Nothing to do - the interval is forced as the device is created.
}

} // namespace

Patch g_vsync_off = { "vsync_off", Locate, Apply, nullptr, nullptr, nullptr, false };
REGISTER_PATCH(g_vsync_off);

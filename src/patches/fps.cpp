// Removes the frame rate ceiling.
//
// The ceiling is vsync, and nothing else. The client asks for
// D3DPRESENT_INTERVAL_DEFAULT, which in D3D9 means "sync to vblank", so Present
// blocks until the monitor is ready - 142 fps on a 144 Hz panel.
//
// Everything inside the client was ruled out first, with measurements rather than
// reading:
//   - every Sleep, QueryPerformanceCounter, QueryPerformanceFrequency and
//     timeGetTime call site was catalogued; none paces frames
//   - the one function shaped like a frame pacer (microsecond accumulator, QPC,
//     drift compensation) is never called in game - hooking it and counting gave
//     exactly zero calls
//   - the render thread only used ~13% of a core while frames took 7 ms; the
//     missing 6 ms was blocked inside Present, which no timing API can show
//
// The interval is part of the presentation parameters, so it cannot be changed
// after the fact - it is set at CreateDeviceEx and re-applied on every Reset.
// Both are intercepted and the field is overwritten on the way through.
//
// No client address is involved. Direct3DCreate9Ex comes from GetProcAddress, and
// the two vtable slots are stable COM ABI:
//      IDirect3D9Ex::CreateDeviceEx   index 20  ->  +0x50
//      IDirect3DDevice9::Reset        index 16  ->  +0x40
//
// While we hold the fresh device, the D3D9Ex present queue is also shortened from
// its default of three frames to one. That is a latency win, not a throughput one.

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
    Log("  fps: %s interval 0x%08X -> IMMEDIATE (windowed=%d, %uHz)",
        where, before, pp->Windowed, pp->FullScreen_RefreshRateInHz);
}

// Replaces one pointer in a COM vtable. The vtable is shared by every object of
// the class, so this only has to be done once.
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
    // Without this, going fullscreen or changing resolution re-applies the
    // client's own parameters and vsync comes back.
    ForceImmediate(pp, "Reset");
    return o_reset(dev, pp);
}

HRESULT STDMETHODCALLTYPE hkCreateDeviceEx(
    IDirect3D9Ex* self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
    D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out)
{
    ForceImmediate(pp, "CreateDeviceEx");

    HRESULT hr = o_createdeviceex(self, adapter, type, focus, flags, pp, mode, out);
    if (FAILED(hr) || !out || !*out) return hr;

    if (!o_reset && HookSlot(*out, 0x40, (void*)hkReset, (void**)&o_reset))
        Log("  fps: Reset hooked, mode changes stay uncapped");

    UINT latency = 0;
    if (SUCCEEDED((*out)->GetMaximumFrameLatency(&latency)) &&
        SUCCEEDED((*out)->SetMaximumFrameLatency(1)))
        Log("  fps: present queue %u -> 1 frame", latency);

    return hr;
}

HRESULT WINAPI hkCreate9Ex(UINT sdk, IDirect3D9Ex** out)
{
    HRESULT hr = o_create9ex(sdk, out);
    if (SUCCEEDED(hr) && out && *out && !o_createdeviceex) {
        if (HookSlot(*out, 0x50, (void*)hkCreateDeviceEx, (void**)&o_createdeviceex))
            Log("  fps: CreateDeviceEx hooked");
    }
    return hr;
}

bool Locate()
{
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) { Log("  fps: d3d9.dll not loaded"); return false; }

    o_create9ex = (Create9Ex_t)GetProcAddress(d3d9, "Direct3DCreate9Ex");
    if (!o_create9ex) { Log("  fps: Direct3DCreate9Ex not exported"); return false; }

    // Hooked here rather than in Apply(): the client creates its device while it
    // starts up, long before anyone can press the apply key. Waiting would mean
    // the interval is already baked in and only a Reset could still change it.
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)o_create9ex, hkCreate9Ex);
    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) { Log("  fps: DetourAttach failed (%ld)", err); return false; }

    Log("  fps: hooked Direct3DCreate9Ex at %p, before device creation", o_create9ex);
    return true;
}

void Apply()
{
    // Nothing to do here - the interval is forced as the device is created, which
    // happens during startup.
}

} // namespace

Patch g_fps = { "fps", Locate, Apply, nullptr, nullptr, nullptr, false };
REGISTER_PATCH(g_fps);

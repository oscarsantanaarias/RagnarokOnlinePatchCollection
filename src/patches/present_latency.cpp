// Lowers the Direct3D 9Ex present queue depth.
//
// The client creates its device with Direct3DCreate9Ex, and D3D9Ex queues up to
// three frames by default. Once the queue is full, Present blocks until the GPU
// retires one - which is where the render thread actually spends its time. That
// blocking is invisible to every timing API in the client, which is why nothing
// in it looks like a frame limiter: there isn't one. Sampling put the thread
// inside Present (called from the site right after the wrapper below), and the
// render thread was only using 13% of a core while frames took 7 ms.
//
// SetMaximumFrameLatency(1) shortens that queue. It cuts input latency for
// certain; whether it raises frame rate depends on where the bottleneck sits, so
// treat this as something to measure, not a guaranteed win.
//
// The present wrapper is unmistakable - it returns D3DERR_DEVICELOST-style
// 0x8200000E when the device pointer is null, and that constant makes the
// signature unique:
//
//      56              push esi
//      8B F1           mov  esi, ecx
//      8B 56 08        mov  edx, [esi+8]        ; the device
//      85 D2           test edx, edx
//      75 ??           jnz  +
//      B8 0E 00 00 82  mov  eax, 0x8200000E
//      5E C2 04 00     pop esi / ret 4
//      8B 02           mov  eax, [edx]
//      6A 00 x4        push 0,0,0,0
//      52              push edx
//      FF 50 44        call [eax+0x44]          ; IDirect3DDevice9::Present

#include <windows.h>
#include <d3d9.h>
#include <detours.h>
#include <cstdint>

#include "../patch.h"
#include "../scan.h"

#pragma comment(lib, "d3d9.lib")

namespace {

typedef uint32_t (__fastcall* Present_t)(void* self, void* edx, int arg);

Present_t g_original = nullptr;
uint8_t*  g_wrapper  = nullptr;
volatile LONG g_configured = 0;

uint32_t __fastcall Hook(void* self, void* edx, int arg)
{
    if (!InterlockedExchange(&g_configured, 1)) {
        // The device lives at +8 in the wrapper's object, same slot the original
        // reads. Ask for the 9Ex interface rather than assuming a vtable index.
        IDirect3DDevice9* dev = *(IDirect3DDevice9**)((uint8_t*)self + 8);
        if (dev) {
            IDirect3DDevice9Ex* ex = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex)) && ex) {
                UINT before = 0;
                ex->GetMaximumFrameLatency(&before);
                HRESULT hr = ex->SetMaximumFrameLatency(1);
                Log("  present_latency: frame latency was %u, set to 1 (hr=0x%08lX)",
                    before, (unsigned long)hr);
                ex->Release();
            } else {
                Log("  present_latency: device is not D3D9Ex, nothing to do");
            }
        }
    }
    return g_original(self, edx, arg);
}

bool Locate()
{
    static const uint8_t kPattern[] = {
        0x56, 0x8B, 0xF1, 0x8B, 0x56, 0x08, 0x85, 0xD2, 0x75, 0x00,
        0xB8, 0x0E, 0x00, 0x00, 0x82, 0x5E, 0xC2, 0x04, 0x00,
        0x8B, 0x02, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00,
        0x52, 0xFF, 0x50, 0x44
    };
    static const char kMask[] = "xxxxxxxxx?xxxxxxxxxxxxxxxxxxxxxxx";

    uintptr_t p = scan::FindPattern(kPattern, kMask,
                                    (uintptr_t)scan::text_begin,
                                    (size_t)(scan::text_end - scan::text_begin));
    if (!p) { Log("  present_latency: present wrapper not found"); return false; }

    g_wrapper = (uint8_t*)p;
    Log("  present_latency: present wrapper at %p", g_wrapper);
    return true;
}

void Apply()
{
    g_original = (Present_t)g_wrapper;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_original, Hook);
    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) { Log("  present_latency: DetourAttach failed (%ld)", err); return; }
    Log("  present_latency: hooked, will lower frame latency on the next present");
}

} // namespace

Patch g_present_latency = { "present_latency", Locate, Apply, nullptr, nullptr, nullptr, false };
REGISTER_PATCH(g_present_latency);

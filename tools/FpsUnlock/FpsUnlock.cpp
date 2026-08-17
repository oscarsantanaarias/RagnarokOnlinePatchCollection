// FpsUnlock - standalone .asi that removes the frame limiter.
//
// The client's main loop ends every iteration with Sleep(1):
//
//      FF 50 10        call dword ptr [eax+10h]     ; run one frame
//      6A 01           push 1
//      FF 15 ....      call Sleep
//
// Sleep(1) does not sleep one millisecond - it sleeps until the next scheduler
// tick, so the frame time gets a floor it does not need. Sampling the render
// thread showed it sitting in that call 16.5% of the time.
//
// Patching the pushed argument from 1 to 0 turns it into Sleep(0): the thread
// still yields to anything else that is ready to run, but returns immediately if
// nothing is. One byte, and a single aligned byte write, so it can be flipped
// while the client runs without any risk of catching the instruction half-written.
//
// Located by pattern, so no address is tied to a client version. Verified:
//      2025-03-19 client -> 00C595A0
//      2026-02-19 client -> 00CDF1A1
//
// Keys:  F9 = toggle Sleep(0) / Sleep(1)

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "psapi.lib")

extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

namespace {

uint8_t* g_arg = nullptr;      // the pushed argument byte of Sleep(1)
uint8_t* g_pacer = nullptr;    // entry of the frame pacer
volatile LONG g_unlocked = 0;
volatile LONG g_pacer_off = 0;

uintptr_t FindPattern(const uint8_t* pattern, const char* mask,
                      uintptr_t base, uintptr_t size, int* hits)
{
    size_t len = strlen(mask);
    uintptr_t found = 0;
    *hits = 0;
    for (uintptr_t i = 0; i + len < size; i++) {
        bool ok = true;
        for (size_t j = 0; j < len; j++)
            if (mask[j] != '?' && pattern[j] != *(uint8_t*)(base + i + j)) { ok = false; break; }
        if (ok) { if (!found) found = base + i; (*hits)++; }
    }
    return found;
}

void Log(const char* fmt, ...)
{
    char path[MAX_PATH], msg[512];
    va_list ap;

    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "FpsUnlock.log"); else strcpy(path, "FpsUnlock.log");

    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    msg[sizeof(msg) - 1] = 0;
    va_end(ap);

    if (FILE* f = fopen(path, "a")) { fprintf(f, "%s\n", msg); fclose(f); }
    printf("[FpsUnlock] %s\n", msg);
    fflush(stdout);
}

uint8_t* FindLimiter()
{
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &mi, sizeof(mi));

    auto* base = (uint8_t*)mi.lpBaseOfDll;
    auto* dos  = (IMAGE_DOS_HEADER*)base;
    auto* nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec  = IMAGE_FIRST_SECTION(nt);

    uint8_t* begin = base + sec[0].VirtualAddress;
    uint8_t* end   = begin + sec[0].Misc.VirtualSize - 8;

    // call [eax+10h] / push 1 / call [imm32]
    static const uint8_t kPattern[] = { 0xFF, 0x50, 0x10, 0x6A, 0x01, 0xFF, 0x15 };

    uint8_t* found = nullptr;
    int hits = 0;
    for (uint8_t* p = begin; p < end; ++p) {
        if (memcmp(p, kPattern, sizeof(kPattern)) != 0) continue;
        if (!found) found = p;
        hits++;
    }

    if (hits == 1) {
        // found+3 is the "6A 01" (push 1); the call to Sleep follows it.
        // Nothing jumps into that range - the only branch nearby is the loop's
        // own JNZ, which targets the top of the loop, well before it.
        Log("main loop Sleep at %p (push at %p)", found + 5, found + 3);
        return found + 3;
    }
    Log("pattern matched %d times, expected exactly 1 - not patching", hits);
    return nullptr;
}

// The real frame pacer: accumulates the requested delay in microseconds, sleeps
// the whole milliseconds, measures how far Sleep overshot with
// QueryPerformanceCounter, and carries the remainder into the next frame. That
// is an FPS cap, not an incidental sleep.
//
// It is __cdecl with its argument at [ebp+8], so the caller cleans the stack and
// a bare ret is enough to turn it into a no-op. One byte, atomic.
//
// The globals it touches move between builds, so they are wildcarded.
uint8_t* FindPacer()
{
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), GetModuleHandle(nullptr), &mi, sizeof(mi));
    auto* base = (uint8_t*)mi.lpBaseOfDll;
    auto* dos  = (IMAGE_DOS_HEADER*)base;
    auto* nt   = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec  = IMAGE_FIRST_SECTION(nt);

    static const uint8_t kPattern[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10,
        0x83, 0x3D, 0,0,0,0, 0x00,
        0x75, 0,
        0x68, 0,0,0,0,
        0xC7, 0x05, 0,0,0,0, 0x01, 0x00, 0x00, 0x00,
        0xFF, 0x15, 0,0,0,0,
        0x8B, 0x4D, 0x08
    };
    static const char kMask[] = "xxxxxxxx????xx?x????xx????xxxxxx????xxx";

    int hits = 0;
    uintptr_t p = FindPattern(kPattern, kMask,
                              (uintptr_t)(base + sec[0].VirtualAddress),
                              sec[0].Misc.VirtualSize, &hits);
    if (hits == 1) { Log("frame pacer at %p", (void*)p); return (uint8_t*)p; }
    Log("frame pacer pattern matched %d times, expected 1 - not patching", hits);
    return nullptr;
}

// --- who calls the pacer -------------------------------------------------
//
// FUN_00ae7c30 has no static cross references: it is reached through a pointer,
// so Ghidra cannot say who drives it. Hooking it and recording the return
// address answers that at runtime, and the argument tells us the delay being
// requested per call - which is the actual frame budget, if this is the cap.

typedef void (__cdecl* Pacer_t)(int);
Pacer_t g_pacer_orig = nullptr;
uint8_t* g_pacer_tramp = nullptr;

struct CallerRec { uintptr_t ret; LONG count; int last_arg; long long sum_arg; };
CallerRec g_callers[32];
volatile LONG g_pacer_calls = 0;

// Target for our own pacer. 1000 fps is effectively "no limit" - the spin never
// has anything left to wait for - but the machinery stays in place so it can be
// dialled down to a real number later.
static int g_max_framerate = 1000;
volatile LONG g_bypass = 1;      // pass 0 to the client's pacer instead of its delay

// Same shape as the limiter that worked on the other client: sleep for the bulk
// of the remaining time, then spin the last millisecond, because Sleep only has
// scheduler-tick resolution and overshoots.
void FramePacer()
{
    static LARGE_INTEGER freq = { 0 };
    static LARGE_INTEGER last = { 0 };

    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (last.QuadPart != 0 && g_max_framerate > 0) {
        double target  = 1.0 / g_max_framerate;
        double elapsed = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
        double remain  = target - elapsed;

        if (remain > 0.002) Sleep((DWORD)((remain - 0.001) * 1000));

        while (elapsed < target) {
            QueryPerformanceCounter(&now);
            elapsed = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
        }
    }
    last = now;
}

void __cdecl hkPacer(int delay_us)
{
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    InterlockedIncrement(&g_pacer_calls);

    for (int i = 0; i < 32; ++i) {
        if (g_callers[i].ret == ret) {
            InterlockedIncrement(&g_callers[i].count);
            g_callers[i].last_arg = delay_us;
            g_callers[i].sum_arg += delay_us;
            break;
        }
        if (g_callers[i].ret == 0) {
            g_callers[i].ret = ret;
            g_callers[i].count = 1;
            g_callers[i].last_arg = delay_us;
            g_callers[i].sum_arg = delay_us;
            break;
        }
    }
    // The unlock: hand the client's pacer 0 instead of the delay it asked for,
    // so its accumulator never reaches a millisecond and it never sleeps. Then
    // run our own pacer, which at 1000 fps waits for nothing.
    FramePacer();
    g_pacer_orig(g_bypass ? 0 : delay_us);
}

void InstallPacerTrace()
{
    if (!g_pacer) return;

    // Prologue is 55 / 8B EC / 83 EC 10 - six bytes, so the trampoline copies
    // six and the patch is a 5-byte jmp plus one nop.
    g_pacer_tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!g_pacer_tramp) return;

    memcpy(g_pacer_tramp, g_pacer, 6);
    g_pacer_tramp[6] = 0xE9;
    *(int32_t*)(g_pacer_tramp + 7) = (int32_t)((intptr_t)(g_pacer + 6) - (intptr_t)(g_pacer_tramp + 11));
    g_pacer_orig = (Pacer_t)g_pacer_tramp;

    DWORD old;
    if (!VirtualProtect(g_pacer, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    g_pacer[0] = 0xE9;
    *(int32_t*)(g_pacer + 1) = (int32_t)((intptr_t)hkPacer - (intptr_t)(g_pacer + 5));
    g_pacer[5] = 0x90;
    VirtualProtect(g_pacer, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_pacer, 6);

    Log("pacer hooked: argument forced to 0, own pacer at %d fps", g_max_framerate);
    Log("  F4 = who calls it    F10 = toggle the 0 bypass");
}

void DumpCallers()
{
    Log("");
    Log("pacer called %ld times", g_pacer_calls);
    for (int i = 0; i < 32; ++i) {
        if (!g_callers[i].ret) break;
        Log("  caller %08X   %6ld calls   last arg %d us   avg %lld us",
            (unsigned)g_callers[i].ret, g_callers[i].count, g_callers[i].last_arg,
            g_callers[i].count ? g_callers[i].sum_arg / g_callers[i].count : 0);
    }
    if (!g_pacer_calls) Log("  never called - this is NOT the frame limiter");
}

DWORD WINAPI HotkeyThread(LPVOID)
{
    int prev9 = 0, prev10 = 0, prev8 = 0;
    for (;;) {
        // F4, not F8: RagnarokPatches.asi already owns F8 for its own report and
        // both DLLs live in the same process.
        int down8  = (GetAsyncKeyState(VK_F4)  & 0x8000) != 0;
        int down9  = (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
        int down10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

        if (down8 && !prev8) DumpCallers();
        prev8 = down8;

        if (down9 && !prev9 && g_arg) {
            InterlockedExchange(&g_unlocked, !g_unlocked);
            // 6A 01 = push 1        -> the original
            // EB 06 = jmp +6        -> skips the push AND the call, so the stack
            //                          stays balanced and Sleep never runs.
            // Both bytes sit in one aligned 16-bit slot, so this single store is
            // atomic on x86: safe to flip while the loop is running.
            *(volatile uint16_t*)g_arg = g_unlocked ? 0x06EB : 0x016A;
            Log("F9  main loop Sleep %s", g_unlocked ? "SKIPPED - fps uncapped" : "restored");
        }
        if (down10 && !prev10) {
            InterlockedExchange(&g_bypass, !g_bypass);
            Log("F10 pacer argument = %s", g_bypass ? "0 (uncapped)" : "original");
        }
        prev9 = down9; prev10 = down10;
        Sleep(60);
    }
}

DWORD WINAPI InitThread(LPVOID)
{
    FILE* dummy;
    // If another .asi already opened a console this fails harmlessly and the
    // freopen below just attaches to the existing one.
    AllocConsole();
    freopen_s(&dummy, "CONOUT$", "w", stdout);

    g_arg = FindLimiter();
    if (g_arg) {
        DWORD old;
        if (VirtualProtect(g_arg, 2, PAGE_EXECUTE_READWRITE, &old))
            Log("ready - F9 skips the main loop Sleep (starts OFF)");
        else { Log("VirtualProtect failed on the Sleep site"); g_arg = nullptr; }
    }

    g_pacer = FindPacer();
    if (g_pacer) InstallPacerTrace();

    CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

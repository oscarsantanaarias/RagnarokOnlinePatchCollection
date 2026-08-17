// RagnarokPatches - loader and hotkey handling.
//
// Nothing is modified when the DLL loads. Every patch locates itself, reports
// whether it fits this client, and waits. The user presses F5 to apply, so the
// original behaviour can be measured first and compared against the patched one.

#include <windows.h>
#include <cstdio>
#include <cstdarg>

#include "patch.h"
#include "scan.h"

static const int   kMaxPatches = 32;
static Patch*      g_patches[kMaxPatches];
static int         g_patch_count = 0;
static volatile LONG g_applied = 0;

void RegisterPatch(Patch* p)
{
    if (g_patch_count < kMaxPatches)
        g_patches[g_patch_count++] = p;
}

void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    char msg[1024];
    va_list ap;

    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "RagnarokPatches.log");
    else       strcpy(path, "RagnarokPatches.log");

    va_start(ap, fmt);
    _vsnprintf(msg, sizeof(msg) - 1, fmt, ap);
    msg[sizeof(msg) - 1] = 0;
    va_end(ap);

    if (FILE* f = fopen(path, "a")) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
    printf("[RagnarokPatches] %s\n", msg);
    fflush(stdout);
}

static void ApplyAll()
{
    if (InterlockedExchange(&g_applied, 1)) {
        Log("F5  already applied");
        return;
    }

    Log("");
    Log("======== APPLYING PATCHES ========");
    for (int i = 0; i < g_patch_count; ++i) {
        Patch* p = g_patches[i];
        if (!p->located) {
            Log("  %-24s skipped (not found in this client)", p->name);
            continue;
        }
        p->apply();
        p->applied = true;
    }
    Log("======== DONE ========");
    Log("");
}

static void ToggleAll()
{
    for (int i = 0; i < g_patch_count; ++i)
        if (g_patches[i]->applied && g_patches[i]->toggle)
            g_patches[i]->toggle();
}

static void ResetAll()
{
    Log("");
    Log("======== COUNTERS CLEARED - measuring ========");
    for (int i = 0; i < g_patch_count; ++i)
        if (g_patches[i]->located && g_patches[i]->reset)
            g_patches[i]->reset();
}

static void ReportAll()
{
    Log("");
    Log("======== REPORT ========");
    for (int i = 0; i < g_patch_count; ++i)
        if (g_patches[i]->located && g_patches[i]->report)
            g_patches[i]->report();
    Log("");
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    int prev5 = 0, prev6 = 0, prev7 = 0, prev8 = 0;
    for (;;) {
        int down5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        int down6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        int down7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        int down8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;

        if (down5 && !prev5) ApplyAll();
        if (down6 && !prev6) ToggleAll();
        if (down7 && !prev7) ResetAll();
        if (down8 && !prev8) ReportAll();

        prev5 = down5; prev6 = down6; prev7 = down7; prev8 = down8;
        Sleep(60);
    }
}

static DWORD WINAPI InitThread(LPVOID)
{
    FILE* dummy;
    AllocConsole();
    SetConsoleTitleA("RagnarokPatches");
    freopen_s(&dummy, "CONOUT$", "w", stdout);

    scan::Init();
    Log("loaded - image %p-%p, .text %p-%p",
        scan::image_begin, scan::image_end, scan::text_begin, scan::text_end);

    int ready = 0;
    for (int i = 0; i < g_patch_count; ++i) {
        Patch* p = g_patches[i];
        p->located = p->locate();
        if (p->located) ready++;
        Log("  %-24s %s", p->name, p->located ? "found" : "NOT FOUND");
    }

    Log("");
    Log("%d/%d patches ready", ready, g_patch_count);
    Log("  F5 = apply patches   F7 = clear counters   F8 = report");
    Log("");

    CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}

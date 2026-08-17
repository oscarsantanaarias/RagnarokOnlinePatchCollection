// Caches CResourceMgr::Load results by name.
//
// The client already keeps loaded resources in memory - the GRF is never read
// twice. What it repeats is *resolving the name*: on every single call Load takes
// a global critical section, memsets a 0x104 buffer, lowercases it through a
// translation table, runs strrchr for the extension, builds the full path as a
// std::string (heap allocation), looks it up, and frees the string again. The
// lookup almost always hits. All the work before it is thrown away.
//
// Measured on the 2025-03-19 client with the skill window open:
//      before   22,915 calls/s, render thread at 65.4% CPU
//      after    ~30 real loads/s, render thread at 17.4%
//      floor    13.2% (no windows open at all)
//
// The window made it visible but was not the cause: shadow.spr and shadow.act are
// resolved once per actor per frame, cursors.spr once per frame, and window border
// bitmaps once per tile - one tile every 5 pixels of window width, which is why
// bigger windows cost more.

#include <windows.h>
#include <detours.h>
#include <cstdint>
#include <cstring>

#include "../patch.h"
#include "../scan.h"

namespace {

// 4096 slots matter. With 128 and roughly 55 distinct resource names, colliding
// entries evicted each other and 28% of calls still missed.
const int   kSlots  = 4096;
const int   kKeyLen = 160;

const DWORD kTtlMs  = 1000;

// Screens that must never be cached.
//
// Character select and character creation build and tear down their windows
// constantly, and the client frees those bitmaps as it goes. A cached pointer
// outlives the resource, and the slot frames simply stop being drawn - no crash,
// they just vanish, leaving the background art showing through.
//
// These screens are also not worth caching: they ran at 2,649 requests/s against
// 21,574/s in game, and the whole cost of this bug lives in the per-frame redraw
// of the in-game UI, not here.
const char* const kNeverCache[] = {
    "select_character",
    "make_character",
};

bool ShouldBypass(const char* key)
{
    for (const char* needle : kNeverCache) {
        // Paths are mixed case in the client, so compare case-insensitively.
        for (const char* p = key; *p; ++p) {
            const char* a = p;
            const char* b = needle;
            while (*b && *a && (*a | 0x20) == (*b | 0x20)) { ++a; ++b; }
            if (!*b) return true;
        }
    }
    return false;
}

// Load takes a plain char*, NOT a std::string. Reading a "capacity" field at
// +0x14 and following it as a pointer dereferenced the first bytes of the text
// itself, throwing an access violation on every call.
typedef void* (__fastcall* ResourceLoad_t)(void* self, void* edx, void* name);

uint8_t*       g_load     = nullptr;
ResourceLoad_t g_original = nullptr;

volatile LONG g_enabled = 0;
volatile LONG g_hits = 0, g_misses = 0;

struct Entry {
    DWORD hash;
    void* resource;
    DWORD stamp;
    char  key[kKeyLen];
};

Entry            g_table[kSlots];
CRITICAL_SECTION g_lock;
LONG             g_lock_ready = 0;

// Which resources actually get loaded, and how often. Counts real loads only -
// cache hits are not work, and listing them just hides the signal. This is what
// exposed the problem in the first place: skill_upbar_mid.bmp showed up 281,819
// times in 19 seconds because the window's title bar re-resolves it once per
// 5-pixel tile, every frame.
const int kNameSlots = 512;

struct NameEntry {
    DWORD hash;
    LONG  count;
    char  name[128];
};

NameEntry     g_names[kNameSlots];
volatile LONG g_measuring = 0;
DWORD         g_started_at = 0;

void RecordName(const char* s, DWORD hash)
{
    for (int i = 0; i < kNameSlots; ++i) {
        int k = (hash + i) & (kNameSlots - 1);
        if (g_names[k].count == 0) {
            g_names[k].hash = hash;
            g_names[k].count = 1;
            strncpy(g_names[k].name, s, sizeof(g_names[0].name) - 1);
            g_names[k].name[sizeof(g_names[0].name) - 1] = 0;
            return;
        }
        if (g_names[k].hash == hash) { g_names[k].count++; return; }
    }
}

void* __fastcall Hook(void* self, void* edx, void* name)
{
    char  key[kKeyLen];
    DWORD hash = 0;
    int   slot = -1;
    int   i    = 0;

    key[0] = 0;
    __try {
        const char* s = (const char*)name;
        if (s) {
            hash = 2166136261u;
            for (i = 0; s[i]; ++i) {
                if (i >= kKeyLen - 1) { hash = 0; break; }   // too long: do not cache
                key[i] = s[i];
                hash = (hash ^ (uint8_t)s[i]) * 16777619u;
            }
            key[i < kKeyLen ? i : 0] = 0;
            if (i == 0) hash = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hash = 0;
        key[0] = 0;
    }

    if (hash && ShouldBypass(key)) hash = 0;   // straight through, never stored

    if (hash) {
        slot = (int)(hash & (kSlots - 1));
        if (g_enabled && g_lock_ready) {
            void* hit = nullptr;
            DWORD now = GetTickCount();

            EnterCriticalSection(&g_lock);
            // Compare the whole key, not just the hash. A 32-bit collision would
            // hand back the wrong resource, and that is visible on screen - it
            // crashed once inside an .act frame-time lookup that bounds-checks
            // the index but not the base pointer.
            if (g_table[slot].hash == hash && g_table[slot].resource &&
                (now - g_table[slot].stamp) < kTtlMs &&
                strcmp(g_table[slot].key, key) == 0) {
                hit = g_table[slot].resource;
            }
            LeaveCriticalSection(&g_lock);

            if (hit) { InterlockedIncrement(&g_hits); return hit; }
        }
    }

    void* resource = g_original(self, edx, name);
    InterlockedIncrement(&g_misses);

    if (hash && g_measuring && g_lock_ready) {
        EnterCriticalSection(&g_lock);
        RecordName(key, hash);
        LeaveCriticalSection(&g_lock);
    }

    // Never cache a null result: callers retry with a fallback resource.
    if (slot >= 0 && resource && g_lock_ready) {
        EnterCriticalSection(&g_lock);
        g_table[slot].hash     = hash;
        g_table[slot].resource = resource;
        g_table[slot].stamp    = GetTickCount();
        memcpy(g_table[slot].key, key, kKeyLen);
        LeaveCriticalSection(&g_lock);
    }
    return resource;
}

// Anchors on resource file names, which are identical across every client
// version, instead of on an opcode signature. Registers and offsets can change
// when the client is rebuilt; "shadow.spr" cannot.
//
// Around each use of the string the compiler emits:
//      E8 ?? ?? ?? ??      call GetResourceManager
//      68 <string>         push offset "shadow.spr"
//      8B C8               mov  ecx, eax
//      E8 ?? ?? ?? ??      call CResourceMgr::Load      <- what we want
//
// Two different anchors must agree on the same destination before we trust it.
//
// Addresses really do move between versions, so this is not optional:
//      Load    00A74090 (2025-03-19)   ->   00AEE770 (later client)
//      GetMgr  00A76F40               ->   00AF1620
uint8_t* FindLoad()
{
    static const char* kAnchors[] = { "shadow.spr", "shadow.act",
                                      "cursors.spr", "cursors.act" };
    uint8_t* candidate = nullptr;
    int votes = 0;

    for (int a = 0; a < 4; ++a) {
        uint8_t* text = scan::FindString(kAnchors[a]);
        if (!text) continue;

        for (uint8_t* q = scan::text_begin + 5; q + 12 < scan::text_end; ++q) {
            if (q[0] != 0x68 || *(uint8_t**)(q + 1) != text) continue;
            if (q[-5] != 0xE8 || q[5] != 0x8B || q[6] != 0xC8 || q[7] != 0xE8) continue;

            uint8_t* load = (uint8_t*)scan::CallTarget(q + 7);
            if (load < scan::text_begin || load >= scan::text_end) continue;

            if (!candidate)          { candidate = load; votes = 1; }
            else if (candidate == load) votes++;
            break;
        }
    }

    if (votes >= 2) {
        Log("  resource_cache: Load at %p (%d anchors agreed)", candidate, votes);
        return candidate;
    }
    Log("  resource_cache: only %d anchor(s) matched, need 2", votes);
    return nullptr;
}

// Points every `call Load` in .text at the cache. One five-byte call becomes
// another five-byte call, so no padding is involved. See scan::RepointCall for
// why padding a bigger block is not safe.
int RepointAllCallSites()
{
    int n = 0;
    for (uint8_t* p = scan::text_begin; p < scan::text_end - 8; ++p) {
        if (p[0] != 0xE8) continue;
        if ((uint8_t*)scan::CallTarget(p) != g_load) continue;
        if (!scan::RepointCall(p, (void*)Hook)) continue;
        n++;
        p += 4;
    }
    return n;
}

// The hook goes in at load time, but with the cache disabled: it only counts and
// records names. That way the untouched client can be measured first and compared
// against the patched one. Applying the patch is what turns the cache on.
bool Locate()
{
    g_load = FindLoad();
    if (!g_load) return false;

    InitializeCriticalSection(&g_lock);
    g_lock_ready = 1;

    // Detours builds the trampoline with its own length disassembler, so any
    // prologue works. Copying a fixed number of bytes by hand assumes the
    // prologue splits cleanly there, which need not hold in another client.
    g_original = (ResourceLoad_t)g_load;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_original, Hook);
    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) {
        Log("  resource_cache: DetourAttach failed (%ld)", err);
        return false;
    }

    g_started_at = GetTickCount();
    InterlockedExchange(&g_measuring, 1);
    Log("  resource_cache: counting (cache off until F5)");
    return true;
}

void Apply()
{
    int sites = RepointAllCallSites();
    InterlockedExchange(&g_enabled, 1);
    Log("  resource_cache: cache ON, %d call sites repointed", sites);
}

void Toggle()
{
    InterlockedExchange(&g_enabled, !g_enabled);
    Log("");
    Log("======== resource_cache %s ========",
        g_enabled ? "ON (patched)" : "OFF (original client behaviour)");
}

void Reset()
{
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; }
    EnterCriticalSection(&g_lock);
    memset(g_names, 0, sizeof(g_names));
    LeaveCriticalSection(&g_lock);

    InterlockedExchange(&g_hits, 0);
    InterlockedExchange(&g_misses, 0);
    g_started_at = GetTickCount();
    InterlockedExchange(&g_measuring, 1);
}

void Report()
{
    LONG  hits = g_hits, misses = g_misses;
    DWORD elapsed = GetTickCount() - g_started_at;
    LONG  total = hits + misses;

    Log("resource_cache  [cache %s]", g_enabled ? "ON" : "OFF (original client)");
    Log("  requests %ld in %lu ms  (%.0f/s)",
        total, elapsed, elapsed ? total * 1000.0 / elapsed : 0.0);
    Log("  served from cache %ld    real loads %ld  (%.0f/s)",
        hits, misses, elapsed ? misses * 1000.0 / elapsed : 0.0);
    if (total) Log("  %.1f%% answered without touching the resource manager",
                   100.0 * hits / total);

    Log("  --- resources actually loaded ---");
    if (!g_lock_ready) return;

    // Simple selection sort over a small table, printed highest first.
    EnterCriticalSection(&g_lock);
    for (;;) {
        int best = -1;
        for (int i = 0; i < kNameSlots; ++i)
            if (g_names[i].count > 0 && (best < 0 || g_names[i].count > g_names[best].count))
                best = i;
        if (best < 0) break;
        Log("  %7ld  %s", g_names[best].count, g_names[best].name);
        g_names[best].count = -g_names[best].count;   // mark as printed
    }
    for (int i = 0; i < kNameSlots; ++i)
        if (g_names[i].count < 0) g_names[i].count = -g_names[i].count;
    LeaveCriticalSection(&g_lock);
}

} // namespace

Patch g_resource_cache = { "resource_cache", Locate, Apply, Toggle, Reset, Report, false, false };
REGISTER_PATCH(g_resource_cache);

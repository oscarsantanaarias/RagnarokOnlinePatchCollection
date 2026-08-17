// Patch registry. Adding a new patch means one .cpp file and one REGISTER_PATCH
// line - nothing here or in dllmain.cpp needs to change.
#pragma once

#include <cstdint>

struct Patch
{
    const char* name;

    // Locate whatever this patch needs (addresses, offsets). Runs once at load.
    // Return false when the pattern is not present in this client: the patch is
    // then skipped and the rest still work.
    bool (*locate)();

    // Actually modify the client. Only called for patches whose locate() passed,
    // and only when the user presses the apply key.
    void (*apply)();

    // Optional. Turn the patch's effect off and on again at runtime, so the
    // original behaviour can be compared without restarting. Bound to F6.
    void (*toggle)();

    // Optional. Zero counters / start measuring. Bound to F7.
    void (*reset)();

    // Optional. Print what was measured. Bound to F8.
    void (*report)();

    bool located;   // filled in by the loader
    bool applied;
};

void RegisterPatch(Patch* p);

struct PatchRegistrar
{
    explicit PatchRegistrar(Patch* p) { RegisterPatch(p); }
};

#define REGISTER_PATCH(var) static PatchRegistrar registrar_##var(&var)

// Logs to the console and to RagnarokPatches.log next to the client executable.
void Log(const char* fmt, ...);

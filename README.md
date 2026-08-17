# RagnarokPatches

Runtime patches for Ragnarok Online clients, built as a single `.asi`.

No hardcoded addresses. Every patch locates itself by scanning the loaded image,
so the same binary works on any client version - including packed ones, because
the client's own stub finishes unpacking before this DLL runs.

## Build

Requires Visual Studio with the **v143** toolset and the Windows SDK.
Open `RagnarokPatches.sln`, select **Release | Win32**, build.

Output: `build\Release\RagnarokPatches.asi`

The platform must be Win32. Ragnarok clients are 32-bit and the bundled
`third_party\Detours\lib\x86\detours.lib` is x86.

## Install

A prebuilt binary is in [`release/`](release/) - copy `RagnarokPatches.asi` next
to the client executable.

Loading needs no injector: the Miles sound system (`Mss32.dll`, already shipped
with the client) calls `LoadLibrary` on every `*.asi` in that folder. That is why
`Mp3dec.asi` lives there - it is a real Miles audio codec, and anything else with
that extension is picked up the same way.

If your client has no `Mss32.dll`, extract `release/asi-loader.rar` into the
client folder too. It is Ultimate ASI Loader built as a `d3d9.dll` proxy, plus
its `d3d9.ini` where `LoadPlugins=1` enables plugin loading.

Beware when disabling it by renaming: Windows matches `*.asi` against short 8.3
names too, so `RagnarokPatches.asi22` still gets loaded and you end up with two
copies of the DLL in the process. Use an extension that cannot shorten to `.asi`,
such as `.bin`, or move the file out of the folder.

## Use

A console window opens with the client. On load, every patch reports whether it
found what it needs. Nothing is modified yet.

| Key | Action |
|-----|--------|
| F5  | Apply all located patches |

Applying is one-way: restart the client to get back to the original code.
Output also goes to `RagnarokPatches.log` next to the client executable.

## Patches

### resource_cache

Caches `CResourceMgr::Load` results by resource name.

The client already keeps loaded resources in memory - the GRF is never read
twice. What repeats is *resolving the name*: on every call it takes a global
critical section, memsets a 0x104 buffer, lowercases it, runs `strrchr` for the
extension, builds the full path as a `std::string`, looks it up, and frees the
string. The lookup nearly always hits; everything before it is wasted.

Measured on the 2025-03-19 client with the skill window open:

| | Calls resolved | Render thread CPU |
|---|---|---|
| Original | 22,915 /s | 65.4% |
| Patched | ~30 /s | 17.4% |
| No windows open (floor) | - | 13.2% |

The skill window only made it visible. `shadow.spr` and `shadow.act` are resolved
once per actor per frame, `cursors.spr` once per frame, and window border bitmaps
once per tile - one tile per 5 pixels of window width, which is why larger windows
cost more.

## Adding a patch

Create a file under `src/patches/`, then add it to `RagnarokPatches.vcxproj`:

```cpp
#include "../patch.h"
#include "../scan.h"

namespace {

bool Locate()
{
    // Find what you need via scan::FindString / scan::FindPattern.
    // Return false if this client does not match; other patches still run.
    return true;
}

void Apply()
{
    // Modify the client. Only called when Locate() returned true.
}

} // namespace

Patch g_my_patch = { "my_patch", Locate, Apply, false, false };
REGISTER_PATCH(g_my_patch);
```

## Notes for whoever touches this next

**Anchor on strings, not on opcode signatures.** Resource file names inside the
GRF are stable across every client version; register allocation and offsets are
not. Addresses genuinely move - `Load` sits at `00A74090` in the 2025-03-19
client and at `00AEE770` in a later one.

**Require two independent anchors to agree** before trusting a located address.

**Never pad a patched block with nops.** Replacing a 17- or 28-byte block with a
call plus padding looks fine until some jump inside the same function targets an
address in the middle of that block; execution then slides through the padding
and crashes somewhere unrelated. Replacing one 5-byte call with another 5-byte
call has no such failure mode. Scanning bytes also cannot tell whether a match
starts on a real instruction boundary, which is a second reason batch-patching
blocks found by signature is unsafe.

**`CResourceMgr::Load` takes a plain `char*`, not a `std::string`.** Treating it
as one and following a "capacity" field at `+0x14` dereferences the first bytes
of the text as a pointer.

**Any cache shared between threads needs a lock.** `Load` is called from several
threads - that is why the client wraps it in a critical section. An unlocked
table tears: one entry ends up holding one resource's key with another's pointer,
and the wrong resource is handed back.

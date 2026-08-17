# Prebuilt binary

| File | What it is |
|------|------------|
| `RagnarokPatches.asi` | The patches. This is the only file you need if your client already loads `.asi` plugins. |
| `asi-loader.rar` | Ultimate ASI Loader (`d3d9.dll` + `d3d9.ini`), for clients that do not. |

## Installing

Drop `RagnarokPatches.asi` next to the client executable. That is usually all.

Most Ragnarok clients load it on their own, with no injector: they ship the Miles
Sound System (`Mss32.dll`), which calls `LoadLibrary` on **every `*.asi` file** in
the client folder while initialising audio. That is why `Mp3dec.asi` sits in the
same folder - it is a genuine Miles audio codec, and anything else with that
extension is picked up the same way.

If your client has no `Mss32.dll`, or the console window never appears, extract
`asi-loader.rar` into the client folder as well. It contains Ultimate ASI Loader
built as a `d3d9.dll` proxy: the client loads `d3d9.dll` as usual, and the loader
then loads every `.asi` next to it. `d3d9.ini` is its configuration -
`LoadPlugins=1` is what enables that.

Verified working on:

| Client | Notes |
|--------|-------|
| `2025-03-19_Ragexe` | unpacked |
| `2026-02-19_Ragexe` | packed - patterns are matched in memory, after the client's own stub finished unpacking |

## Disabling it

Move the file out of the folder, or rename it to something that cannot end in
`.asi` - `.bin` works.

Renaming to `RagnarokPatches.asi22` does **not** disable it. Windows matches the
`*.asi` wildcard against short 8.3 filenames as well, so that file still gets
loaded and you end up running two copies of the DLL in the same process.

## Use

A console window opens together with the client and reports whether each patch
found what it needs. Nothing is modified until you ask for it.

| Key | Action |
|-----|--------|
| F5  | Apply the patches |
| F6  | Turn the cache off/on, to compare against the original |
| F7  | Clear counters and start measuring |
| F8  | Print the report |

Applying is one-way - restart the client to get the original code back.
Output is also written to `RagnarokPatches.log` next to the client executable.

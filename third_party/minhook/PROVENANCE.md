# MinHook vendoring provenance

## Upstream

- **Repository:** https://github.com/TsudaKageyu/minhook
- **Tag:** `v1.3.3`
- **Tag commit:** `9fbd087432700d73fc571118d6a9697a36443d88`
- **Imported:** 2026-05-20 (no local modifications)
- **License:** 2-clause BSD (see `LICENSE.txt` in this directory)

## How we obtained it

```powershell
Invoke-WebRequest `
  -Uri "https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.zip" `
  -OutFile minhook.zip
# Extracted contents of `minhook-1.3.3/` placed verbatim in this directory.
```

## What's vendored

In the **source repository**, the upstream MinHook v1.3.3 source tree
is mirrored in this directory. Our build links only the x64 subset
needed for inline hooking:

```
include/MinHook.h
src/buffer.c, buffer.h
src/hook.c
src/trampoline.c, trampoline.h
src/hde/hde64.c, hde64.h
src/hde/pstdint.h
src/hde/table64.h
```

The upstream tree also contains `src/hde/hde32.*`, `src/hde/table32.*`,
the upstream `build/` and `dll_resources/` subdirectories, etc. — those
are present in the source repo for fidelity to upstream but are NOT
compiled into our DLL (we're x64-only and don't ship MinHook as a DLL).

Files are unmodified upstream. To verify, compare against the upstream
tag by checksumming each file or re-running the import command above
and diffing the result.

In the **binary release zip**, only this `PROVENANCE.md` and the
upstream `LICENSE.txt` are shipped (we satisfy MinHook's BSD-2-Clause
attribution requirement; the source itself is in the GitHub repo / the
auto-generated source archive for each release tag).

## What our code calls

The full list of MinHook API calls our code makes (all in
`src/cpu_hooks.cpp`):

- `MH_Initialize()` — on hook install
- `MH_CreateHook()` — once per hook target
- `MH_EnableHook(MH_ALL_HOOKS)` — once after all hooks created
- `MH_DisableHook(MH_ALL_HOOKS)` — on enable-time failure (best-effort)
  and on explicit DLL unload
- `MH_RemoveHook()` — on individual targets when tearing down a
  partially-completed install, or on install-time failures
- `MH_Uninitialize()` — on explicit DLL unload, or on install failure
  if WE called `MH_Initialize()` during the current install attempt

No other entry points used. No private/undocumented MinHook internals
touched.

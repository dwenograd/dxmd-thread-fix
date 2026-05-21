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

In the **source repository**, the full upstream tree is in this
directory (only the files needed to build MinHook as part of our DLL):

```
include/MinHook.h
src/buffer.c, buffer.h
src/hook.c
src/trampoline.c, trampoline.h
src/hde/hde64.c, hde64.h
src/hde/pstdint.h
src/hde/table64.h
(plus hde32.* and table32.* for completeness; we link only the x64 hde)
```

Unmodified upstream files. To verify, compare against the upstream
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

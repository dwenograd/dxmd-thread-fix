# MinHook vendoring provenance

## Upstream

- **Repository:** https://github.com/TsudaKageyu/minhook
- **Tag:** `v1.3.3`
- **Tag commit:** `9fbd087432700d73fc571118d6a9697a36443d88`
- **Imported:** 2026-05-20 (no local modifications)
- **License:** 2-clause BSD (see `LICENSE.txt` in this directory)

## How we obtained it

```powershell
Invoke-WebRequest \
  -Uri "https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.zip" \
  -OutFile minhook.zip
# Extracted contents of `minhook-1.3.3/` placed verbatim in this directory.
```

## What's vendored

Only the source needed to build MinHook as part of our DLL:

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

## What our code calls

The full list of MinHook API calls our code makes:

- `MH_Initialize()` — once, in `install_cpu_hooks()` (cpu_hooks.cpp)
- `MH_CreateHook()` — once per hook target (cpu_hooks.cpp)
- `MH_EnableHook(MH_ALL_HOOKS)` — once after all hooks created (cpu_hooks.cpp)
- `MH_RemoveHook()` — on individual targets if install partially fails (cpu_hooks.cpp)
- `MH_DisableHook(MH_ALL_HOOKS)` — once, on explicit DLL unload (cpu_hooks.cpp)
- `MH_Uninitialize()` — once, on explicit DLL unload (cpu_hooks.cpp)

No other entry points used. No private/undocumented MinHook internals
touched.

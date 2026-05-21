// Changelog

All notable changes to this project will be documented here.

## v1.1.1 (2026-05-21)

Comment-cleanup pass over the v1.1.0 source. No code changes,
no behaviour changes, no ABI changes — same 20 exports, same
ordinals, same import surface. Trimmed roughly 15 lines of
commentary that restated obvious code or general x64/Windows
knowledge.

If you have v1.1.0 working, there's no reason to update.

## v1.1.0 (2026-05-21)

### Drop-in DLL, no INI, no installer scripts

User-facing simplification pass over v1.0.0. The fix behavior is
unchanged; the surface area is much smaller.

- **No more INI.** The three v1.0.0 INI knobs
  (`LogicalProcessors`, `ClampAffinity`, `LogLevel`) were never tuned
  in practice. They're now hardcoded constants in
  `src/dxmd_thread_fix.cpp` (defaults: `8`, `false`, `1`). Edit and
  recompile if you genuinely need different values — see
  [`docs/BUILDING.md`](docs/BUILDING.md).
- **No more PowerShell installer.** `install.ps1` and `uninstall.ps1`
  are removed. Install = drop `dxgi.dll` into `retail\`. Uninstall =
  delete it.
- **Single-file C++ source.** The 7 `.cpp` + 5 `.h` files in `src/`
  collapsed into one `src/dxmd_thread_fix.cpp` (plus the inherently-
  separate `.asm`, `.def`, `.rc` toolchain inputs). The ABI surface
  (20 exports, same ordinals, same names) is byte-identical to
  v1.0.0; the only import-table change is `GetPrivateProfileIntW`
  removed (no INI to read).
- **Heavy README sections moved to `docs/`.** Trust / auditing,
  compatibility, troubleshooting, building, and the architectural
  design narrative now each have their own file under
  [`docs/`](docs/). The README is a short drop-in guide.

### Migrating from v1.0.0

If you used `install.ps1` to install v1.0.0:

- Your `<game>\retail\dxgi.dll` will be replaced by v1.1.0's via the
  same drop-in step (or just leave v1.0.0 in place if it's working;
  v1.1.0 has no different runtime behavior with default settings).
- The leftover `<game>\retail\dxmd-thread-fix.ini` from v1.0.0 is
  ignored by v1.1.0 and can be deleted.

## v1.0.0 (2026-05-21)

### What this is

The first public release of **dxmd-thread-fix**: a drop-in `dxgi.dll`
shim that fixes the long-standing high-core-CPU crash in Deus Ex:
Mankind Divided (build 1.19, the final 2017 patch). It also fixes the
notorious alt-tab crash as a side effect of the same root-cause fix.

### Architecture summary

- A proxy `dxgi.dll` placed next to `DXMD.exe` (Windows loader picks
  it up before System32's via standard DLL search order).
- All 20 dxgi exports forwarded to System32's real dxgi via asm
  tail-jump stubs (exact name + ordinal match for compatibility with
  graphics debuggers and the Steam overlay).
- MinHook inline detours on 6 CPU/topology APIs (`GetSystemInfo`,
  `GetNativeSystemInfo`, `GetActive/MaxProcessorCount`,
  `GetActive/MaxProcessorGroupCount`) so the game sees 8 logical
  processors instead of the real ~64 on high-core CPUs.

### Tested on

- Threadripper 3970X (32C/64T), Windows 11, NVIDIA RTX 4090, Steam install.
- Validated through: main menu, zone loads, combat, save/load, aggressive
  alt-tabbing during Bink intro movies (historically the most fragile
  state in DXMD).

### Trust / provenance

- MIT licensed (see `LICENSE`).
- Source: https://github.com/dwenograd/dxmd-thread-fix
- VERSIONINFO embedded in the DLL; right-click → Properties → Details
  should show ProductName "dxmd-thread-fix".
- The DLL imports only `KERNEL32.dll`; no network, registry-write, or
  process-creation APIs.
- The DLL reads only `dxmd-thread-fix.ini` next to itself; writes only
  `dxmd-thread-fix.log` next to itself (when `LogLevel > 0`).
- The install/uninstall PowerShell scripts additionally read Steam's
  registry install path for auto-detection, and write/copy/delete files
  inside the game's `retail\` folder. They do not write to the registry
  or contact the network.
- Vendors MinHook 1.3.3 (BSD-2-Clause) unmodified; see
  `third_party/minhook/PROVENANCE.md` for the upstream tag and commit hash.

### Multiple rounds of independent review

The codebase went through multiple rounds of parallel review using
independent rubber-duck, code-review, and security-review agents
covering correctness, security, edge cases, packaging, and
documentation accuracy. Each round resulted in a hardening pass; the
fixes from those reviews are included in this release.

### Known limitations (documented, not bugs)

- Conflicts with other mods that also use a `dxgi.dll` proxy
  (Special K, ReShade configured as `dxgi.dll`, ENBSeries). Use only
  one at a time.
- Microsoft Store / Xbox Game Pass installs of DXMD are not supported
  (the app sandbox blocks `dxgi.dll` proxying).
- The shim does NOT fix DXMD's separate **rapid menu interaction** crash
  (inventory/looting clicked too fast). That's a different bug with a
  different root cause.

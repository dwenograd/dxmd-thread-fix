// Changelog

All notable changes to this project will be documented here.

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

### Six rounds of independent review

Before release, the codebase went through six rounds of parallel review
using independent rubber-duck, code-review, and security-review agents
covering correctness, security, edge cases, packaging, and
documentation accuracy. Each round resulted in a hardening pass. Four
independent security reviews all came back clean.

### Known limitations (documented, not bugs)

- Conflicts with other mods that also use a `dxgi.dll` proxy
  (Special K, ReShade configured as `dxgi.dll`, ENBSeries). Use only
  one at a time.
- Microsoft Store / Xbox Game Pass installs of DXMD are not supported
  (the app sandbox blocks `dxgi.dll` proxying).
- The shim does NOT fix DXMD's separate **rapid menu interaction** crash
  (inventory/looting clicked too fast). That's a different bug with a
  different root cause.

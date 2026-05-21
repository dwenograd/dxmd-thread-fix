# dxmd-thread-fix

A drop-in `dxgi.dll` shim that fixes the high-core-CPU crash in
**Deus Ex: Mankind Divided** (build 1.19, the final Aug 2017 patch).

This is a *real* fix, not an affinity workaround. It hooks the Windows
CPU-discovery APIs (`GetSystemInfo` and friends) so the game and its
middleware (`bink2w64.dll`, `amd_ags64.dll`, `ApexFramework_x64.dll`,
PhysX, ...) see a sane logical-processor count and size their thread
pools accordingly. The game stops spawning more threads than it can
manage, and the crash stops happening.

Tested on a Threadripper 3970X (32C/64T). Should work on any high-core
CPU including the 96C/192T+ ones that arrived after DXMD's last patch.

## Why this is different from Process Lasso / `start /affinity`

The popular workarounds (Process Lasso, `start /affinity FF`, BES,
Task Manager affinity) constrain *where* the game's 64 threads can run.
The game still spawns 64 threads — it just crams them onto 8 cores.

If the underlying bug is a fixed-size internal buffer indexed by CPU
count, those workarounds can still die. They also require an external
tool or wrapper per launch.

This shim lies to the game about how many CPUs exist. The game sees
8 (configurable), spawns 8 worker threads, and that's the end of the
story. No external launcher, no per-game-launch fiddling, no leftover
process running in the background.

## Install

1. Build `dxgi.dll` (or grab a release zip — see below).
2. Drop **`dxgi.dll`** and **`dxmd-thread-fix.ini`** into your DXMD
   install's `retail\` folder, next to `DXMD.exe`. Typical Steam path:
   ```
   <SteamLibrary>\steamapps\common\Deus Ex Mankind Divided\retail\
   ```
3. Launch DXMD normally (through Steam).
4. Check `retail\dxmd-thread-fix.log` to verify the hook engaged. You
   should see lines like `GetSystemInfo: 64 -> 8`.

The included `install.ps1` does steps 1-3 automatically and can
auto-detect the Steam install location:

```powershell
pwsh -File install.ps1
```

## Uninstall

```powershell
pwsh -File uninstall.ps1
```

Or manually: delete `dxgi.dll`, `dxmd-thread-fix.ini`, and
`dxmd-thread-fix.log` from `retail\`. The shader cache (`PSOCache.bin`)
and your saves are untouched.

## Verify it's working

After launching DXMD, `retail\dxmd-thread-fix.log` should contain
something like:

```
[20:45:12.103] dxmd-thread-fix attach. config: LogicalProcessors=8 ClampAffinity=0 LogLevel=1
[20:45:12.105] Real dxgi loaded: C:\WINDOWS\system32\dxgi.dll @ 00007FFD4CE00000
[20:45:12.105] dxgi exports resolved: 20 ok, 0 missing.
[20:45:12.106] Fake topology: 8 logical processors, 1 group(s)  (real: 64 in 1 group(s))
[20:45:12.130] Hooked GetSystemInfo @ 00007FFD514BD740
[20:45:12.131] GetSystemInfo: 64 -> 8
... more "Hooked X" lines ...
[20:45:12.180] CPU hook install complete: 6 active, 1 opt-skipped.
```

The `64 -> 8` line is the smoking gun — that's the game's own CPU
detection being lied to.

If you don't see a log file at all, the shim didn't load. Most likely
cause: another mod is already proxying `dxgi.dll` (Special K, ReShade,
ENB). See [Compatibility](#compatibility) below.

## Configuration

Edit `dxmd-thread-fix.ini` next to `dxgi.dll`. Defaults work for almost
everyone.

```ini
[ThreadFix]
LogicalProcessors=8     ; how many CPUs to report to the game
ClampAffinity=0         ; 1 = also rewrite SetThreadAffinityMask calls
LogLevel=1              ; 0=silent 1=startup+first-hit 2=verbose
```

* **LogicalProcessors** — the community-validated sweet spot is `8`.
  Some users report stability at `12`. If your CPU has fewer logical
  processors than this value, your real count is reported (we never lie
  *upward*).
* **ClampAffinity** — leave at `0` unless your log shows
  `SetThreadAffinityMask` calls with masks outside the first
  `LogicalProcessors` bits.
* **LogLevel** — `1` is fine for normal use; `2` makes the log noisy
  but is useful when filing a bug.

## Compatibility

| Mod / Tool | Works with this? |
|---|---|
| Steam overlay | ✅ Yes (Steam hooks the swapchain VTable, not `dxgi.dll` itself) |
| Steam achievements | ✅ Yes (we don't touch save data or game code) |
| ReShade (proxy = `dxgi.dll`) | ❌ Conflicts — both want to be `dxgi.dll`. See workaround below. |
| ReShade (proxy = `d3d11.dll` or other) | ✅ Yes |
| Special K | ❌ Conflicts — Special K also proxies `dxgi.dll`. |
| ENBSeries | ❌ Conflicts (also proxies `dxgi.dll`). |
| AMD/NVIDIA driver overlays | ✅ Yes |

**Workaround for `dxgi.dll`-proxy conflicts:** chain the proxies — load
the other mod first (e.g. rename ours to `dxgi.dxmd-thread-fix.dll` and
have the other proxy load us). Future v1.1 will likely add an
alternative install mode that proxies `winmm.dll` instead, sidestepping
the conflict.

## Theory of operation

`dumpbin /imports DXMD.exe` reveals:

```
KERNEL32.dll
   GetSystemInfo
   SetThreadAffinityMask
   IsProcessorFeaturePresent
   CreateThread
   ...
```

`GetSystemInfo` fills a `SYSTEM_INFO` struct whose `dwNumberOfProcessors`
field is what the game uses to size its worker thread pool. The
middleware DLLs `bink2w64.dll` and `amd_ags64.dll` also import
`GetSystemInfo`. Hooking that one API at its KernelBase implementation
address catches every caller, because the OS loader follows
forwarders during import resolution — both `kernel32.dll!GetSystemInfo`
and `KernelBase.dll!GetSystemInfo` end up writing the same physical
address into every IAT slot.

So the fix is:
1. **Get loaded into the DXMD process early.** DXMD already statically
   imports `dxgi.dll`. Drop our `dxgi.dll` into the EXE's directory and
   the OS loader picks it up first (standard DLL search order).
2. **Forward every real `dxgi.dll` export.** We resolve all 20 exports
   from `C:\Windows\System32\dxgi.dll` at load time and tail-jump
   through them via assembly stubs. Ordinals are preserved exactly.
3. **Hook the CPU-discovery APIs** using [MinHook][mh]. We cover:
   `GetSystemInfo`, `GetNativeSystemInfo`, `GetActiveProcessorCount`,
   `GetMaximumProcessorCount`, `GetActiveProcessorGroupCount`,
   `GetMaximumProcessorGroupCount`. Every hook returns values
   consistent with one fake topology (1 group, N logical processors)
   so middleware that consults multiple APIs gets coherent answers.
4. **Lie consistently.** `dwNumberOfProcessors`, `dwActiveProcessorMask`,
   the per-group counts, the group count — all agree.

[mh]: https://github.com/TsudaKageyu/minhook

The shim is loader-lock-safe in practice: at the moment our `DllMain`
runs, the only thread is the one processing DXMD's static imports, so
MinHook's `SuspendThread`-on-all-others enumerates no other threads.
This pattern is what every dxgi-proxy game mod uses (Special K,
ReShade, ENB) and is well-validated across thousands of games.

## Build from source

Requires Windows + Visual Studio 2022 (Community is fine) with the
"Desktop development with C++" workload installed (provides cl.exe,
ml64.exe, link.exe).

```powershell
git clone https://github.com/<you>/dxmd-thread-fix.git
cd dxmd-thread-fix
pwsh -File build.ps1
pwsh -File install.ps1
```

Output: `dist\dxgi.dll` (~150 KB).

## Project layout

```
src/
  dllmain.cpp        - DLL entry point, attach/detach sequence
  config.cpp/.h      - INI parsing via GetPrivateProfileIntW
  log.cpp/.h         - thread-safe append-only logger
  topology.cpp/.h    - single source of truth for the fake CPU topology
  dxgi_exports.cpp   - loads real dxgi, resolves all 20 export pointers
  dxgi_exports.h
  dxgi_stubs.asm     - one tail-jump stub per export
  dxgi.def           - module-def file: names + ordinals
  cpu_hooks.cpp/.h   - MinHook detours for the topology APIs
third_party/
  minhook/           - vendored MinHook 1.3.3 (BSD-2-Clause)
build.ps1            - MSVC build
install.ps1          - copy dxgi.dll + ini into DXMD's retail/
uninstall.ps1        - reverse of install
dxmd-thread-fix.ini  - default config
```

## License

MIT. See [LICENSE](LICENSE).

This project vendors MinHook (BSD-2-Clause). See
[third_party/minhook/LICENSE.txt](third_party/minhook/LICENSE.txt).

## Acknowledgements

* [Tsuda Kageyu](https://github.com/TsudaKageyu) for MinHook.
* The Steam Community / r/Deusex / PCGamingWiki threads that have been
  trading affinity workarounds for this bug since 2017.
* Eidos Montréal for one of the best immersive sims ever made,
  even if they shipped it with this bug.

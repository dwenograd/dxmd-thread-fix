# dxmd-thread-fix — source-level design notes

This document is for anyone reading the source code for the first
time. It explains *why* the code is shaped the way it is, so the
patterns that look weird in isolation (compile-time function-pointer
initialization! asm tail-jumps! three-pass MinHook install!) make
sense as soon as you understand the constraints.

If you're a user, you don't need this — read the [README](../README.md).

If you're a security-curious reader checking that the DLL does what
it claims, this is the right starting point.

---

## The problem

Deus Ex: Mankind Divided (DXMD), build 1.19 (the final 2017 patch),
crashes on CPUs with high logical-processor counts. On a 32C/64T
Threadripper, the game typically crashes within seconds — before the
title screen, during the first zone load, or when you alt-tab.

`dumpbin /imports DXMD.exe` shows the game calls `GetSystemInfo`
directly. That API returns a `SYSTEM_INFO` struct whose
`dwNumberOfProcessors` field tells the game how many logical
processors exist. DXMD uses that number to size its internal worker
thread pool. The bug pattern is consistent with a fixed-size internal
buffer or a worker-pool sizing routine that doesn't survive 64+
threads.

Existing community workarounds (Process Lasso, `start /affinity`,
Task Manager affinity) constrain *where* the game's threads run but
don't reduce *how many* it spawns. If the bug is in the spawning
path (which it appears to be), they don't reliably help.

## The fix: hook `GetSystemInfo` and lie

The fix is conceptually simple:
1. Get loaded into the DXMD process early.
2. Patch the OS APIs that report CPU count to return a sane number
   (default: 8) instead of the real ~64.
3. Profit. Game spawns 8 workers, doesn't trip its bug, doesn't crash.

The rest of this doc is about how to do step 1 and step 2 without
adding *new* bugs.

---

## Step 1: getting loaded into DXMD early

### Why a `dxgi.dll` proxy

DXMD statically imports `dxgi.dll`. The Windows loader resolves that
import using the standard DLL search order, which for non-KnownDLLs
searches the EXE's own directory before `System32`. (KnownDLLs —
the small set of pre-mapped core OS DLLs listed in
`HKLM\SYSTEM\...\Session Manager\KnownDLLs` — bypass this search and
load from `\KnownDlls\`. `dxgi.dll` is not in the KnownDLLs list on
any version of Windows we support, so the standard search applies.)
So if we drop our `dxgi.dll` into the same `retail/` folder as
`DXMD.exe`, the loader picks ours up first.

This means our DLL runs **earlier than any DXMD code** — early enough
that our hooks are in place before `GetSystemInfo` is ever called.

### What "being dxgi.dll" requires

Our proxy exports the 20 dxgi exports captured from System32 dxgi.dll
on the Windows 10 / early Windows 11 export set (`src/dxgi.def`).
DXMD imports a subset of these via its IAT; middleware and tooling
(PIX, AMD AGS, etc.) may resolve others by name or ordinal at
runtime via `GetProcAddress`. We preserve that observed set so
anything that worked against the real dxgi.dll for DXMD still
works against our proxy.

We do NOT mirror exports that newer Windows versions add over time
— e.g., Windows 11 ships a `fothk` export at ordinal 1000 that's
not in our list. Those are deliberately omitted: their signatures
are undocumented, and a wrong-shape stub would corrupt the
caller's stack on return. DXMD shipped in 2016 with middleware
from that era; none of it uses Windows-11-era exports. See
`src/dxgi.def` for the full rationale.

We MUST:
- Export the same 20 names with the same ordinals (`src/dxgi.def`).
- For each export, forward the call to the real System32 dxgi.dll
  without disturbing arguments, return values, or registers.

The forwarding is done in `src/dxgi_stubs.asm` — one tail-jump
instruction per export:

```asm
CreateDXGIFactory PROC
    jmp QWORD PTR [pfn_CreateDXGIFactory]
CreateDXGIFactory ENDP
```

A tail-jump preserves the entire ABI (rcx/rdx/r8/r9, xmm0-3, the
stack, the return address) without us writing per-export thunks.
Critical for the **private** dxgi exports (`DXGID3D10*`, `PIX*`,
`SetAppCompatStringPointer`, etc.) whose prototypes are not publicly
documented.

`pfn_CreateDXGIFactory` and friends are 64-bit globals in `.data`,
declared in `src/dxgi_exports.cpp`. They're overwritten in our
DllMain with the real System32 dxgi addresses by
`load_system_dxgi_and_resolve()`.

### Why we can't use static `.def` forwarders

The natural temptation is to use the linker's `.def`-file forwarder
syntax: `CreateDXGIFactory = dxgi.CreateDXGIFactory`. That doesn't
work because our file is also named `dxgi.dll`, so the forwarder
would loop back to us. We'd need to either rename System32's dxgi
locally (ugly install) or pick a different name. Runtime-resolved
function pointers in `.data` are cleaner.

---

## Step 2: the apphelp pre-DllMain crash (and why traps exist)

The very first attempt at this fix initialized every `pfn_FOO` to
`nullptr` and resolved them in DllMain. **That crashed every install
during process startup**, before our DllMain ever ran.

Root cause: in the DXMD startup path we observed (debugger and
minidump analysis on real crash dumps), the Windows app-compat
shim engine (`apphelp.dll`'s `SE_DllLoaded` code path) runs on
DLLs in the process and calls some of dxgi's compat-namespace
exports (notably `SetAppCompatStringPointer`) BEFORE our DllMain
entry point. This appears to be tied to DXMD's compat-flagged
state and to dxgi's specifically-shimmable compat namespace; we
don't claim this is a general "for every DLL on every Windows"
invariant, only that it reliably happens for our target process.
Our asm stub did `jmp [NULL]` and the process died at fault
address 0.

Fix: every `pfn_FOO` is initialized at **compile time** to point at
a no-op trap function (see `src/dtf_traps.cpp`). The asm stubs always
land on valid code, even before our DllMain runs. Apphelp asks "did
you handle this compat string?", the trap returns 0 (which the
apphelp code path observed during DXMD startup accepts as a
successful no-op shim result), and the loader proceeds to run
our DllMain, which overwrites the trap pointers with real addresses.

### Five trap categories

Not all traps return 0. Returning S_OK to `CreateDXGIFactory*` would
mean "factory created successfully", and the game would dereference
the uninitialized output pointer. So:

- **`dtf_trap_pre_resolve`** (generic, returns 0). Used for the
  compat-pass exports apphelp actually calls — empirically observed
  to be safe for those — plus undocumented private exports
  (PIX/DXGID3D10) as a best-effort fallback. Returning 0 is NOT
  semantically proven safe for arbitrary undocumented HRESULT-shaped
  exports; in practice these resolve to real System32 dxgi
  addresses at runtime so the trap is dead code for them.
- **`dtf_trap_CreateDXGIFactory`** (`(REFIID, void**) -> HRESULT`).
  Zeros the out-pointer, returns `DXGI_ERROR_NOT_FOUND`. Used for
  `CreateDXGIFactory` and `CreateDXGIFactory1`.
- **`dtf_trap_CreateDXGIFactory2`** (`(UINT, REFIID, void**) -> HRESULT`).
  Same shape with the extra flags parameter. Used for
  `CreateDXGIFactory2` and `DXGIGetDebugInterface1`.
- **`dtf_trap_HRESULT_void`** (`() -> HRESULT`). Returns
  `DXGI_ERROR_NOT_FOUND`. Used for `DXGIDeclareAdapterRemovalSupport`.
- **`dtf_trap_HRESULT_HANDLE`** (`(HANDLE) -> HRESULT`). Used for
  `DXGIDisableVBlankVirtualization` (signature
  community-reverse-engineered).

After DllMain successfully resolves System32 dxgi, every pfn for an
export that System32 actually has is overwritten with the real
address. Only exports missing from the host's Windows version stay
pointing at their trap.

---

## Step 3: hooking the CPU APIs

`src/cpu_hooks.cpp` installs inline detours via [MinHook][mh] on
**six** CPU/topology APIs:

- `GetSystemInfo`
- `GetNativeSystemInfo`
- `GetActiveProcessorCount`
- `GetMaximumProcessorCount`
- `GetActiveProcessorGroupCount`
- `GetMaximumProcessorGroupCount`

[mh]: https://github.com/TsudaKageyu/minhook

### Why six APIs, not just `GetSystemInfo`

DXMD itself only imports `GetSystemInfo`. But middleware loaded into
the same process (`bink2w64.dll`, `amd_ags64.dll`, possibly Apex /
PhysX) may dynamically resolve other CPU-discovery APIs through
`GetProcAddress`. Hooking all six with values consistent with one
fake topology (8 logical processors, 1 processor group) means no
code path can see "8 here" and "64 there" and get confused.

### Hook target: the kernel32-resolved address

For each API, we call `GetProcAddress(kernel32, "GetSystemInfo")`.
On modern Windows, that may return either:
- The actual implementation in `kernel32`, OR
- A forwarder/stub that chains through the apiset DLLs to
  `KernelBase.dll`.

Either way, callers that resolve the symbol via kernel32 (whether
by static import or runtime `GetProcAddress`) end up with the same
address that we hook, so they hit our detour. The DXMD binary and
all its bundled middleware import only from `KERNEL32.dll`, so this
is sufficient for the target use case.

**Acknowledged limitation:** a caller that imports directly from
`KernelBase.dll` and lands on a *distinct* implementation address
would bypass this hook. We don't currently cover that, because
nobody in the DXMD process does it.

### Three-pass install (Pass 1: create, Pass 2: publish, Pass 3: enable)

`install_cpu_hooks()` in `cpu_hooks.cpp` is more elaborate than it
looks at first glance:

- **Pass 1**: `MH_CreateHook` for every API. Collect (target, tramp)
  pairs in a local array. **Don't** publish anything to the global
  `g_real_*` trampoline pointers yet. If any *required* hook fails
  to create, `goto fail` — the cleanup loop frees what we created.
- **Pass 2**: publish all trampoline pointers to the `g_real_*`
  globals. Pure memory writes, can't fail meaningfully.
- **Pass 3**: `MH_EnableHook(MH_ALL_HOOKS)` makes everything live.

Why three passes? Because if we published trampolines in Pass 1 and
then had to clean up on a later failure, the published global
pointer would briefly point at a `MH_RemoveHook`-freed trampoline
buffer. Even though nothing should call the detour before EnableHook
(it's not enabled yet), keeping the publish window minimal removes
the entire class of "what if it does, somehow?" concerns.

The 6 topology APIs are treated as an **all-or-nothing required
set**. If any one fails to install, we tear down everything and log
`FIX STATUS: INACTIVE`. The user sees a loud "not active" banner in
their log rather than a half-applied fix that might still crash.
`SetThreadAffinityMask` is opt-in via the INI (`ClampAffinity=1`)
and treated as optional.

### Why MinHook from DllMain (against general advice)

General Windows advice is "don't do non-trivial work in DllMain".
We do, deliberately, because:

- **In DXMD's observed import/init order, our DllMain runs before
  most middleware modules have done their initialization work.**
  Strictly: a dxgi-proxy DLL is guaranteed to load before DXMD's
  entry point, because DXMD statically imports dxgi.dll. Whether
  our DllMain runs before each SIBLING module's DllMain depends on
  the loader's topological walk of the import graph, which is
  module-specific and not a documented loader invariant.
  Empirically (validated by in-game testing) our DllMain runs
  before ApexFramework, PhysX*, fmod, bink, and amd_ags have done
  their init work. Several of those modules import topology APIs
  (visible in dumpbin /imports) and may call them during their own
  init; if we deferred our hook install to a worker thread, the
  worker wouldn't be scheduled before those modules ran their init
  code and the crash would still happen.

- **MinHook's `SuspendThread`-on-all-others loop is safe here**
  because in the normal DXMD startup path we target, no DXMD-
  created worker threads exist yet — MinHook ends up with no
  threads to suspend. (Injected tools or AV instrumentation could
  in principle have created threads earlier; not observed, and
  outside our supported scenario.)

- **MinHook does not call LoadLibrary or take the loader lock
  itself.** It writes to executable code pages we resolve via
  GetProcAddress (whose results are stable in DllMain).

This is the standard pattern for dxgi-proxy game mods (Special K,
ReShade, ENBSeries). Decades of production use across thousands of
games corroborate it.

### What we explicitly DON'T do

- **No `DisableThreadLibraryCalls`.** Combining it with the static
  CRT (`/MT`) is a known proxy-DLL hazard — ReShade explicitly
  avoids it because the static CRT relies on per-thread
  notifications for some internal state.

- **No `/guard:cf`** (Control Flow Guard) in the build. cpu_hooks.cpp
  calls the original APIs through MinHook trampolines stored in
  `g_real_*` function pointers; those trampolines live in runtime-
  allocated executable memory (MinHook uses `VirtualAlloc`). Whether
  CFG accepts such targets depends on Windows version and on whether
  MinHook calls `SetProcessValidCallTargets` to register them — the
  v1.3.3 release we vendor does not. Rather than risk breaking the
  install on some unknown subset of Windows versions, we leave CFG
  off for v1.0.0 (the other mitigations are still on). The static
  CRT brings in some CFG-related load-config metadata anyway, but
  the `IMAGE_DLLCHARACTERISTICS_GUARD_CF` bit is not set on our
  image so the OS loader doesn't enforce CFG on us.

- **No work in DLL_PROCESS_DETACH when `lpReserved != nullptr`**
  (i.e. at process exit). The OS reclaims everything; doing
  cleanup risks loader-lock deadlocks. We only run `detach()` on
  explicit `FreeLibrary`.

---

## Defensive layers

A few of the patterns in the source exist not for the happy path
but for specific edge cases we discovered during review:

### Host process check (`host_is_dxmd()` in `dllmain.cpp`)

If our DLL is accidentally loaded into a non-DXMD process (someone
copies it to the wrong folder, or it gets dropped next to a
different executable), we still forward dxgi correctly but **skip
the CPU hooks**. The CPU hooks are DXMD-specific and we don't want
to alter other apps' thread-pool sizing as a side effect of being
in the wrong process.

This is a LOLBin (living-off-the-land binary) protection: even if
someone tried to repurpose our DLL as a way to lie about CPU count
in some unrelated app, our DLL refuses.

### Long-path-safe `GetModuleFileNameW` (`path_util.cpp`)

Windows has had >MAX_PATH paths since long-path support was added
(opt-in per user). Our DLL has three places that need module paths
(DLL self for INI/log discovery, host EXE for the DXMD check). Each
uses the dynamic-buffer retry pattern from `path_util.cpp`: start
at MAX_PATH, double the buffer up to 32K wchars on truncation, free
cleanly on failure.

The previous fixed-MAX_PATH approach would silently disable INI/log
and host detection on deeply-nested installs. Now everything scales.

### Crash-safe logging (`log.cpp`)

`log_line()` opens the file, writes one line, closes the file —
every time. That's slower than holding it open, but means every
line is committed to the OS file handle the moment it's written
and survives a process crash. (We don't `FlushFileBuffers`, so
this isn't power-loss durable — but the threat is "DXMD crashed
mid-game", not "PC lost power", and crash-survival is sufficient.)
If the game crashes immediately after `FIX STATUS: ACTIVE`, that
line is in the log.

The startup-truncate-then-append pattern means each game run
overwrites the prior log. So "send me your log" from a support
forum gets the current run's diagnostics, not 50 launches' worth.

### Consistent fake topology (`topology.h`/`topology.cpp`)

Every hook reads from one `FakeTopology` struct. Returning
inconsistent values across APIs (8 from `GetSystemInfo`, 64 from
`GetActiveProcessorCount`) would be worse than not hooking at all —
the game might allocate for 8 and schedule for 64. One struct, one
source of truth.

`set_topology()` MUST run before `install_cpu_hooks()`, because it
calls the unhooked APIs via `GetProcAddress` to record the real
values. After hooks are installed, the API code itself is patched —
`GetProcAddress` returns the same exported address as before, but
calling that address enters our detour. Running `set_topology()`
post-hook would feed our own lies back into our "real" topology. The
call order in `dllmain.cpp::attach()` is correct; this is documented
inline as a maintenance invariant.

---

## Build hardening

Release build (`build.ps1`):

- `/MT` — static CRT, so the user doesn't need any VC++ redist
- `/GS /sdl` — stack-buffer checks, additional SDL hardening
- `/W4 /EHsc /std:c++17` — high warning level
- `/DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT` — explicit security mitigations
- **No** `/guard:cf` — incompatible with MinHook (see above)

VERSIONINFO is embedded (`src/version.rc`) so:
- The OS treats the file as a named, versioned project rather than
  an anonymous DLL (helps with AV heuristics).
- Users can verify identity via right-click → Properties → Details
  → "ProductName" should be `dxmd-thread-fix`.
- The install/uninstall scripts use the VERSIONINFO ProductName
  field as a convenience identity check (so upgrades between dtf
  versions don't trigger the foreign-mod-detected path). NOTE: this
  is NOT a cryptographic authenticity check — any DLL can claim
  `ProductName=dxmd-thread-fix`. The scripts use it to avoid
  accidentally overwriting a foreign mod's DLL, not to prove the
  installed DLL is genuinely ours. Use the SHA-256 hashes from the
  GitHub release manifest for that.

---

## Trust surface

What this DLL is allowed to do, at runtime:

- Read `dxmd-thread-fix.ini` next to itself (via
  `GetPrivateProfileIntW`).
- Write `dxmd-thread-fix.log` next to itself, only when `LogLevel > 0`.
- Load the real `dxgi.dll` from the OS system directory returned by
  `GetSystemDirectoryW` (normally `C:\Windows\System32` on a default
  Windows install; can differ on non-default installs).
- Resolve 20 specific exports from that DLL and forward calls.
- Install MinHook detours on six kernel32-resolved APIs.

What this DLL is technically capable of (what the import surface
allows) vs. what the source actually shows it doing:

- It imports `LoadLibraryExW` and `GetProcAddress`, so in principle
  it could load any other DLL on the system. The source shows the
  ONLY use of `LoadLibraryExW` is for the real dxgi.dll at the path
  returned by `GetSystemDirectoryW` (normally `System32\dxgi.dll`)
  — see `dxgi_exports.cpp::load_system_dxgi_and_resolve`. Verify
  by grep.
- It does NOT import `ws2_32`, `wininet`, `winhttp` (no socket /
  HTTP capability — would need a runtime LoadLibrary to get it, and
  the source contains no such call).
- It does NOT statically import `advapi32` and the source contains
  no dynamic LoadLibrary of it (grep for `advapi32` in the source).
  No registry capability via that route.
- It does NOT statically import `kernel32!CreateProcess*`,
  `shell32!ShellExecute*`, and the source contains no dynamic
  LoadLibrary that would resolve them. No process-creation
  capability via that route.
- Project-controlled files this source explicitly opens, writes,
  or loads at runtime:
    * READ:  `dxmd-thread-fix.ini` next to the DLL (via
      `GetPrivateProfileIntW` — values are integers clamped to small
      ranges, no string injection surface).
    * WRITE: `dxmd-thread-fix.log` next to the DLL, only if
      LogLevel > 0 — opened+written+closed per line (see
      `log.cpp::log_line`).
    * LOAD:  the real dxgi.dll from the OS system directory
      returned by `GetSystemDirectoryW` (normally
      `C:\Windows\System32\dxgi.dll`).
  (Loading the real dxgi.dll causes the OS loader to transitively
  map dxgi's own dependencies (ntdll, advapi32, gdi32, etc. — exact
  list varies by Windows version; verify on your system with
  `dumpbin /imports` on the actual System32\dxgi.dll). That's normal
  OS behavior outside this DLL's direct control.)
  An import-table view alone cannot prove path confinement —
  kernel32 already provides CreateFileW/FindFirstFileW/etc., so
  having only kernel32 as an import does NOT mean we can only
  touch the files listed above. The real proof is source review:
  grep the source for `CreateFile`, `OpenFile`, `GetProcAddress`,
  and verify the only paths passed are the INI/log filenames and
  the System32\dxgi.dll absolute path computed at load time.
- It does NOT touch save data (`Documents/`, `%APPDATA%`,
  PSOCache.bin, Steam userdata, or anything Steam-Cloud-tracked).

`dumpbin /imports dist/dxgi.dll` confirms the import surface is
`KERNEL32.dll` only. The full per-release dump is published as
`dxmd-thread-fix-v<VERSION>-imports.txt` on the GitHub release page.

---

## Reading order

If you want to skim the source roughly in order of "what runs first":

1. [`dxgi.def`](dxgi.def) — what we claim to export
2. [`dxgi_stubs.asm`](dxgi_stubs.asm) — the export bodies
3. [`dxgi_exports.cpp`](dxgi_exports.cpp) — `pfn_FOO` initializers and resolver
4. [`dtf_traps.cpp`](dtf_traps.cpp) — the trap functions
5. [`dllmain.cpp`](dllmain.cpp) — entry point and `attach()` sequence
6. [`config.cpp`](config.cpp) — INI parsing
7. [`log.cpp`](log.cpp) — logging
8. [`topology.cpp`](topology.cpp) — fake CPU topology
9. [`cpu_hooks.cpp`](cpu_hooks.cpp) — MinHook detour install and the hook bodies
10. [`path_util.cpp`](path_util.cpp) — shared long-path-safe helper
11. [`version.rc`](version.rc) — VERSIONINFO resource

Each file's header comment is self-contained — you shouldn't need to
read them in any particular order if you're chasing a specific
question.

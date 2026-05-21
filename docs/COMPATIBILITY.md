# Compatibility

## Confirmed working

- **Steam version of DXMD** (the only place this has been tested).
- **Steam overlay** — fine. Steam hooks the D3D swapchain VTable
  after swapchain creation, which is orthogonal to our static-import
  proxy.
- **NVIDIA GPUs** — tested on RTX 4090. The fix is in the
  `GetSystemInfo` / `GetActiveProcessorCount` hook code, not in any
  GPU-specific path, so there's no plausible reason it would behave
  differently on AMD or Intel; but AMD/Intel were not specifically
  validated for this release.
- **Windows 11** — primary test platform.
- **Threadripper 3970X (32C/64T)** — primary test platform, fix
  developed against this hardware.

## Expected to work (untested)

- **Steam achievements** — we don't touch any Steam APIs, don't
  modify save data, don't intercept achievement-relevant code paths.
  Expected compatible, but not specifically verified.
- **Smaller CPUs (8C/16T, 12C/24T)** — should be a harmless no-op
  (we clamp our reported count downward to your real count).
- **Larger CPUs (96C/192T+)** — the topology hooks return a
  single-group, 8-CPU view, so should work.

## Known conflicts

| Other mod | Conflict | Workaround |
|---|---|---|
| ReShade (configured as `dxgi.dll` proxy) | Both want to be `dxgi.dll`. | Configure ReShade as `d3d11.dll` proxy instead. |
| Special K | Both proxy `dxgi.dll`. Special K's HDR retrofit specifically requires its dxgi proxy. | No good workaround currently — use one or the other. |
| ENBSeries | Both proxy `dxgi.dll`. | Use only one. |

## Not supported

- **Microsoft Store / Xbox Game Pass** install of DXMD — the app
  sandbox blocks `dxgi.dll` proxying.
- **Pirated copies** — we can't help if your install is itself broken
  by a crack patcher.
- **DXMD demo / older patches** — only build `1.19.801.0` (the final
  2017 patch) has been tested. Earlier patches may differ in import
  table layout. The DLL itself does NOT runtime-check the DXMD build
  version, so it'll still try to install on other builds; the only
  enforced check is that the host EXE is named `DXMD.exe`.

## Supported platforms

| | Status |
|---|---|
| DXMD build | Tested on 1.19.801.0 (final 2017 patch). The DLL does NOT enforce this at runtime; other builds may work but are untested. |
| OS | Windows 11 — tested. Windows 10 — expected to work (the DLL imports nothing Windows-10-specific) but not specifically validated this release. Windows 7 SP1 / 8.1 — build targets that baseline but neither tested. Older Windows ships an older System32 dxgi.dll with a smaller export set; the trap fallback (`src/dxmd_thread_fix.cpp` SECTION 1) is designed to handle missing exports cleanly, but that path is itself less-tested. If you're on Win7/8.1, please file a bug report with `dxmd-thread-fix.log` attached if you see any `WARN: real dxgi missing export <name>` lines. |
| Architecture | x64 — the only one DXMD ships in |
| CPU | Any x86-64 CPU. The fix is most useful at 16+ logical processors but is harmless on smaller CPUs. |

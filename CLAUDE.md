# CLAUDE.md

Guidance for working in the Pulse repo.

## Hard rules

### ASCII only in everything (no exceptions)

Never put non-ASCII or "smart" characters in ANY file: source (.cpp/.hpp/.h),
shaders (.hlsl), build scripts (.bat), CMake, comments, string literals,
docs, or commit messages. This includes:

- em-dash and en-dash: use a comma, "to", or a plain ASCII hyphen instead
- curly/smart quotes: use straight `'` and `"`
- ellipsis glyph: use three dots `...`
- arrows and other symbol glyphs: write words ("to") or plain ASCII (`->`)
- non-breaking spaces and any other byte >= 0x80

Why this is a hard rule: non-ASCII bytes routinely break the Edit/patch tooling
(the old_string fails to match), corrupt MSVC string literals via codepage
misreads (a garbled window title is the classic symptom), and make builds and
edits fail in ways that are slow to diagnose. Every file in this repo must stay
pure 7-bit ASCII.

To check before finishing a change:
```powershell
# lists any file containing a byte >= 0x80 (should print nothing)
Get-ChildItem -Recurse -File -Include *.cpp,*.hpp,*.h,*.hlsl,*.bat,*.ps1,*.md,*.txt `
  -Path src,tools,assets,CMakeLists.txt |
  Where-Object { ([IO.File]::ReadAllBytes($_.FullName) | Where-Object { $_ -ge 0x80 }).Count -gt 0 } |
  Select-Object FullName
```
The MSVC build also passes `/utf-8`, but the rule above is the real safeguard:
do not introduce the characters in the first place.

## Build

Toolchain: MSVC (VS 2022/2026) + Ninja + Windows SDK; vendored deps under
`third_party/` (run `tools\Provision-ThirdParty.bat` once on a clean checkout).
All project scripts are .bat (no .ps1) so they run by double-click with no
execution-policy friction.
The cmake/ninja that may be on PATH are MinGW, so the scripts load the VS x64
developer environment (vcvars64) before configuring.

- `Build.bat [Config] [Target]` - dev build into `build/` (default target
  `pulse_window`, config `RelWithDebInfo`). `Build.bat Debug` turns on the D3D12
  debug layer + GPU-based validation.
- `Package.bat [Config]` - builds a self-contained `dist/` folder (default
  Release): the exe plus every runtime DLL and the shaders, runnable from any
  location.
- Runnable exes today: `build/pulse_window.exe` (windowed spinning-cube demo)
  and `build/engine_smoke.exe` (headless validation stages that write BMPs). The
  real game exe `pulse` does not link yet; it needs the game submission port.

## Orientation

Clean-sheet Direct3D 12 + DXR engine. The canonical design is
`docs/Plan_PULSE_engine_and_assets.txt` (engine in Part I/II, asset pipeline in
Part III); the experience target is `docs/PROTOTYPE_SPEC.md`; project rules are in
`docs/PROJECT_RULES.md`.

Engine layout:
- `src/Engine/RHI/` - D3D12 abstraction: Device, Heaps (bindless), Resource,
  Commands, Pipeline (bindless root sig + PSO cache), Uploader, Swapchain,
  Readback, AgilitySDK export.
- `src/Engine/RenderGraph/` - pass authoring, auto-barriers, pass culling,
  transient pool, named-resource dump (the `--render-pass` hook).
- `src/Engine/Shaders/` - dxc compiler wrapper (compile-at-load in dev).
- `src/Engine/Core/` - Log, Image (BMP), Mat (row-major, reverse-Z), Paths.
- `src/Platform/` - Win32 Window.
- `assets/shaders/` - HLSL (SM6.6 bindless; vertices pulled from a structured
  buffer by SV_VertexID, resources indexed via ResourceDescriptorHeap).
- `tools/dev/` - engine_smoke and pulse_window bring-up drivers.

Conventions: bindless everywhere (SM6.6 `ResourceDescriptorHeap`), reverse-Z
depth (clear 0, GREATER_EQUAL), no input-assembler vertex layouts. A missing
required shader/asset/engine path must fail loudly, never silently substitute
(see `docs/PROJECT_RULES.md`).

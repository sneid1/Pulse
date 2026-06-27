# third_party, pinned vendored dependencies

This tree is **provisioned, not committed**: the binaries are large and fully
re-creatable from pinned upstream versions. `tools\Provision-ThirdParty.bat` is
the source of truth, run it from a clean checkout to recreate everything here.

```bat
tools\Provision-ThirdParty.bat
```

## Manifest (pinned)

| Dep | Version | Source | Used for |
|-----|---------|--------|----------|
| DirectX Agility SDK | `1.619.3` (`D3D12_SDK_VERSION = 619`) | NuGet `Microsoft.Direct3D.D3D12` | Current D3D12 features + redistributable debug layer (no OS "Graphics Tools" needed). Ships `agility/bin/x64/{D3D12Core,d3d12SDKLayers}.dll` next to the exe under `D3D12/`. Includes the `d3dx12` helper headers. |
| DirectX Shader Compiler | `1.9.2602.24` | NuGet `Microsoft.Direct3D.DXC` | `dxc` HLSL to DXIL (SM6.6 + RT). `dxc/lib/x64/dxcompiler.lib` linked; `dxc/bin/x64/{dxcompiler,dxil}.dll` shipped next to the exe. |
| DirectXTex (texconv) | `may2026` | GitHub release | Offline BC7/BC5 DDS transcode, wrapped by `--import-asset`. |
| cgltf | `master` (single header) | github.com/jkuhlmann/cgltf | glTF 2.0 `.glb` loader. |
| stb_vorbis | `master` (single file) | github.com/nothings/stb | OGG decode for the music bed. |

## Layout

```
agility/include/        Agility D3D12 headers (d3d12.h, d3dx12/*, d3d12sdklayers.h, ...), included BEFORE the Windows SDK
agility/bin/x64/         D3D12Core.dll, d3d12SDKLayers.dll (+ pdbs), copied to build/D3D12/ post-build
dxc/include/             dxcapi.h, d3d12shader.h, ...
dxc/lib/x64/             dxcompiler.lib, dxil.lib
dxc/bin/x64/             dxc.exe, dxcompiler.dll, dxil.dll, DLLs copied next to the exe post-build
cgltf/cgltf.h
stb/stb_vorbis.c
directxtex/texconv.exe
```

Licensing: Agility SDK and DXC are MIT (Microsoft); DirectXTex is MIT; cgltf is
MIT; stb is public-domain/MIT. All redistributable.

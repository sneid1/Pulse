@echo off
setlocal enabledelayedexpansion

rem Provision third_party\ for the Pulse engine from pinned upstream versions.
rem Re-creatable from a clean checkout; third_party\ is gitignored (binaries).
rem See third_party\README.md for the manifest. Uses Windows built-ins (curl, tar,
rem robocopy) only.

set "TOOLS=%~dp0"
set "ROOT=%TOOLS%.."
set "TP=%ROOT%\third_party"
set "DL=%TP%\_dl"

rem Pinned versions (bump deliberately; the engine is validated against these).
set "AGILITY=1.619.3"
set "DXC=1.9.2602.24"
set "DXTEX=may2026"

if not exist "%DL%" mkdir "%DL%"

echo Downloading pinned dependencies...
curl -sL -o "%DL%\agility.zip" "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/%AGILITY%/microsoft.direct3d.d3d12.%AGILITY%.nupkg" || ( echo download failed & exit /b 1 )
curl -sL -o "%DL%\dxc.zip" "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.dxc/%DXC%/microsoft.direct3d.dxc.%DXC%.nupkg" || ( echo download failed & exit /b 1 )
curl -sL -o "%DL%\texconv.exe" "https://github.com/microsoft/DirectXTex/releases/download/%DXTEX%/texconv.exe" || ( echo download failed & exit /b 1 )
curl -sL -o "%DL%\cgltf.h" "https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h" || ( echo download failed & exit /b 1 )
curl -sL -o "%DL%\stb_vorbis.c" "https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c" || ( echo download failed & exit /b 1 )

echo Extracting + laying out third_party...
mkdir "%DL%\agility_x" 2>nul
mkdir "%DL%\dxc_x" 2>nul
tar -xf "%DL%\agility.zip" -C "%DL%\agility_x" || ( echo extract failed & exit /b 1 )
tar -xf "%DL%\dxc.zip" -C "%DL%\dxc_x" || ( echo extract failed & exit /b 1 )

set "AG=%DL%\agility_x\build\native"
set "DX=%DL%\dxc_x\build\native"

rem Reset targets so re-provisioning is clean.
for %%D in (agility dxc cgltf stb directxtex) do if exist "%TP%\%%D" rmdir /s /q "%TP%\%%D"

robocopy "%AG%\include" "%TP%\agility\include" /E /NJH /NJS /NDL /NFL /NP >nul
if errorlevel 8 ( echo copy failed & exit /b 1 )
robocopy "%AG%\bin\x64" "%TP%\agility\bin\x64" D3D12Core.dll D3D12Core.pdb d3d12SDKLayers.dll d3d12SDKLayers.pdb /NJH /NJS /NDL /NFL /NP >nul
if errorlevel 8 ( echo copy failed & exit /b 1 )
robocopy "%DX%\include" "%TP%\dxc\include" /E /NJH /NJS /NDL /NFL /NP >nul
if errorlevel 8 ( echo copy failed & exit /b 1 )
robocopy "%DX%\bin\x64" "%TP%\dxc\bin\x64" dxc.exe dxcompiler.dll dxil.dll /NJH /NJS /NDL /NFL /NP >nul
if errorlevel 8 ( echo copy failed & exit /b 1 )
robocopy "%DX%\lib\x64" "%TP%\dxc\lib\x64" dxcompiler.lib dxil.lib /NJH /NJS /NDL /NFL /NP >nul
if errorlevel 8 ( echo copy failed & exit /b 1 )

mkdir "%TP%\cgltf" 2>nul
copy /y "%DL%\cgltf.h" "%TP%\cgltf\cgltf.h" >nul
mkdir "%TP%\stb" 2>nul
copy /y "%DL%\stb_vorbis.c" "%TP%\stb\stb_vorbis.c" >nul
mkdir "%TP%\directxtex" 2>nul
copy /y "%DL%\texconv.exe" "%TP%\directxtex\texconv.exe" >nul

rmdir /s /q "%DL%"
echo third_party provisioned (Agility %AGILITY%, DXC %DXC%, DirectXTex %DXTEX%).
endlocal

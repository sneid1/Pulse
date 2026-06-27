@echo off
setlocal enabledelayedexpansion

rem Build a self-contained dist\ folder: the windowed exe plus every runtime
rem dependency (Agility + dxc DLLs and the shaders), runnable standalone from any
rem location. Built in a separate dir so the dev build\ stays untouched.
rem Usage:  Package.bat [Config]   (Config: Release default, or RelWithDebInfo/Debug)

set "ROOT=%~dp0"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
set "PKGBUILD=%ROOT%build\pkg"
set "DIST=%ROOT%dist"

rem --- load the Visual Studio x64 developer environment ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [Package] vcvars64.bat not found under "%VSROOT%"
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul

rem --- configure + build the windowed exe ---
cmake -S "%ROOT%." -B "%PKGBUILD%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 ( echo [Package] cmake configure failed & exit /b 1 )
if not exist "%ROOT%assets\bumstrum\fps_ak_animated\scene.gltf" (
    echo [Package] animated AK asset missing: assets\bumstrum\fps_ak_animated\scene.gltf
    exit /b 1
)
if not exist "%ROOT%assets\bumstrum\fps_animated_carbine\scene.gltf" (
    echo [Package] animated carbine asset missing: assets\bumstrum\fps_animated_carbine\scene.gltf
    exit /b 1
)
cmake --build "%PKGBUILD%" --target pulse
if errorlevel 1 ( echo [Package] build failed & exit /b 1 )

rem --- assemble dist\ (exe + runtime DLLs + runtime-only game assets) ---
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%\D3D12"
copy /y "%PKGBUILD%\pulse.exe"         "%DIST%\PULSE.exe"        >nul
copy /y "%PKGBUILD%\dxcompiler.dll"    "%DIST%\"                 >nul
copy /y "%PKGBUILD%\dxil.dll"          "%DIST%\"                 >nul
copy /y "%PKGBUILD%\D3D12\*.dll"       "%DIST%\D3D12\"           >nul

mkdir "%DIST%\assets" 2>nul
copy /y "%ROOT%assets\CREDITS.txt" "%DIST%\assets\" >nul 2>nul
copy /y "%ROOT%assets\README.md"   "%DIST%\assets\" >nul 2>nul

rem Audio runtime banks only. Raw Sonniss bundles and REAPER/source stems stay out of dist.
robocopy "%ROOT%assets\audio" "%DIST%\assets\audio" *.wav *.txt /LEV:1 >nul
if errorlevel 8 ( echo [Package] audio copy failed & exit /b 1 )
robocopy "%ROOT%assets\audio\music" "%DIST%\assets\audio\music" *.wav >nul
if errorlevel 8 ( echo [Package] music copy failed & exit /b 1 )

rem Core renderer/runtime data.
robocopy "%ROOT%assets\fonts"    "%DIST%\assets\fonts"    /E >nul
if errorlevel 8 ( echo [Package] font copy failed & exit /b 1 )
robocopy "%ROOT%assets\icons"    "%DIST%\assets\icons"    /E >nul
if errorlevel 8 ( echo [Package] icon copy failed & exit /b 1 )
robocopy "%ROOT%assets\shaders"  "%DIST%\assets\shaders"  /E >nul
if errorlevel 8 ( echo [Package] shader copy failed & exit /b 1 )
robocopy "%ROOT%assets\textures" "%DIST%\assets\textures" /E >nul
if errorlevel 8 ( echo [Package] texture copy failed & exit /b 1 )

rem Static fallback meshes. Skip generated source backups and obsolete baked viewmodels.
robocopy "%ROOT%assets\models" "%DIST%\assets\models" /E /XD "%ROOT%assets\models\_procedural_backup" /XF pulse_ak47_viewmodel.* pulse_left_hand_viewmodel.* pulse_right_hand_viewmodel.* pulse_pistol_viewmodel.* bumstrum_ak47_viewmodel.glb bumstrum_carbine_viewmodel.glb bumstrum_pistol_viewmodel.glb >nul
if errorlevel 8 ( echo [Package] model copy failed & exit /b 1 )

rem Authored first-person weapon rigs used by config/pulse.weapons.
for %%D in (fps_pistol_animated fps_ak_animated fps_animated_carbine fps_smg9_animated shotgun_animated sniper_animated) do (
    robocopy "%ROOT%assets\bumstrum\%%D" "%DIST%\assets\bumstrum\%%D" /E >nul
    if errorlevel 8 ( echo [Package] bumstrum %%D copy failed & exit /b 1 )
)

rem Runtime enemy/environment Meshy assets. Keep extracted GLBs/textures; skip source zips/previews/editor formats.
robocopy "%ROOT%assets\meshy\enemies\rigged_concepts" "%DIST%\assets\meshy\enemies\rigged_concepts" *.glb >nul
if errorlevel 8 ( echo [Package] rigged enemy copy failed & exit /b 1 )
robocopy "%ROOT%assets\meshy\enemies\shared" "%DIST%\assets\meshy\enemies\shared" /E >nul
if errorlevel 8 ( echo [Package] shared enemy texture copy failed & exit /b 1 )
rem Only the concept GLBs actually loaded at runtime (PulseGame.cpp ModelAsset tables):
rem enemies 002 + 010 and bosses 015/016/017. The other pulse_enemy_concepts folders are raw
rem generator inputs that were rigged into assets\meshy\enemies\rigged_concepts and are never
rem opened at runtime. Ship only model_glb.glb (textures are embedded; the loose texture_*.png /
rem obj / mtl / previews beside it are redundant).
set "EIROOT=%ROOT%assets\meshy\enemy_inputs\meshy_generated_models"
set "EIDST=%DIST%\assets\meshy\enemy_inputs\meshy_generated_models"
for %%P in (
    "pulse_enemy_concepts\002_enemy_gunner_vertical_armored_caster"
    "pulse_enemy_concepts\010_enemy_shielded_elite_gunner_integrated_defense"
    "pulse_boss_concepts\015_boss_foundry_null_forge_marshal"
    "pulse_boss_concepts\016_boss_furnace_crucible_tyrant"
    "pulse_boss_concepts\017_boss_reliquary_saint_engine_oracle"
) do (
    robocopy "%EIROOT%\%%~P" "%EIDST%\%%~P" model_glb.glb >nul
    if errorlevel 8 ( echo [Package] enemy_inputs %%~P copy failed & exit /b 1 )
)
mkdir "%DIST%\assets\meshy\raw" 2>nul
copy /y "%ROOT%assets\meshy\raw\slice_cyan_crystal.glb"     "%DIST%\assets\meshy\raw\" >nul
copy /y "%ROOT%assets\meshy\raw\slice_monument_obelisk.glb" "%DIST%\assets\meshy\raw\" >nul
copy /y "%ROOT%assets\meshy\raw\slice_gateway_arch.glb"     "%DIST%\assets\meshy\raw\" >nul

rem Current arena path: Quaternius/brutalist packs plus the sci-fi hero kit and PolyHaven PBR maps.
rem texture_0_*.png and preview_*.png next to each model_glb.glb are redundant: the GLB embeds its
rem own textures and the runtime only opens model_glb.glb. (base_env_*.png decals stay - different name.)
rem The Quaternius kits also ship duplicate export folders the engine never loads (it only reads each
rem kit's glTF\ subdir, with textures co-located there): drop the Textures\, Planet Textures\,
rem glTF (Godot)\ and Textures[Pro]\ duplicates via /XD.
set "QROOT=%ROOT%assets\packs\pulse_environment\quaternius"
robocopy "%ROOT%assets\packs\pulse_environment" "%DIST%\assets\packs\pulse_environment" /E ^
    /XF *.fbx *.obj *.mtl *.blend *.blend1 *.jsonl *.csv *.gif texture_0_*.png preview_*.png ^
    /XD "%QROOT%\Sci-Fi Essentials Kit[Pro]\Textures" "%QROOT%\Sci-Fi Essentials Kit[Pro]\Planet Textures" "%QROOT%\Modular SciFi MegaKit[Pro]\glTF (Godot)" "%QROOT%\Modular SciFi MegaKit[Pro]\Textures[Pro]" >nul
if errorlevel 8 ( echo [Package] environment pack copy failed & exit /b 1 )
robocopy "%ROOT%assets\external\polyhaven" "%DIST%\assets\external\polyhaven" /E >nul
if errorlevel 8 ( echo [Package] polyhaven copy failed & exit /b 1 )
for %%D in (scifi_tileset power_reactor hallway_straight_01 hallway_cross hallway_t) do (
    robocopy "%ROOT%assets\external\sketchfab_scifi\%%D" "%DIST%\assets\external\sketchfab_scifi\%%D" /E >nul
    if errorlevel 8 ( echo [Package] sketchfab_scifi %%D copy failed & exit /b 1 )
)

mkdir "%DIST%\config" 2>nul
copy /y "%ROOT%config\pulse.*"        "%DIST%\config\"          >nul 2>nul

rem --- optimize shipped GLBs: downscale embedded 4K textures to per-asset caps ---
rem The engine glTF loader is stb PNG/JPEG only (no Draco/meshopt/KTX2), so the bake keeps
rem PNG/JPEG and only prunes/dedups/downscales geometry-safe; see tools\assets\glb-bake\bake.mjs.
where node >nul 2>nul || ( echo [Package] node not found - required for the asset bake & exit /b 1 )
where gltf-transform >nul 2>nul || ( echo [Package] gltf-transform missing - run: npm install -g @gltf-transform/cli & exit /b 1 )
node "%ROOT%tools\assets\glb-bake\bake.mjs" "%DIST%\assets"
if errorlevel 1 ( echo [Package] asset bake failed & exit /b 1 )

echo.
echo [Package] dist ready (%CONFIG%):
echo   "%DIST%\PULSE.exe"
endlocal

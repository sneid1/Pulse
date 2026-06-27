@echo off
setlocal

rem Author all runtime assets as part of the build (no runtime fallbacks; a missing
rem asset fails loud per PROJECT_RULES.md). Meshes come from the deterministic
rem Blender scripts; the grey-box world textures from the texture authoring tool.

set "ROOT=%~dp0"
set "BLENDER=%ROOT%tools\blender\blender-5.1.2-windows-x64\blender.exe"
if not exist "%BLENDER%" (
    echo [assets] bundled Blender not found at "%BLENDER%"
    exit /b 1
)

echo [assets] authoring weapon + hands...
"%BLENDER%" -b --python "%ROOT%tools\blender\build_pulse_weapon.py" >nul
if errorlevel 1 ( echo [assets] weapon authoring failed & exit /b 1 )

echo [assets] authoring enemies...
"%BLENDER%" -b --python "%ROOT%tools\blender\build_pulse_enemies.py" >nul
if errorlevel 1 ( echo [assets] enemy authoring failed & exit /b 1 )

echo [assets] authoring world textures...
python "%ROOT%tools\author_world_textures.py"
if errorlevel 1 ( echo [assets] texture authoring failed & exit /b 1 )

echo [assets] done.
endlocal

@echo off
rem Open a PULSE weapon EDITOR in the vendored Blender to modify an ORIGINAL in-game weapon.
rem   open_weapon_editor.bat            -> opens the pistol
rem   open_weapon_editor.bat ak         -> opens the rifle  (names: pistol ak carbine smg9 shotgun sniper)
rem Rebuild the editor .blends from source first with: setup_weapon_editor.py
set "HERE=%~dp0"
set "W=%~1"
if "%W%"=="" set "W=pistol"
"%HERE%blender-5.1.2-windows-x64\blender.exe" "%HERE%weapon_edit_%W%.blend"

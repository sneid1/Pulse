@echo off
setlocal enabledelayedexpansion

rem Build the Pulse game/engine with MSVC + Ninja and produce a runnable exe.
rem Usage:  Build.bat [Config] [Target]
rem   Config : Debug | RelWithDebInfo (default) | Release
rem   Target : pulse (default, the game) | pulse_window | engine_smoke | pulse_engine
rem Output exe: build\pulse.exe  (run from a console; it loads assets from ..\assets)
rem NOTE: run Author-Assets.bat once first so the required assets exist.

set "ROOT=%~dp0"
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=RelWithDebInfo"
set "TARGET=%~2"
if "%TARGET%"=="" set "TARGET=pulse"

rem --- locate and load the Visual Studio x64 developer environment ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
)
if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo [Build] vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)
call "%VCVARS%" >nul

rem --- configure + build ---
cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
if errorlevel 1 ( echo [Build] cmake configure failed & exit /b 1 )

cmake --build "%ROOT%build" --target %TARGET%
if errorlevel 1 ( echo [Build] build failed & exit /b 1 )

rem Produce the adaptive music stems from the higher-quality no-composer pipeline.
rem Do not silently fall back to the engine's placeholder music bake.
if exist "%ROOT%build\pulse.exe" (
    set "MISSING_MUSIC=0"
    for %%B in (foundry furnace reliquary) do (
        for %%M in (bed bass drums pressure boss overpulse) do (
            if not exist "%ROOT%assets\audio\music\%%B_%%M.wav" set "MISSING_MUSIC=1"
        )
    )
    if not exist "%ROOT%assets\audio\music\hub_bed.wav" set "MISSING_MUSIC=1"
    for %%S in (room_clear reward boss_intro overpulse run_win run_lose sector_foundry sector_furnace sector_reliquary) do (
        if not exist "%ROOT%assets\audio\music\stinger_%%S.wav" set "MISSING_MUSIC=1"
    )
    if "!MISSING_MUSIC!"=="1" (
        echo [assets] producing adaptive music v3 bank...
        powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\reaper\render_pulse_music_headless.ps1"
        if errorlevel 1 ( echo [assets] music export failed & exit /b 1 )
    )
)

rem Produce authored weapon fire banks from the offline layered gunshot producer
rem if any required bank is absent. Existing complete banks are not rebuilt here.
if exist "%ROOT%build\pulse.exe" (
    set "MISSING_GUN_BANK=0"
    for %%G in (sfx_fire.wav sfx_fire_ak47.wav sfx_fire_ak47_7.wav sfx_fire_carbine.wav sfx_fire_carbine_7.wav sfx_fire_pistol.wav sfx_fire_pistol_5.wav sfx_fire_pulse_smg.wav sfx_fire_pulse_smg_7.wav sfx_fire_machine_pistol.wav sfx_fire_machine_pistol_7.wav sfx_fire_marksman.wav sfx_fire_marksman_4.wav sfx_fire_scattergun.wav sfx_fire_scattergun_7.wav sfx_fire_railbolt.wav sfx_fire_railbolt_4.wav) do (
        if not exist "%ROOT%assets\audio\%%G" set "MISSING_GUN_BANK=1"
    )
    if "!MISSING_GUN_BANK!"=="1" (
        echo [assets] producing layered gunshot banks...
        python "%ROOT%tools\audio\pulse_gunshot_producer.py"
        if errorlevel 1 ( echo [assets] gunshot export failed & exit /b 1 )
    )
)

rem Bake the required non-music utility SFX if missing. Existing authored gunshots
rem and music stems are kept by --bake-audio.
if exist "%ROOT%build\pulse.exe" (
    set "MISSING_SFX=0"
    for %%S in (sfx_config.wav sfx_dash.wav sfx_fire.wav sfx_hit.wav sfx_kill.wav sfx_hurt.wav sfx_dryfire.wav sfx_reload_start.wav sfx_reload_end.wav sfx_pickup.wav) do (
        if not exist "%ROOT%assets\audio\%%S" set "MISSING_SFX=1"
    )
    if "!MISSING_SFX!"=="1" (
        echo [assets] baking missing utility SFX...
        "%ROOT%build\pulse.exe" --bake-audio "%ROOT%assets\audio" >nul
        if errorlevel 1 ( echo [assets] SFX bake failed & exit /b 1 )
    )
)

rem Produce authored per-weapon reload/action banks. Live guns require these in
rem --weapon-test; locked guns get banks too so future unlocks do not inherit generic foley.
if exist "%ROOT%build\pulse.exe" (
    set "MISSING_RELOAD_BANK=0"
    for %%W in (pistol ak47 carbine pulse_smg machine_pistol scattergun marksman railbolt) do (
        for %%E in (dry equip reload_start mag_out mag_in reload_end bolt shell) do (
            if not exist "%ROOT%assets\audio\sfx_weapon_%%W_%%E.wav" set "MISSING_RELOAD_BANK=1"
            if not exist "%ROOT%assets\audio\sfx_weapon_%%W_%%E_2.wav" set "MISSING_RELOAD_BANK=1"
        )
    )
    if "!MISSING_RELOAD_BANK!"=="1" (
        echo [assets] producing weapon reload banks...
        python "%ROOT%tools\audio\pulse_reload_producer.py"
        if errorlevel 1 ( echo [assets] reload export failed & exit /b 1 )
    )
)

rem Produce authored enemy combat/attack banks. These keep enemy tells, releases,
rem impacts, hurt bodies, and deaths out of the player hit/kill confirmation bus.
if exist "%ROOT%build\pulse.exe" (
    set "MISSING_ENEMY_BANK=0"
    for %%K in (rusher ranged tank stalker boss) do (
        for %%E in (telegraph shot impact beam lunge melee_hit hurt death boss_burst) do (
            if not exist "%ROOT%assets\audio\sfx_enemy_%%K_%%E.wav" set "MISSING_ENEMY_BANK=1"
            if not exist "%ROOT%assets\audio\sfx_enemy_%%K_%%E_3.wav" set "MISSING_ENEMY_BANK=1"
        )
    )
    if "!MISSING_ENEMY_BANK!"=="1" (
        echo [assets] producing enemy combat banks...
        python "%ROOT%tools\audio\pulse_enemy_sfx_producer.py"
        if errorlevel 1 ( echo [assets] enemy SFX export failed & exit /b 1 )
    )
)

rem Produce the player-feedback / ability / UI banks (sfx_fb_*). These are tactile
rem player-bus cues (hitmarker, crit, kill, abilities, charge-ready, shield, pickups,
rem UI, run win/lose) kept distinct from the enemy/world banks without arcade chimes.
if exist "%ROOT%build\pulse.exe" (
    set "MISSING_FB_BANK=0"
    for %%E in (hitmarker hit_crit kill kill_elite dash jump ability_tactical ability_ultimate charge_ready explosion shield_absorb shield_break low_health pickup_health pickup_shield pickup_ammo pickup_scrap pickup_powerup ui_move ui_confirm ui_cancel ui_reward run_win run_lose) do (
        if not exist "%ROOT%assets\audio\sfx_fb_%%E.wav" set "MISSING_FB_BANK=1"
    )
    if "!MISSING_FB_BANK!"=="1" (
        echo [assets] producing player-feedback banks...
        python "%ROOT%tools\audio\pulse_player_sfx_producer.py"
        if errorlevel 1 ( echo [assets] player feedback export failed & exit /b 1 )
    )
)

rem Stage the authored runtime audio next to the executable as well as keeping the
rem source assets under assets\audio for iteration.
if exist "%ROOT%build\pulse.exe" (
    if not exist "%ROOT%build\assets\audio" mkdir "%ROOT%build\assets\audio"
    for %%A in ("%ROOT%assets\audio\sfx_*.wav") do (
        copy /Y "%%~fA" "%ROOT%build\assets\audio\" >nul
        if errorlevel 1 ( echo [assets] audio staging failed & exit /b 1 )
    )
    if not exist "%ROOT%build\assets\audio\music" mkdir "%ROOT%build\assets\audio\music"
    for %%A in ("%ROOT%assets\audio\music\*.wav") do (
        copy /Y "%%~fA" "%ROOT%build\assets\audio\music\" >nul
        if errorlevel 1 ( echo [assets] music staging failed & exit /b 1 )
    )
)

rem Mix gate: measure every staged bank and FAIL the build on any clipping. This makes
rem "zero clipping" a verifiable build invariant, not an ears-only hope. Skipped only if
rem python is unavailable (the producers above already require it when banks are missing).
if exist "%ROOT%build\pulse.exe" (
    where python >nul 2>nul
    if not errorlevel 1 (
        echo [assets] validating adaptive music contract...
        python "%ROOT%tools\audio\pulse_music_validate.py" --quiet "%ROOT%assets\audio\music"
        if errorlevel 1 ( echo [assets] music validation FAILED & exit /b 1 )
        echo [assets] validating audio levels [zero-clip gate]...
        python "%ROOT%tools\audio\pulse_audio_validate.py" --quiet "%ROOT%assets\audio\sfx_*.wav" "%ROOT%assets\audio\music\*.wav"
        if errorlevel 1 ( echo [assets] audio validation FAILED ^(clipping detected^) & exit /b 1 )
    )
)

rem Transcode the sourced PBR textures to BCn DDS (BC7/BC5) via --import-asset. The
rem scan is idempotent (skips up-to-date DDS), so run it every build to pick up any
rem newly added asset texture sets under assets\external.
if exist "%ROOT%build\pulse.exe" (
    echo [assets] transcoding PBR textures to BCn DDS...
    "%ROOT%build\pulse.exe" --import-assets-all >nul
)

echo.
echo [Build] OK  (%CONFIG%, target %TARGET%)
echo [Build] Run:  "%ROOT%build\%TARGET%.exe"
endlocal

# PULSE Gunplay Contract

This is the implementation contract for AAA-style PULSE weapon feel. Reward/catalog
metadata stays in `config/pulse.content`; weapon feel, viewmodel, recoil, VFX, and
readiness live in `config/pulse.weapons`.

## Live Rule

A weapon can enter the reward pool only when its `WeaponProfile` has:

- valid damage, cadence, magazine, reserve, and reload values
- an authored first-person viewmodel path
- semantic animation windows for idle, fire, and reload
- muzzle hints for flash/tracer projection
- a per-weapon fire bank target
- per-weapon action banks for dry-fire, equip, reload stages, shell insert, and bolt/ready motion

No static or unrelated viewmodel fallback is allowed. Designed-but-incomplete guns stay
in the catalog for dumps/tests, but are excluded from reward rolls.

## Current Profiles

- `pistol`: live semi-auto sidearm, Bumstrum FPS pistol rig.
- `ak47`: live AK-47, 600 RPM full-auto, Bumstrum FPS AK rig.
- `carbine`: live tactical 3-round burst carbine, Bumstrum FPS carbine rig.
- `pulse_smg`: live mobility spray gun, Bumstrum FPS SMG9 rig.
- `scattergun`: live close pellet shotgun, Bumstrum shotgun rig.
- `marksman`: live bolt-action precision sniper, Bumstrum sniper rig.
- `machine_pistol`: locked until a unique compact automatic sidearm rig/audio bank exists.
- `railbolt`: locked until a unique projectile/rail weapon rig, charge cue, trail, and bank exist.

## Fire Bank Production

Live weapon fire banks are produced by:

```powershell
python tools\audio\pulse_gunshot_producer.py
```

The producer writes the default bank plus all live per-weapon banks:

- `sfx_fire.wav` through `sfx_fire_7.wav`
- `sfx_fire_pistol.wav` through `sfx_fire_pistol_5.wav`
- `sfx_fire_ak47.wav` through `sfx_fire_ak47_7.wav`
- `sfx_fire_carbine.wav` through `sfx_fire_carbine_7.wav`
- `sfx_fire_pulse_smg.wav` through `sfx_fire_pulse_smg_7.wav`
- `sfx_fire_marksman.wav` through `sfx_fire_marksman_4.wav`
- `sfx_fire_scattergun.wav` through `sfx_fire_scattergun_7.wav`

Each bank is a short mono 48 kHz round-robin set with source body, high-band crack,
low thump, fast tail, and per-weapon peak targets. Do not let a live weapon fall
back to `sfx_fire.wav`; a missing per-weapon bank is a validation failure.

## Reload Bank Production

Per-weapon reload/action banks are produced by:

```powershell
python tools\audio\pulse_reload_producer.py
```

The producer writes three round-robin variants for every weapon profile, including
locked future weapons:

- `sfx_weapon_<weapon>_dry.wav` through `_2`
- `sfx_weapon_<weapon>_equip.wav` through `_2`
- `sfx_weapon_<weapon>_reload_start.wav` through `_2`
- `sfx_weapon_<weapon>_mag_out.wav` through `_2`
- `sfx_weapon_<weapon>_mag_in.wav` through `_2`
- `sfx_weapon_<weapon>_reload_end.wav` through `_2`
- `sfx_weapon_<weapon>_bolt.wav` through `_2`
- `sfx_weapon_<weapon>_shell.wav` through `_2`

These banks are procedural foley-style renders: latches, magazine bodies, spring
ticks, bolt/slide motion, shotgun shell handling, hand cloth, and pulse/rail
energy-cell servo layers. Magazine reloads trigger staged `reload_start`,
`mag_out`, `mag_in`, and `reload_end` cues; per-shell weapons trigger
`reload_start`, repeated `shell` inserts, and `reload_end`. Live weapons must not
fall back to the generic dry-fire or reload utility samples for player-facing
weapon actions.

## Validation Commands

```powershell
.\Build.bat
.\build\pulse.exe --content-dump
.\build\pulse.exe --dump-weapon-profiles
.\build\pulse.exe --weapon-test
.\build\pulse.exe --asset-selftest --force-raster
.\build\pulse.exe --capture-weapon-matrix --force-raster
```

`--capture-weapon-matrix` writes fresh native-renderer captures to
`build/weapon_matrix/` for every live profile.

# PULSE Music System

## Sonic Thesis

PULSE music is not a playlist. It is a beat-locked pressure machine: dark
acid-techno drive fused with industrial relicpunk texture. The score should feel
like transformer hum, furnace rumble, cold vault space, tight mechanical drums,
and hostile semi-alien motifs pushing under the run without stealing foreground
from gunshots, hit confirmation, damage, or kill feedback.

## Runtime Contract

The runtime score is built from synchronized loop stems plus event stingers. Do
not ship one long combat track as the score.

All loop stems are stereo 48 kHz / 16-bit WAV, 140 BPM, 16 bars, identical frame
count, and identical downbeat. Required runtime files live in
`assets/audio/music/`:

- `foundry_bed.wav`, `foundry_bass.wav`, `foundry_drums.wav`,
  `foundry_pressure.wav`, `foundry_boss.wav`, `foundry_overpulse.wav`
- `furnace_bed.wav`, `furnace_bass.wav`, `furnace_drums.wav`,
  `furnace_pressure.wav`, `furnace_boss.wav`, `furnace_overpulse.wav`
- `reliquary_bed.wav`, `reliquary_bass.wav`, `reliquary_drums.wav`,
  `reliquary_pressure.wav`, `reliquary_boss.wav`, `reliquary_overpulse.wav`
- `hub_bed.wav`
- `stinger_room_clear.wav`, `stinger_reward.wav`, `stinger_boss_intro.wav`,
  `stinger_overpulse.wav`, `stinger_run_win.wav`, `stinger_run_lose.wav`,
  `stinger_sector_foundry.wav`, `stinger_sector_furnace.wav`,
  `stinger_sector_reliquary.wav`

Packaged builds must fail loud if any required music file is missing. The old
root-level `music_*.wav` files are legacy artifacts only; the engine loads the
v3 bank from `assets/audio/music/`.

## Public API

Runtime callers should use:

```cpp
audio.setMusicContext(enabled, bpm, baseVolume, intensity, state, biome, overpulseActive);
audio.playMusicStinger(type, volume, quantizeToBar);
```

`MusicBiome` values are `Foundry`, `Furnace`, and `Reliquary`.
`MusicStingerType` values are `RoomClear`, `Reward`, `BossIntro`, `Overpulse`,
`RunWin`, `RunLose`, `SectorFoundry`, `SectorFurnace`, and `SectorReliquary`.
`setMusicState(...)` and the older `setMusicContext(...)` overload remain as
legacy wrappers for older call sites and default to no sustained overpulse layer.

## Audio State Machine

States:

- `Silent`: no music layers.
- `Hub`: `hub_bed.wav` only, restrained and low.
- `Combat`: biome bed always active; bass, drums, and pressure open with run
  intensity. The overpulse layer opens only while OVERPULSE is actively sustained.
- `Reward`: starts with a 600 ms breath dip, then restores biome bed plus a
  small bass residue.
- `Boss`: biome bed, bass, drums, pressure, and boss layer active. Boss layer
  must never open outside boss state. The overpulse layer can also open here
  while OVERPULSE is active.
- `RunOver`: clears pending quantized stingers and fades down so the run-end
  stinger and feedback have space.

Transitions:

- Layer gains slew in the mixer over roughly 220 ms.
- Biome bank changes crossfade over roughly 500 ms while preserving loop phase.
- Biome changes trigger a sector stinger, bar-quantized while music is active.
- Room-clear, reward, boss-intro, overpulse, and sector stingers are
  bar-quantized.
- Run-win and run-lose stingers play immediately.
- Run-end and boss-intro stingers override lower-priority stingers; room-clear
  and reward stingers have a 2-second duplicate-trigger cooldown.
- Stingers route through the music bus, so gameplay SFX can still duck them.

## RTPC Mapping

`combatIntensity_` is the run pressure control:

- `0.00-0.12`: bed only.
- `0.12-0.44`: bass fades in.
- `0.34-0.72`: drums become full.
- `0.58-0.94`: pressure layer opens.
- During Combat or Boss, OVERPULSE opens the sustained overpulse layer in
  addition to the entry stinger.
- Boss state overrides intensity and enables the boss layer.

Pulse OVERPULSE triggers the `Overpulse` music stinger only on entry, with a
10-second cooldown. While OVERPULSE remains active during Combat or Boss, the
sixth overpulse stem stays up. The run phase still owns the section; intensity
alone cannot select boss music.

## Production Path

Current no-composer source:

- Offline producer: `tools/audio/pulse_music_producer.py`
- Contract validator: `tools/audio/pulse_music_validate.py`
- Matrix report: `tools/audio/pulse_music_report.py`
- REAPER-editable project: `assets/audio/reaper_source/PULSE_adaptive_music.rpp`
- Headless export command:

```powershell
powershell -ExecutionPolicy Bypass -File tools\reaper\render_pulse_music_headless.ps1
```

The producer writes runtime WAVs to `assets/audio/music/`, matching source clips
to `assets/audio/reaper_source/`, and a REAPER session without launching REAPER.
Foundry should be cold/electric, Furnace heavy and heat-scarred, Reliquary sparse
and spatial. Overpulse layers are biome-specific: Foundry electrical arcing,
Furnace heat-pressure distortion, and Reliquary cold spectral shimmer. Boss
layers share one antagonist motif transformed per biome.

## Verification

Use these checks after a music change:

```bat
Build.bat
python tools\audio\pulse_music_validate.py assets\audio\music
python tools\audio\pulse_audio_validate.py assets\audio\music\*.wav
python tools\audio\pulse_music_report.py build\music_v3_report
build\pulse.exe --render-music build\music_foundry_combat_high.wav 16 --music-state combat --music-biome foundry --music-intensity 0.90
build\pulse.exe --render-music build\music_foundry_overpulse.wav 16 --music-state combat --music-biome foundry --music-intensity 0.98 --music-overpulse
build\pulse.exe --render-music build\music_furnace_boss.wav 16 --music-state boss --music-biome furnace
build\pulse.exe --render-music-stinger sector_foundry build\music_stinger_sector_foundry.wav 4
build\pulse.exe --bot-test 60 --record-audio build\pt_run_audio.wav
```

Acceptance:

- No clipping or true-peak violations.
- No clicks on loops or biome transitions.
- Boss layer is absent outside boss state.
- Pulse layers visibly and audibly rise with intensity, and OVERPULSE is audible
  as a sustained layer while active.
- Reward state creates a clear breath mark before the bed returns.
- Sector changes are clear without feeling like a hard music cut.
- Gunshots, hit confirmation, kill confirmation, and damage cues remain in the
  foreground.

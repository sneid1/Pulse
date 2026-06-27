# PULSE SFX conform workflow

How to replace the weak, pure-synth banks (player feedback + enemy creatures) with
real source material, conformed so it sits correctly next to the existing banks.

## Why this exists

The gun banks sound good because raw recordings are layered + EQ'd + peak-targeted
before shipping. Dropped-in files sound "wrong" only when they skip that conform:
they sit at the wrong level and tone. `tools/audio/pulse_conform_producer.py` runs
the same conform on any source so it lands at the right level, tone, and format.

The runtime mixer (reverb glue, music ducking, 3D placement) already lifts every
bank; this step fixes the underlying source quality of the synth banks.

## One-time setup

1. Get royalty-free source material (commercial-use OK):
   - Sonniss "GameAudioGDC" bundles (free, huge, already used for guns).
   - CC0 packs: Kenney audio, freesound.org CC0 filter, OpenGameArt.
   - Or AI-generated one-shots (e.g. ElevenLabs Sound Effects) exported to WAV.
2. Drop raw sources anywhere under `assets/external/` (keep a CREDITS note with
   the license, matching `assets/audio/CREDITS.txt`).
3. WAV works out of the box. For mp3/ogg/flac: `pip install soundfile`, or convert
   to WAV first.

## Conform a bank

```
python tools/audio/pulse_conform_producer.py --stem <bankStem> --inputs <files...> [opts]
```

Each input becomes one round-robin variant (`<stem>.wav`, `<stem>_1.wav`, ...).
Give 3-8 distinct takes per bank for natural variation, or one take plus
`--variants N` to synthesize micro-varied copies. Output goes to `assets/audio/`
and is auto-checked by `pulse_audio_validate.py` (zero-clip gate).

Useful options: `--max-len S` (cap length), `--peak-db` (target ceiling, default
-1.0), `--hp/--lp` (band trim), `--tilt-db` (brightness), `--trim-db` (silence trim).

## Priority order (weakest first)

These are pure synthesis today and benefit most:

### Player feedback bus -- sfx_fb_<event>
Short, tactile, non-musical, in-your-head (NOT spatialized). Keep them tight.
Events: hitmarker, hit_crit, kill, kill_elite, dash, jump, ability_tactical,
ability_ultimate, charge_ready, explosion, shield_absorb, shield_break,
low_health, pickup_health, pickup_shield, pickup_ammo, pickup_scrap,
pickup_powerup, ui_move, ui_confirm, ui_cancel, ui_reward, run_win, run_lose.

Examples:
```
python tools/audio/pulse_conform_producer.py --stem sfx_fb_jump \
    --inputs jump_a.wav jump_b.wav --max-len 0.30
python tools/audio/pulse_conform_producer.py --stem sfx_fb_ui_move \
    --inputs tick.wav --variants 4 --max-len 0.15 --hp 200
python tools/audio/pulse_conform_producer.py --stem sfx_fb_hitmarker \
    --inputs hit_a.wav hit_b.wav hit_c.wav --max-len 0.18 --tilt-db 2.0
```

### Enemy creatures -- sfx_enemy_<kind>_<event>
World cues; the engine spatializes these (pan/distance), so source them dry/close.
Kinds: rusher, ranged, tank, stalker, boss.
Events: telegraph, shot, impact, beam, lunge, melee_hit, hurt, death, boss_burst.

Examples:
```
python tools/audio/pulse_conform_producer.py --stem sfx_enemy_rusher_death \
    --inputs growl_die_a.wav growl_die_b.wav growl_die_c.wav --max-len 0.9
python tools/audio/pulse_conform_producer.py --stem sfx_enemy_tank_telegraph \
    --inputs heavy_windup.wav --variants 3 --max-len 0.7 --tilt-db -1.5
```

## Verify

- Levels/clipping: `python tools/audio/pulse_audio_validate.py assets/audio/sfx_fb_*.wav`
  (also runs automatically on every `Build.bat`).
- In-game: run `build\pulse.exe` and listen (the runtime feel pass + spatialization
  apply on top). Per the project workflow, tune by ear live.
- Spatial placement (enemy banks) can be spot-checked headless:
  `build\pulse.exe --render-enemy-event death out.wav 2 --enemy rusher --emitter -8 0`

Keep the filename, mono, and 48 kHz contract and the engine picks new banks up with
no code change (it prefers real WAVs over the synth fallback).

# PULSE Player Audio Spec

The player-feedback bus is the short tactile top layer the player reads to
confirm their OWN actions and state: hits, crits, kills, movement, abilities,
shield events, pickups, menu navigation, and run win/lose. It is deliberately
distinct from the weapon/enemy/world banks so a confirmation cue can never be
confused with a threat sound, but it avoids arcade coin/chime/fanfare language.
The current pass uses filtered transients, body, air, servo, metal, and shield
fracture layers so the cues stay readable without sounding like a retro UI.

## Runtime Contract

Player-feedback banks are 48 kHz, 16-bit, mono WAVs, one round-robin bank per
event:

```text
sfx_fb_<event>.wav
sfx_fb_<event>_1.wav
sfx_fb_<event>_2.wav
...
```

The engine auto-loads every event bank at startup (`AudioSystem::loadBank`) and
plays it through `AudioSystem::playFeedback(FeedbackEventType, volume, seqIndex)`.
The game routes all calls through `PulseGame::playFeedback`, which walks a single
free-running `feedbackSoundIndex_` so repeats cycle the variants deterministically
(headless capture stays reproducible).

Banks are SOFT-required: if a bank is missing, `playFeedback` degrades to the
closest legacy engine one-shot (`SoundEventType`) rather than going silent. The
build regenerates the banks, so shipping builds always use the authored versions.

## Events

- `hitmarker`: standard connect confirm. Must cut through music. Very short, dry tick + body.
- `hit_crit`: headshot/crit connect. Sharper double contact, not a melodic up-chime.
- `kill`: standard kill confirm (muted mechanical thock).
- `kill_elite`: boss/elite kill confirm (bigger low body + stressed metal).
- `dash`: dash whoosh (also used for a clean i-frame dodge, quieter).
- `jump`: grounded takeoff/suit motion cue, deliberately not a boing or pitch sweep.
- `ability_tactical`: grenade/tactical cast (servo prime into physical launch).
- `ability_ultimate`: overdrive ultimate cast (noisy charge into saturated impact).
- `charge_ready`: edge-triggered when a tactical/ultimate charge first fills.
- `explosion`: player AoE / projectile splash detonation (grenade, railbolt).
- `shield_absorb`: a partial shield absorb (short shield grit + clink).
- `shield_break`: shield drained to zero (fracture grit + debris ticks).
- `low_health`: edge-triggered once as HP dips into the danger band (never loops).
- `pickup_health` / `pickup_shield` / `pickup_ammo`: per-kind pickup confirms.
- `pickup_scrap`: economy ding (reserved; available for scrap-pickup cues).
- `pickup_powerup`: weapon/passive/upgrade acquired (reward, shop, hub unlock; restrained).
- `ui_move`: menu move / reroll / heat adjust (subtle).
- `ui_confirm`: positive menu action (two soft mechanical contacts).
- `ui_cancel`: rejected / not-enough action (muted mechanical downbeat).
- `ui_reward`: room cleared / reward presented (short reward thock + restrained bloom).
- `run_win`: run-clear stinger, celebratory but not a fanfare.
- `run_lose`: somber death stinger, low and muffled.

## Production

The current no-sound-designer bank is produced by:

```powershell
python tools\audio\pulse_player_sfx_producer.py
```

It is pure procedural synthesis (no external recordings), so the whole bank is
reproducible from the script. The current recipe intentionally avoids pure sine
blips, arps, and coin-like pickup language. Build.bat regenerates any missing
`sfx_fb_*` bank.
The producer writes `assets/audio/PULSE_player_sfx_producer_log.txt` with peak/RMS
per variant.

## Mix Rules

- Combat confirms (`hitmarker`, `hit_crit`, `kill`, `kill_elite`) are the hottest
  player-feedback banks, but are now slightly less peak-forward than the old
  arcade pass so they read clearly without dominating the weapons.
- Ambient/UI cues (`ui_move`, `low_health`, pickups) sit lower so they inform
  without competing with combat readability.
- Player feedback never reuses an enemy/world bank, and shield events are kept
  distinct from flesh damage (HP damage still uses the engine `hurt` cue).
- Every bank is peak-targeted with no clipping; verify with
  `python tools\audio\pulse_audio_validate.py assets\audio\sfx_fb_*.wav`.

## Headless Testing

Render any event through the engine (not just the source WAV):

```powershell
build\pulse.exe --render-feedback-event kill build\fb_kill.wav 3
```

Validate levels and emit spectrograms:

```powershell
python tools\audio\pulse_audio_validate.py build\fb_kill.wav --spectrogram build\spectro
```

Spectrograms and level stats are a mix gate, not a substitute for ears-on
playtesting.

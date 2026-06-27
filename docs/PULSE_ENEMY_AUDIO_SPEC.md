# PULSE Enemy Audio Spec

Enemy audio is the threat/body layer under the player feedback bus. Player
`Hit` and `Kill` sounds stay crisp and UI-like; enemy banks add attack tells,
launches, impacts, hurt bodies, and deaths.

## Runtime Contract

Enemy banks are 48 kHz, 16-bit, mono WAVs:

```text
sfx_enemy_<kind>_<event>.wav
sfx_enemy_<kind>_<event>_1.wav
sfx_enemy_<kind>_<event>_2.wav
sfx_enemy_<kind>_<event>_3.wav
```

Kinds:

- `rusher`
- `ranged`
- `tank`
- `stalker`
- `boss`

Events:

- `telegraph`: attack wind-up warning; must be readable but not mask gunshots.
- `shot`: hostile orb/projectile release.
- `impact`: hostile orb wall/player impact.
- `beam`: instant beam release.
- `lunge`: rusher/stalker launch or heavy movement commitment.
- `melee_hit`: melee contact/slam layer, played with player hurt feedback.
- `hurt`: enemy body reaction under the hitmarker.
- `death`: enemy collapse/disintegrate layer under kill confirmation.
- `boss_burst`: boss radial-pattern release.

## Production

The current no-sound-designer bank is produced by:

```powershell
python tools\audio\pulse_enemy_sfx_producer.py
```

The build checks every kind/event bank and regenerates missing files. The
producer writes `assets/audio/PULSE_enemy_sfx_producer_log.txt` with peak/RMS
and coarse band stats for every generated variant.

## Mix Rules

- Telegraphs are intentionally below attack releases and death/body hits.
- Enemy hurt/death layers do not replace player hit/kill confirmation.
- Boss events use the `boss` bank rather than repurposing `Kill`.
- Hostile projectile impacts use the source enemy bank, so tank orbs land
  heavier than stalker darts.

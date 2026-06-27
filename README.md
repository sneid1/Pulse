# PULSE

**A fast, momentum-driven first-person roguelite shooter on a custom Direct3D 12 engine.**

![Platform](https://img.shields.io/badge/platform-Windows-0a84ff?style=flat-square)
![Engine](https://img.shields.io/badge/engine-Direct3D%2012%20%2B%20DXR-7a5cff?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B-00a3a3?style=flat-square)
![Genre](https://img.shields.io/badge/genre-FPS%20Roguelite-ff4d6d?style=flat-square)
![Status](https://img.shields.io/badge/status-in%20development-f5a623?style=flat-square)

This is the source project for PULSE. The public game-page repository is
[`sneid1/pulsegame`](https://github.com/sneid1/pulsegame); it carries the player-facing
overview, screenshots, and release links. This repo carries the native engine,
game code, tools, configuration, and the runtime asset set required to build and
package the game.

## The Game

PULSE is built around short, readable arena fights, strong weapon feel, and
aggressive movement. Runs move through neon-brutalist rooms across the Foundry,
Furnace, and Reliquary biomes. You clear waves, choose doors, buy from shops,
upgrade weapons at the forge, take event risks, stack passives, and push toward
elite fights and bosses.

The core loop is simple: move fast, shoot clean, build pressure, die, rebuild,
and run it back.

## Gameplay Systems

- **Pulse meter:** aggression raises Pulse, retreat cools it, and higher bands
  increase combat intensity, element output, and adaptive music pressure.
- **Room-based roguelite structure:** authored combat rooms, shops, events,
  reward choices, pacing rules, elites, and sector bosses.
- **Six live weapon profiles:** pistol, AK-47, tactical carbine, Pulse SMG,
  scattergun, and marksman rifle, each with distinct recoil, reload behavior,
  and audio identity.
- **Element stack effects:** burn, shock, cryo, and corrode create status damage,
  control, vulnerability, and pair reactions.
- **Affinity builds:** passives and weapon aspects stack toward set bonuses that
  shape each run.
- **Readable enemies:** rushers, ranged attackers, stalkers, tanks, elites, and
  bosses use wind-ups and dedicated audio tells.

## Controls

| Action | Input |
| --- | --- |
| Move | WASD |
| Look | Mouse |
| Fire | Left mouse |
| Dash | Shift |
| Jump / double jump | Space |
| Reload | R |
| Swap weapon | Q |
| Quick-swap pistol | V |
| Cycle weapon aspect | X |
| Grenade | G |
| Ultimate | F |
| Menu / pause / back | Esc |

## Build From Source

Requirements:

- Windows with Visual Studio C++ build tools.
- CMake and Ninja.
- Git LFS for runtime binary assets.
- Python for audio/asset validation tools.
- Node.js plus `@gltf-transform/cli` for `Package.bat` asset baking.

Fresh checkout:

```bat
git lfs install
git lfs pull
tools\Provision-ThirdParty.bat
```

Build and run the development executable:

```bat
Build.bat RelWithDebInfo pulse
build\pulse.exe
```

Create a self-contained runtime folder:

```bat
Package.bat Release
dist\PULSE.exe
```

`Author-Assets.bat` is for regenerating authored content. Normal builds use the
runtime assets tracked in this repo.

## Assets

The tracked `assets/` tree is the build-required runtime set. It is intentionally
kept in line with what `Package.bat` copies to `dist/assets`; raw downloads,
authoring workspaces, caches, captures, and obsolete source exports are ignored.

Large binary assets are stored with Git LFS. If a clone is missing models, audio,
or textures, run:

```bat
git lfs pull
```

See [`assets/README.md`](assets/README.md) and [`assets/CREDITS.txt`](assets/CREDITS.txt)
for asset layout and licensing notes.

## Tech

PULSE runs on a clean-sheet native C++ engine:

- Direct3D 12 renderer with DXR-capable ray-tracing support.
- Bindless Shader Model 6.6 resource access.
- Render graph with automatic barriers, pass culling, and transient resource
  pooling.
- Reverse-Z depth.
- Data-driven weapons, rooms, content, style, and tuning through `config/`.
- WASAPI audio with authored weapon, enemy, UI, ambience, and adaptive music
  banks.
- Headless capture, validation, and asset-review tooling for iteration.

## Repository Map

- `src/` - engine, platform, rendering, audio, UI, and game code.
- `assets/` - runtime assets required by the build/package path.
- `config/` - data-driven game content, rooms, style, and weapon definitions.
- `tools/` - provisioning, asset, audio, Blender, Meshy, and validation tools.
- `docs/` - design plans, prototype specs, audio specs, and playtest prompts.
- `Package.bat` - source of truth for what ships in `dist/`.

## Status

The project is in active development. This source repo is intended to build and
package the current Windows runtime, while [`sneid1/pulsegame`](https://github.com/sneid1/pulsegame)
serves as the lightweight public game page.

## Credits

PULSE combines original engine/game work with original and openly licensed game
content. Per-asset details live in [`assets/CREDITS.txt`](assets/CREDITS.txt).

Notable asset sources include Quaternius sci-fi kits, DJMaesen / bumstrum
first-person weapon rigs, Poly Haven materials, Vincent Sevedge gunshot source
recordings, and Sonniss GameAudioGDC source material. Most adaptive music and
many gameplay sound effects are produced specifically for PULSE.

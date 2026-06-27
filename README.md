# PULSE

**A fast, momentum-driven first-person roguelite shooter on a custom Direct3D 12 engine.**

![Platform](https://img.shields.io/badge/platform-Windows-0a84ff?style=flat-square)
![Engine](https://img.shields.io/badge/engine-Direct3D%2012%20%2B%20DXR-7a5cff?style=flat-square)
![Language](https://img.shields.io/badge/language-C%2B%2B-00a3a3?style=flat-square)
![Genre](https://img.shields.io/badge/genre-FPS%20Roguelite-ff4d6d?style=flat-square)
![Status](https://img.shields.io/badge/status-in%20development-f5a623?style=flat-square)

![PULSE title screen: the PULSE wordmark over the current oscilloscope-style main menu.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-title-current.jpg)

PULSE is a fast first-person roguelite shooter about momentum, pressure, and clean
weapon feel. You sprint through neon-brutalist arenas, chain kills to keep the run
alive, and build a loadout from weapons, passives, shops, events, and escalating
combat rewards.

The target is simple: move fast, shoot clean, die, rebuild, run it back.

Download: https://drive.google.com/drive/folders/1YxxNOfGPxYPvW6nGrgLS-6Hy50EZHr0o?usp=drive_link

## Contents

- [Screenshots](#screenshots)
- [The Game](#the-game)
- [The Pulse system](#the-pulse-system)
- [Elements and effects](#elements-and-effects)
- [Builds and loadout](#builds-and-loadout)
- [Weapons](#weapons)
- [Enemies](#enemies)
- [Controls](#controls)
- [Accessibility and comfort](#accessibility-and-comfort)
- [Audio](#audio)
- [Tech](#tech)
- [Status](#status)
- [Credits](#credits)
- [Links](#links)
- [Source repository](#source-repository)

## Screenshots

### Biomes

| Foundry | Furnace | Reliquary |
| --- | --- | --- |
| ![Foundry biome: cold cyan industrial room with the tactical carbine and a hostile wave.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-foundry.jpg) | ![Furnace biome: amber heat-haze arena with the scattergun facing enemies.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-furnace.jpg) | ![Reliquary biome: blue monumental room with the marksman rifle and enemies.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-reliquary.jpg) |
| Cold, electric, industrial. | Heavy, heat-scarred, close. | Sparse, spatial, monumental. |

### Options and shop

| Accessibility options | Shop and forge |
| --- | --- |
| ![PULSE options menu showing accessibility settings: text and HUD scale, colorblind preset, high-contrast HUD, reduce flashes/motion/bloom, screen shake.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-menu-options.jpg) | ![PULSE shop: passive rewards by rarity with element set progress, a forge bay for the active weapon, repair, and reroll.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-shop.jpg) |

## The Game

PULSE is built around short, readable arena fights instead of slow attrition.
Enemies hit hard, but they are telegraphed. Weapons have strong recoil identity,
fast feedback, and low time-to-kill. Movement is part of the offense: dash to
clear attacks, double-jump into angles, and stay close enough for the run's
pressure systems to reward aggression.

A run is a sequence of rooms across themed biomes. You clear waves, choose a
route through doors, spend scrap at shops, take risks at events, and grab combat
rewards that stack into a build, then push deeper toward escalating elites and a
boss. Die and you start a fresh run with everything you learned.

The current build features:

- A room-based roguelite run structure with doors, shops, events, reward
  choices, and seeded pacing.
- 30 authored room templates across the Foundry, Furnace, and Reliquary biomes,
  each with its own palette, lighting, ambience, and music.
- Six live weapon profiles, each with a distinct recoil, reload, and audio
  identity.
- Enemy archetypes built around readable pressure: rushers, ranged attackers,
  stalkers, tanks, elites, and boss encounters.
- A momentum-forward Pulse system that drives intensity, rewards aggression, and
  ties combat pacing directly to the music.
- Adaptive 140 BPM techno combat music with layered SFX and weapon-specific
  action banks.
- A deep accessibility and comfort suite (see below).

## The Pulse system

Pulse is the game's namesake mechanic and its tempo. Killing and staying
aggressive charges your Pulse; backing off lets it cool. As it climbs it amplifies
your run, sharpens your element and ability output, and pushes the music into
higher-pressure layers. The HUD meter at the bottom of the screen shows your
current band so you can read the moment at a glance and decide when to spend it.

It is a feedback loop, not a separate resource to babysit: play fast and clean and
the whole game leans in with you.

## Elements and effects

Damage can carry status elements. Enemies accumulate stacks, then the element
does its own job: burn ticks, shock discharges, cryo freezes, and corrode makes
targets easier to break. Items and weapon aspects decide which elements your
build applies, while the Pulse meter amplifies status output as it climbs.

| Burn | Shock |
| --- | --- |
| ![PULSE gameplay: burning enemies with orange status effects active.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-element-burn-combat-v2.jpg) | ![PULSE gameplay: shocking enemies with blue-white status effects active.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-element-shock-combat-v2.jpg) |

| Cryo | Corrode |
| --- | --- |
| ![PULSE gameplay: freezing enemies with cryo status effects active.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-element-cryo-combat-v2.jpg) | ![PULSE gameplay: corroding enemies with green acid status effects active.](https://raw.githubusercontent.com/sneid1/pulsegame/main/media/pulse-element-corrode-combat-v2.jpg) |

| Element | Effect |
| --- | --- |
| **Burn** | Damage over time. More stacks mean deeper attrition. |
| **Shock** | Builds charge; at the threshold it discharges for burst damage and chain arcs. |
| **Cryo** | Chills and slows; full chill freezes, and frozen enemies shatter for bonus damage. |
| **Corrode** | Melts armor so enemies take more damage and receive stronger status application. |

Two elements on one target can also trigger a pair reaction:

| Reaction | Trigger | Effect |
| --- | --- | --- |
| Plasma Surge | Burn + Shock | Splash burst plus extra shock charge. |
| Thermal Shock | Burn + Cryo | Steam burst that strips burn and chill into damage. |
| Superconduct | Shock + Cryo | Cold arc burst that primes nearby enemies. |
| Galvanic Melt | Shock + Corrode | Corrosion forces an instant shock discharge. |
| Caustic Fire | Burn + Corrode | Flame sticks harder and acid spreads. |

Affinities are the build identity layer attached to rewards. Collecting three of
one affinity turns on its amplifier; collecting five unlocks its signature.

| Affinity | Applies | 3-set | 5-set |
| --- | --- | --- | --- |
| **Pyro** | Burn | +50% burn applied | Burning enemies detonate on kill. |
| **Volt** | Shock | +50% shock applied | Chain arcs carry your elements. |
| **Cryo** | Chill / freeze | +50% chill applied | Shatters emit a freeze nova. |
| **Acid** | Corrode | +50% corrode applied | Corrode spreads on kill. |
| **Kinetic** | Mobility and aggression | +10% movement, faster dash | +15% damage. |
| **Bulwark** | Defense | +8% damage reduction | +40 max health. |

## Builds and loadout

Rewards stack into a build over a run:

- **Passives** carry elemental affinities (kinetic, cryo, pyro, volt, acid). Stack
  enough of one affinity and its set bonus kicks in, so the passives you skip
  matter as much as the ones you take.
- **Weapon aspects** are alternate forms that rework a weapon as you pour power
  into it at the forge, each with its own intrinsic element (for example the
  AK-47's Inferno and Tempest forms, or the scattergun's Avalanche and Meltdown).
- **The shop** sells passives by rarity, repairs your health, rerolls the stock,
  and runs a forge bay that upgrades and unlocks aspects on your active weapon.
- **Events** offer risk-for-reward deals between fights.

## Weapons

| Weapon | Role |
| --- | --- |
| Pistol | Semi-auto sidearm with reliable precision and infinite reserve. |
| AK-47 | Full-auto rifle with heavy kick and strong sustained damage. |
| Tactical Carbine | Controlled 3-round burst rifle for disciplined mid-range fights. |
| Pulse SMG | High-rate mobility weapon for close pressure and spray control. |
| Scattergun | Close-range pellet weapon with per-shell reload behavior. |
| Marksman | Precision rifle for deliberate, high-impact shots. |

Two experimental catalog weapons, the machine pistol and the railbolt, are
currently locked until their unique first-person rigs and audio banks are
complete.

## Enemies

Threats are designed to be read, not memorized. Each archetype telegraphs its
attack with a wind-up and a distinct sound:

- **Rushers** close the distance and pressure you into moving.
- **Ranged** attackers throw telegraphed projectiles to control space.
- **Stalkers** flank and punish a static player.
- **Tanks** soak damage and hold ground.
- **Elites** are tougher, modified variants that raise the stakes mid-run.
- **Bosses** cap a sector with radial-burst and beam patterns.

## Controls

### Combat

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

### Menus and shop

| Action | Input |
| --- | --- |
| Navigate | Arrow keys or WASD |
| Confirm | Enter or Space |
| Shop buys | 1-4 |
| Shop repair | H |
| Shop reroll | R |
| Shop forge | F |

## Accessibility and comfort

PULSE ships with a dedicated accessibility tab so the game can be tuned to how you
play and see:

- HUD scale and text scale.
- Colorblind presets and a high-contrast HUD.
- Element and rarity glyphs so important state never relies on color alone.
- Reduce flashes, reduce motion, and reduce bloom toggles.
- Adjustable screen shake.
- Mono downmix and combat readability tuning.

## Audio

The soundtrack is adaptive: 140 BPM techno stems are layered live so the music
follows the fight, with biome-specific beds, sustained high-pressure layers, and
boss transformations of a shared antagonist motif. Weapons, enemies, and player
feedback each sit on their own bank so a confirmation never reads as a threat, and
every build runs through a zero-clip mix gate.

## Tech

PULSE runs on a clean-sheet native C++ engine built from scratch:

- A **Direct3D 12** renderer with a DXR-capable ray-tracing tier.
- **Bindless** resources throughout (Shader Model 6.6 `ResourceDescriptorHeap`),
  with no input-assembler vertex layouts.
- A **render graph** with automatic barriers, pass culling, and transient
  resource pooling.
- **Reverse-Z** depth for precision at range.
- Data-driven gameplay: weapons, content, rooms, and tuning load from config and
  hot-reload in place during development.
- A headless capture and vision-critique loop used to author and validate the
  look without shipping a single frame blind.

Runtime content is packaged separately from the public game page so
[`sneid1/pulsegame`](https://github.com/sneid1/pulsegame) can stay lightweight.
This source repo tracks the build-required runtime assets with Git LFS.

## Status

The Windows runtime build is being prepared for distribution; download and
release instructions will be added here once the distribution path is finalized.
This source repository is in active development and is intended to build and
package the current Windows runtime.

## Credits

PULSE is built on its own engine with a mix of original and openly licensed
content. Full per-asset license files live alongside each asset in the source
project.

- **3D art:** Quaternius "Modular SciFi MegaKit" (CC0); first-person weapon
  viewmodels by DJMaesen / bumstrum (CC-BY-4.0); surface materials from Poly Haven
  (CC0).
- **Audio:** gunshot source recordings by Vincent Sevedge (CC-BY-3.0) and by
  FLYSOUND and other suppliers via the Sonniss GameAudioGDC royalty-free bundle;
  the adaptive music and most sound effects are original procedural synthesis.

## Links

- Public game page: https://github.com/sneid1/pulsegame
- Source repository: https://github.com/sneid1/Pulse

## Source repository

This repository carries the native engine, game code, tools, configuration, and
the runtime asset set required to build and package PULSE.

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

Repository map:

- `src/` - engine, platform, rendering, audio, UI, and game code.
- `assets/` - runtime assets required by the build/package path.
- `config/` - data-driven game content, rooms, style, and weapon definitions.
- `tools/` - provisioning, asset, audio, Blender, Meshy, and validation tools.
- `docs/` - design plans, prototype specs, audio specs, and playtest prompts.
- `Package.bat` - source of truth for what ships in `dist/`.

The tracked `assets/` tree is intentionally kept in line with what `Package.bat`
copies to `dist/assets`; raw downloads, authoring workspaces, caches, captures,
and obsolete source exports are ignored. See [`assets/README.md`](assets/README.md)
and [`assets/CREDITS.txt`](assets/CREDITS.txt) for asset layout and licensing
notes.

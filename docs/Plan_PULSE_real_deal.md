# PULSE - "The Real Deal" Roguelite Overhaul (AA indie quality)

The combat feel is proven and the roguelite SKELETON exists (clean `RunPhase` machine,
deterministic seeded runs, headless `--balance-sim`, Hades-style spatial-door loop). But
the content + mechanics are demo-grade. This plan rebuilds the roguelite STRUCTURE and
MECHANICS to a shippable AA-indie bar, on top of the proven feel. It supersedes the
scope of `Plan_PULSE_roguelite.md` (Phases A-D) and `Plan_PULSE_roguelike_structure.md`
(features 1-4), which are DONE and are the foundation this builds on.

Direction locked with the developer (2026-06-23): the most ambitious option on every
axis - layer all three build systems, hybrid meta, branching biome map, and make the
Pulse a real gameplay mechanic.

## Hard rules carried over (do not break)

- ASCII only in every file (see CLAUDE.md).
- Difficulty escalates by COUNT / CADENCE / COMPOSITION / aggression / enemy-damage, and
  now by the Pulse - NEVER by per-enemy HP sponge. Every enemy keeps dying in a small,
  decisive number of hits at any power level.
- Determinism: every new roll uses `run_.rng()`, never the combat-jitter `rng_`, so
  `--balance-sim` stays byte-reproducible.
- Fail loud on missing required assets; new UI degrades gracefully (auto-pick fallback).
- Everything stays headless-verifiable (`--bot-test`, `--balance-sim`).
- Meta permanent power stays SMALL and capped (hybrid, not a grind-gate).

## Vision (one line)

Momentum is the mechanic. You carve through neon-brutalist biomes; the harder you push,
the more powerful you get, the more the world escalates, and the higher the music
ascends - feeding elemental + proc + weapon-aspect builds that snowball into distinct,
broken-strong runs across a routed map of real, multi-phase bosses.

## Pillar 1 - The Pulse (signature momentum mechanic)

Promote `combatIntensity_` from a cosmetic music float to a real meter with tiers
(Cold -> Warm -> Hot -> Burning -> OVERPULSE), owned by `src/Game/Pulse.{hpp,cpp}`.

- RISES: kills (faster successive kills ramp harder), headshots, no-hit time, dashing
  through enemies, aggressive proximity, on-beat actions (a Tier-3+ reward).
- DRAINS: idling / not fighting, backpedaling; taking a hit knocks you DOWN a tier.
- GRANTS (escalating by tier): damage / fire-rate / move-speed / ability-charge-rate
  ramps; an OVERPULSE state at the top (brief power spike + visual/audio bloom).
- DRIVES RISK/REWARD: higher Pulse biases loot tier + scrap up (the greed spine).
- STILL DRIVES THE MUSIC (keep the existing `audio.setMusic(..., intensity)` hook).

Wiring: combat hooks feed the meter (`onEnemyKilled`, `damagePlayer`, `fireProfileShot`,
dash); a `BuildStats`-style aggregate is read at the fire / move / charge sites to apply
the grants. The HUD shows the meter + the active tier state.

## Pillar 2 - The build engine (layer all three)

(a) STATUS ELEMENTS - new `src/Game/Status.{hpp,cpp}`. Enemies gain a status component
(per-element stacks + timers). Damage can carry an element; statuses tick and trigger:
  - Burn  : DoT, stacks; "detonate" consumes stacks for AoE.
  - Shock : chains to nearby; combos vs frozen.
  - Cryo  : slows -> freezes; frozen take shatter/crit bonus.
  - Corrode: melts armor/shield effectiveness, amps DoT/status.
  Apply hooks in `resolveHitscan`/damage resolution; tick in `updateEnemies`; VFX reuse
  the existing particle temperature + heat-haze systems.

(b) PROC / KEYWORD MATRIX - extend `Build`: items carry TAGS (on-hit, on-kill, status,
mobility, defense, proc) + proc coefficients; some items amplify others by tag with
stacking curves (hyperbolic for chance). Deepens `EffectKind`/`recompute` from flat sums
into a tagged interaction matrix.

(c) WEAPON ASPECTS - each weapon gains 2-3 Aspects (unlockable forms) that rework firing
via the existing archetype kernel (`fireProfileShot`); build becomes weapon-led with
supporting items. Aspect stored on the loadout slot.

Plus: real rarity (Common/Uncommon/Rare/Legendary), synergy surfacing in the reward UI
(Hades-style "this combos with what you own" highlight), and set bonuses (owning N of an
element/tag grants a threshold effect).

## Pillar 3 - Branching biome map + boss roster

- Replace the linear `steps_` (`Run.cpp`) with a routed NODE GRAPH per run: typed nodes
  (Combat/Elite/Cache/Shop/Event/Forge/MiniBoss/Boss/Secret), a visible map, multi-step
  route choice. New `src/Game/Map.{hpp,cpp}` (or a graph inside `Run`).
- BUILD BIOMES INTO THE BRUTALIST DEFAULT (they do NOT exist today). The default run is
  the brutalist arena (`main.cpp:263`), and `Wasteland::generate(Biome,...)` throws the
  biome away on the brutalist path (`Wasteland.cpp:871-872`): everything keys off
  `AreaSize` and one fixed 6-slot palette. Real work, not a recolor toggle:
  - Per-biome palette/material SETS baked in `loadBrutalist` (e.g. Foundry cold-concrete/
    cyan, Furnace oxide/amber, Reliquary bone/violet, Sump black-glass/green); a
    biome-aware `generateBrutalist` selects palette + lighting/fog.
  - Per-biome geometry motif + a hazard archetype (crushers, turret nests, hazard floors,
    low-grav) carved into the existing template/cover gen.
  - Per-biome enemy roster (shared + biome-exclusive) and loot biased toward an element/tag.
  - Wire the currently-inert `currentBiome_` (`PulseGame.cpp:2462`) + lighting (`~6976`)
    through; lean on the asset/Blender pipeline + vision loop for the new looks.
- BOSS ROSTER - new `src/Game/Boss.{hpp,cpp}`: 3-5 real multi-phase bosses (mobile
  duelist, zoner/artillery, swarmlord/summoner, ... + final boss), each with telegraphed
  attack patterns, arena mechanics, and weak points. One per biome. Retires the
  Warden-reskin.
- Variable run length via route choice (short/risky vs long/greedy).

## Pillar 4 - Hybrid meta + economy depth

- Light PERMANENT layer in `Meta`: a small, capped opt-in "Mirror" board (modest
  persistent boosts), plus existing unlocks, cosmetics, a codex (enemies/weapons/bosses/
  lore), and records. Runs stay mostly fair; permanent power is small.
- Deeper HEAT/ASCENSION: named, individually-toggleable modifiers (Hades Pact-style)
  instead of a single 0-10 dial; extend `RunMods`/`heatMods`.
- ECONOMY DEPTH: richer shop services (a Forge/altar to upgrade/recombine/infuse items
  with an element), item-remove/bank, a 20+ deal catalog, a rare currency for legendaries/
  forging. Save format extended (versioned, graceful fallback).

## Cross-cutting - verification & balance

Extend `--balance-sim` telemetry to the axes AA balance needs (currently unmeasured):
per-room walls, TTK by room/biome, build-archetype win-rates, status uptime, Pulse-tier
distribution, per-biome/boss clear rates, weapon/aspect usage. Add bot policies for the
new phases (map routing, forge, aspect picks). Hold the no-HP-sponge line and the
monotonic-heat constraint.

## Architecture strategy

- Tame the 8.7k-line monolith: new systems land in new modules (`Pulse`, `Status`,
  `Boss`, `Map`); combat hooks into them stay small and localized at the mapped
  read-sites. Carve combat-adjacent code out opportunistically where a milestone touches it.
- Data-drive content: extend the pipe-delimited `config/pulse.content` parser
  (`Build.cpp:loadContentFile`) with new record types (`status|`, `aspect|`, `set|`,
  `enemy|`, `boss|`, `deal|`, `heat|`, `biome|`) so the bulk authoring is data + F5 hot
  reload, not recompiles. Only recombinations of fixed vocab are data; a new effect TYPE
  is still a C++ hook.

## Milestones (each independently buildable + headless-verifiable)

Order = combat depth first (improves the feel gate first), then structure, then meta/
economy, then content+balance. Each leaves the game playable and `--bot-test` passing.

- M1  The Pulse: meter math + tier grants + greed loot bias + hit-knockdown + HUD + music
      + Pulse-tier sim telemetry.
- M2  Status-element combat layer: Status module, 4 elements (apply/tick/trigger), base
      VFX/SFX, first wave of status items.
- M3  Build engine depth: tags + proc matrix + rarity (incl. Legendary) + weapon aspects
      + synergy surfacing + set bonuses; expand to a 50+ catalog, data-driven.
- M4  Branching biome map + build biomes into the brutalist default (palette/motif/hazard
      /roster/reward identity); map UI + routing + bot policy.
- M5  Boss roster: 3-5 multi-phase bosses w/ telegraphs + arena mechanics + weak points.
- M6  Hybrid meta + economy depth: Mirror board, codex/records, named-pact heat, Forge
      rooms, richer shop + 20+ deals, currencies; save-format extension.
- M7  Content breadth + balance + polish: fill pools, data-ify, extend sim telemetry,
      tune curves (no-sponge, monotonic heat, build diversity), full feel pass.

## Verification per milestone

- `Build.bat` clean; `pulse --bot-test 120` passes (no crash, sane state, full run plays);
  D3D12 GBV clean in a debug build.
- `pulse --balance-sim 200` (+ per-heat / `--sim-mods` sweeps): win-rate direction, no
  per-room wall, monotonic heat, build diversity (M3+).
- Headless pose captures for each new HUD/screen reviewed via the vision loop; developer
  feel-gate on a fully-built run.
- Old save profiles load cleanly after the format extension.

## Risks

- Scope is large - mitigated by independently shippable milestones; reprioritize at any
  boundary.
- Balance interactions (Pulse x status x proc x aspects x heat) are the dominant risk -
  lean on the extended sim after every milestone; never reach for enemy-HP.
- Monolith churn - keep new logic in new modules, localized hooks only.

## Status (all milestones IMPLEMENTED, 2026-06-23)

- M1 DONE - `src/Game/Pulse.{hpp,cpp}`; combatIntensity_ is now derived from the meter; grants
  at the damage/fire/move/charge read-sites; greed loot bias from room-peak Pulse; HUD tier
  meter + OVERPULSE; sim telemetry `avg pulse` + per-tier time.
- M2 DONE - `src/Game/Status.{hpp,cpp}`; Burn/Shock/Cryo/Corrode apply/tick/trigger; damageEnemy
  corrode-amp + frozen-shatter; updateStatuses DoT/chain; 6 status items; boss freeze-resist;
  sim `status uptime`.
- M3 DONE - `Build` gains ItemTier::Legendary, Affinity + 3-set amps + 5-set signature transforms
  (Pyro detonate / Volt conduct / Cryo nova / Acid spread / Kinetic / Bulwark), WeaponAspect forms
  (X cycles, unlock-by-power, intrinsic element), reward-UI synergy surfacing; ~49 items.
- M4 DONE - brutalist arena is biome-aware (`Wasteland` per-biome `brutalMat_`), biome lighting +
  sky in buildFrame, biome names (Foundry/Furnace/Reliquary) + element identity, full-run route
  rail (`drawRouteRail`), `--biome N` capture flag. BIOME VISUAL-POP RESOLVED (M7 follow-up): a
  per-biome frame-wide grade tint (`PostParams.gradeTint` -> `TonemapPush` -> tonemap.hlsl, root
  constants widened 16->20) gives Foundry cold cyan-teal / Furnace hot amber / Reliquary violet -
  the shared sci-fi tileset no longer washes out the biome read.
- M5 DONE - boss roster in `updateBoss` (Warden radial / Smelter lance+nova / Choir spiral+blink),
  one per biome, telegraphs, themed movement, capped summons (`summonBossAdds`), weak-point punish
  window (1.6x in damageEnemy + EXPOSED HUD tell).
- M6 DONE - `Meta` MIRROR board (6 capped permanent nodes, save-persisted via the `mirror=` key,
  applied at run start + live), deals 6->16, Forge shop service (`shopForge`). Deferred polish:
  named-pact heat, codex, separate rare currency.
- M7 DONE - bot aggression tuned (pushes when hot), sim cap raised to 320s, heat sweep confirms
  the relative curve (heat raises difficulty, no sponge wall), content `affinity:` token data-ify,
  ASCII rule verified, ~58 content entries.

Post-M7 polish pass (DONE): per-biome grade tint (biomes now read at a glance), enemy status
body-tints (burn/freeze/corrode/chill/shock show on the enemy body), bot-aggression tuning
(cleaner balance signal; status uptime ~20%).

Remaining polish (live-tunable, not blockers): absolute win-rate balance (a human + Mirror
feel-gate matter; the sim uses a fresh no-Mirror profile), named pacts, codex, separate rare
currency.

# Pulse - Post-M2: the Roguelite Game Phase

## Context

The look (M1) and breadth/RT (M2) are done; the *feel* is proven (fast movement + dash,
one hitscan gun with CS2 recoil, 3 enemies, pickups, juice, instant-restart-with-score).
The design doc always called Pulse *"a fast first-person gun roguelike"*
(docs/PROTOTYPE_SPEC.md:3) and Section 8 names this next phase: run structure -> build
variety. This phase keeps the **proven combat feel as its foundation** and wraps it in a
roguelite. (The firing core is generalized for weapon archetypes and build mods perturb
feel, so the feel *primitives* are preserved, not the literal code - see Design principles.)

**Direction:** multi-arena runs; roguelite with meta-progression; **keep it simple for
now.** The build is **three stacking axes** that combine into powerful runs:

1. **Weapons** - a roster of distinct guns, each with a **scaling power level + effects**
   (no alt-fire). You collect/upgrade weapons; their power and effects stack into the build.
2. **Utility / abilities** - equippable **active** abilities on cooldown (dash mods,
   grenade/deployable, ultimate).
3. **Passives** - stacking item effects (RoR2-style counts): stats + on-hit (fire rate,
   crit, lifesteal, chain, explosive, move/dash...).

The design goal is the power-fantasy: weapons x abilities x passives **stack** into unique,
potentially broken-strong runs. (Reactive music deferred; curses/modifiers deferred.)

## Design principles (resolved)

These govern every balance number and resolve the core tension between the roguelite and the
proven feel (decided 2026-06-20):

- **Borrow RoR2's item structure, but Doom Eternal / Returnal's encounter model.** Enemy
  **base HP stays low and fixed** - every enemy keeps dying in a small, decisive number of
  hits at full stack. Runs escalate by **count, speed, aggression, and composition** (more
  ranged, elites, mixed swarms), **never by per-enemy HP**. Player power makes kills
  *snappier* (more one-shots, faster clears); the game answers with *more and nastier*, not
  *tankier*. This keeps "never empty a mag into one thing" (PROTOTYPE_SPEC s5) true forever
  and turns escalation into a movement/flow test - exactly what the prototype proved.
- **One intensity/greed spine drives escalation + music + risk-reward.** Aggressive fighting
  builds intensity; intensity drives difficulty and the techno layering; an optional "push
  it" path (faster clear / opt-in elite wave) trades risk for better loot. One mechanic
  unifies the run structure with the deferred reactive-techno identity (the `combatIntensity_`
  wiring in Phase A is its first increment).
- **Weapons differ by firing archetype, not by numbers.** A small firing kernel
  (hitscan-auto, precise/burst, multi-pellet spread, projectile, beam/charge); each weapon =
  archetype + power level + intrinsic effects. Distinct *feel* is the brand, so this is worth
  the cost: it genuinely reworks `tryFire`/`updateWeapon`/`acquireTarget` (the one place the
  "wrap, don't rebuild" rule bends).
- **The feel gate covers built runs.** Build mods change movement and fire cadence, so feel
  is judged on a fully-stacked run, not just base combat.

## Combat depth / skill expression

Difficulty scales by enemy intensity (count/aggression/composition), so depth must come from
what the horde *demands*, never from enemy HP. The test for every mechanic: does it punish
back-pedaling and reward decisive movement + prioritization + aggression? The levers:

- **Threat composition as a triage puzzle (highest leverage).** Heterogeneous threats with
  distinct counters - rushers punish standing still, leading-shot ranged punish predictable
  movement, zoners punish camping, and **priority targets** (healer/shielder/summoner/bomber)
  punish tunnel-vision by changing the fight if ignored. **Weak points + counterplay** per
  enemy (exposed core, stagger a rusher mid-lunge, shoot an orb out of the air) reward aim and
  knowledge - headshot detection already exists. **Elite affixes** (volatile/shielded/fast/
  regen) recombine the threat space cheaply and scale difficulty by changing the puzzle, not
  inflating HP.
- **Aggression economy (defense from offense).** Survival comes from forward pressure, not
  turtling: kill-streak / no-hit buffs (Returnal adrenaline), health/ammo/dash-charge on kill,
  and intensity that decays when you disengage. This mechanizes the proven flow thesis, feeds
  the intensity spine, and charges abilities (below).
- **Movement is the signature skill axis.** Deepen the dash (i-frames through telegraphs,
  reposition through the swarm, dash-resets); make threats **spatial/telegraphed** (zones to
  dodge, not numbers ticking down); use floor-plan (cover, chokes, hazards) plus the existing
  boid separation so kiting and herding the crowd are positional skills. (Sim is planar - no
  real verticality.)
- **Precision pays.** Headshot / weak-point bonus damage, fast target switching, and - with
  the archetype roster - **weapon-swapping as skill** (right tool for the current threat mix =
  the Doom Eternal combat-puzzle loop).
- **Abilities are the timing/placement skill axis**, distinct from aim and routing - see
  Phase B.

## Run loop

A *run* = a seeded sequence of rooms grouped into a few **sectors**, each sector a biome
(material set + enemy/reward pool) ending in a boss. Per room: clear escalating waves ->
**reward choice** (pick 1 of 3 from a mixed weapon/ability/passive pool) -> next room.
Death ends the run; final boss-clear wins. Roguelite frame: **Hub** (spend meta,
pick starting loadout) -> **Run** -> payout -> Hub. Each run draws a **dedicated seeded
run-RNG** (room/reward/biome sequence) kept separate from the combat-jitter `rng_`
(`tryFire` spread), so a seed reproduces a run no matter how the player shoots.
Difficulty scales per room/sector by **count/speed/aggression/composition, not enemy HP**
(see Design principles), gated by the intensity/greed spine.

## Phases (lean; each playable + verifiable)

**Phase A - Run/room state machine.** A phase enum `{ Hub, InRoom, RoomCleared, Boss,
RunOver }` + a `Run` (sector/room index, seed, room sequence) above the flat `update()`,
plus the dedicated seeded **run-RNG** (see Run loop). **Reuse the one existing arena for
every room first**; vary enemy composition + escalation. This phase also builds the
**wave + clear-condition subsystem that does not exist yet**: `updateSpawning` is currently
an endless trickle capped at `spawnMaxConcurrent` with no notion of "done", so Phase A adds
discrete waves and a room-clear test. Clear all waves -> RoomCleared -> exit -> next room ->
boss -> death/win -> restart. **Drive `combatIntensity_` from run/wave state here** (ramp
across a room, drop on death): the `audio.setMusic(..., combatIntensity_)` hook already
exists, so the run feeds the music from day one - a cheap down payment on the deferred
reactive techno (Phase E). *Files:* new `src/Game/Run.{hpp,cpp}`; PulseGame `update`/
`updateSpawning`/`resetRun` become phase-aware. *Verify:* a full run plays
room->boss->death->restart via `--bot-test`.

**Phase B - The stacking build (the heart).** **Design the effect vocabulary first:** the
hook points combat exposes (on-fire, on-hit, on-kill, on-dash, on-ability-use, on-damaged,
stat-mods) ARE
the space of items that can ever exist - enumerate them before building the layer. New
numbers and recombinations are data; a genuinely new effect *type* is always a new C++ hook.
Then build in this order (easiest/highest payoff first):
- **Passives:** an item registry (data-driven) + a player inventory of **stacks**
  (`map<id,int>`) + an effect layer combat reads (`tryFire`, `updateWeapon`, `damagePlayer`,
  `updatePlayer`). ~16 starter items.
- **Weapons:** generalize the single hitscan `Weapon` into a small **firing-archetype kernel**
  (hitscan-auto, precise/burst, multi-pellet spread, projectile, beam/charge); each weapon =
  archetype + a **power level** that scales (upgrades/dupes raise damage + unlock effects) +
  intrinsic effects; acquire + swap a small loadout. This is the one place combat is genuinely
  reworked (`tryFire`/`updateWeapon`/`acquireTarget` go archetype-dispatched), so it lands
  after passives and gets its own feel pass. Weapon effects stack with passives.
- **Abilities (the timing/placement skill axis):** dash stays the always-on core verb; add
  **3 equip roles, ~2 new buttons** - a **dash mod** (modifies the core verb, mobility without
  a button), a **tactical** (short-charge throwable/deployable that shapes space: grenade,
  gravity well, decoy, barrier), and an **ultimate** (long-charge "moment"; prefer
  execution-window ults like time-slow / damage-amp over fire-and-forget nukes). **Charge them
  through aggression** (kills/damage/intensity), not just a wall-clock cooldown, so flow play
  buys agency and turtling starves it. The gun stays primary DPS; abilities are
  enablers/control/mobility/moments and a rich source of build pivots (a gravity well makes a
  single-target weapon AoE). MVP if scope is tight: dash-mod + ultimate, add tactical later.
- **Reward choice UI** on the GPU HUD (`buildHud` / `UiDrawList`): pick 1 of 3 on
  RoomCleared, mixed pool across the three axes. Needs infrastructure the game lacks
  today: a **paused/time-stopped selection state** (the only non-play state now is the
  `restartTimer_` death branch), a **mouse-mode switch** (selection cursor vs mouselook),
  and a **headless selection hook** so `buildBotInput` can pick a reward (it only scripts
  move/fire today) for the automated full-run verify.
*Files:* new `src/Game/Build.{hpp,cpp}` (registries + player build + apply hooks); combat
hooks in PulseGame; content in a new `config/pulse.content` so pools grow without recompiling - but note
`Config.cpp` is a flat key=value parser today, so structured per-item records (each with a
typed effect list) need a small structured-table parser, and only recombinations of the
fixed effect vocabulary are data (a new effect *type* is code). *Verify:* clearing rooms
grants choices; weapons/abilities/passives visibly stack and scale (HUD captures; bot picks).

**Phase C - Meta-progression + hub + save.** Persistent **meta-currency**, an **unlock
list** (new weapons/abilities/passives/enemies drip in), a **Hub** phase (spend meta, pick
starting loadout), and **save/load** to a versioned file in the user/app dir (graceful
fresh-profile fallback). Meta spending is **content/option unlocks only** (new items +
starting-loadout options = more *choices*), never permanent stat power, which would
grind-gate and cut against the clean-skill thesis. **Save stability:** key items/weapons by stable
string/hashed IDs in the registry, never array index, so adding content later does not
invalidate saves. *Files:* new `src/Game/Meta.{hpp,cpp}`; Hub UI; run-over payout +
persist. *Verify:* meta persists across restarts; unlocks expand run pools; a save written
before new content is added still loads cleanly after.

**Phase D - Multi-arena / biomes + content breadth.** Procedural/templated **room layouts**
(extend the arena wall-grid + 3D arena-mesh gen) and **biomes** (distinct PBR material sets
per sector via the M2 cgltf + DDS + PolyHaven pipeline; props/cover via `MeshFile`/cgltf).
Two caveats from the current code: the sim is **planar** (a fixed `Arena[][]` grid, player
height is a scalar fraction of room height), so biome variety is floor-plan + dressing, not
true 3D/vertical level design; and GPU meshes/materials are built **once** in
`ensureGpuResources`, so per-room/per-biome swaps need that lifecycle reworked from once-only
to per-room. **Content:** more enemy types + elites; fill the weapon/ability/passive pools.
**Bosses are their own scoping pass, not drop-in content:** the enemy model today is 3 fixed
kinds with simple FSMs, no multi-phase behaviour, no telegraphed attack patterns, no
boss-arena framework - design that as a dedicated step (the Phase A `Boss` enum is a stub
until then). *Files:* room/biome gen in Run.cpp + PulseGame arena-mesh gen; enemy
system (`EnemyKind`, `updateEnemies`, `styleFor`, `spawnEnemy`); materials in
`ensureGpuResources`. *Verify:* runs traverse visually distinct arenas; vision-loop captures
per biome; bosses readable.

**Phase E - reactive techno** (spec Section 6). The first implementation is the
state/intensity-driven stem method in `docs/PULSE_MUSIC_SYSTEM.md`: short synchronized
loops, phase-owned sections, and intensity-owned layer gain. Later work adds bar-quantized
stingers and horizontal resequencing, not a single long track.

## Reuse (do not rebuild)
- **Combat feel primitives** are the foundation: movement/dash, recoil/juice, hit feedback,
  hit-stop, damage cues - reused as-is and read by the build layer. (Exceptions, per Design
  principles: the firing core is generalized for weapon archetypes, and build mods perturb
  movement/cadence - so the feel gate covers built runs.)
- **Hot-reload config** (Tunables + config/pulse.tuning + `loadConfig`, F5) - extend for
  content data.
- **GPU HUD** (`UiDrawList`/`buildHud`) for all roguelite screens.
- **Seeded RNG**: keep `rng_` for combat jitter, add a **separate seeded run-RNG** for
  run/level/reward/biome generation (one shared stream makes seeds non-reproducible, since
  `tryFire` spread already draws from `rng_`). `score_/restartTimer_` evolve into the
  run/meta loop.
- **M2 asset pipeline** (cgltf `MeshFile`, BCn DDS, PolyHaven) for biomes/props.
- **Headless verify** (`--bot-test`, `--pose`, capture) - extend `buildBotInput` to play
  full runs + pick rewards for regression.

## Critical files
- `src/Game/PulseGame.{hpp,cpp}` - phase-aware loop, combat reads build mods, HUD screens,
  arena gen, enemy/weapon extensions.
- New `src/Game/Run.{hpp,cpp}`, `Build.{hpp,cpp}`, `Meta.{hpp,cpp}` - keep systems modular.
- `src/Engine/Config.cpp` + `config/pulse.tuning` (+ `config/pulse.content`) - data-driven
  content/tuning.

## Verification
- **Automated:** `--bot-test` plays full runs (clear rooms, pick rewards, fight a boss),
  deterministic per seed, no crash, sane score/HP; headless captures of each HUD screen +
  biome; D3D12 GBV stays clean.
- **Balance simulation (exploit the determinism):** seeded runs + the headless bot make it
  cheap to sim many runs and report TTK distributions, DPS-vs-sector curves, and run-clear
  rates - an automated signal for bullet-sponge drift and degenerate builds. Balance, not
  crashes, is the dominant risk of this phase.
- **Feel gate (developer, spec Section 4/7):** the roguelite must not degrade combat feel,
  judged on a *built* run (stacked passives/weapons), not just base combat; the "five+
  minutes without being asked" bar applied to a full run.
- **Save:** persists across restarts; corrupt/missing degrades to a fresh profile.

## Notes
- Build **systems first on one arena** (A-C), then content breadth (D). Don't author big
  content pools before the loop is fun.
- The **effect vocabulary** (the build-layer hook points) must be designed before Phase B
  builds that layer, since the hooks define every item that can ever exist. The later
  content+balance pass (weapon/ability/passive lists + numbers + unlock list) is its own
  design sub-step before Phase D bulk authoring, and should design a few **synergies/build
  pivots** (item A enables item B), not just additive stacks - additive-only stacking is
  "more numbers, same feel".
- Keep PulseGame modular (new Game/* files) to avoid a monolith.

## Design decisions (resolved 2026-06-20)

The larger tensions, now resolved (see Design principles for the first three):

1. **Stacking power vs. low-TTK feel:** resolved by the encounter-model principle - scale
   count/aggression/composition, fix low base HP, feel gate on built runs.
2. **Weapon variety:** **distinct firing archetypes** - real feel variety is the brand;
   accept the firing core is reworked, sequence it after passives in Phase B.
3. **Run agency / risk-reward:** the **intensity/greed spine** is the run's organizing
   mechanic and doubles as the music driver. Rarity tiers + optional elite/challenge rooms
   layer on in Phase D; in-run economy/rerolls are a stretch - revisit only if the loop needs
   more agency once it is playing.
4. **Meta model:** **content/option unlocks only**, no permanent stat power (Phase C).
5. **Scope:** **all three axes in Phase B** as sequenced (passives -> weapons -> abilities),
   then meta (C), then biomes/content (D). Passives still land first as the cheapest,
   highest-variety, lowest-feel-risk axis; weapons (highest feel-risk) land last in B with a
   dedicated feel pass.

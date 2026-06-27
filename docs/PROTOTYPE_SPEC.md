# Prototype Spec, Working title: *PULSE*

A fast first-person gun roguelike with clean, COD-crisp gunplay, a dark
atmospheric world, and a driving techno score that builds with the run.

This document specifies the **prototype only**, the smallest build that
answers the single question the whole game depends on:

> **Does moving fast and shooting feel clean, crisp and satisfying, and does
> the techno score make it feel like flow rather than noise?**

If the answer is yes, everything else is built outward from a proven core. If
no, we have spent two to four weeks, not a year. Nothing in this spec exists to
make the prototype look like a game. It exists to expose the core, naked, and
test whether it is fun.

---

## 1. What this prototype is NOT

To protect scope, the prototype explicitly excludes:

- Art, environment design, or atmosphere beyond a grey-box room
- More than one weapon
- More than 2-3 enemy types
- Progression, upgrades, builds, or meta-systems
- Procedural generation
- Menus beyond instant restart
- Story, narrative, UI polish
- Multiplayer of any kind (this is single-player; do not entertain multiplayer)

If a task is not in section 4, it is out of scope for the prototype.

---

## 2. The design target (the thing being tested)

The feel we are aiming for is **fast movement + clean gunplay**, explicitly:

- **Fast/kinetic, not deliberate.** Constant motion. The player is always
  moving, dodging, repositioning. Standing still is not the optimal state.
  Reference: Doom Eternal, Returnal movement; the *flow* of a good run.
- **COD-clean, not CS-precise.** The skill is fluid target acquisition and
  weapon handling *while moving*, not stop-and-snipe crosshair discipline.
  Shooting accurately while moving must feel good, not punished.
- **Decisive, not spongy.** Kills are snappy. Low time-to-kill. Enemies die
  fast and satisfyingly. Bullet-sponge health bars are the enemy of "clean."
- **Rhythmic.** A driving techno track underneath; the test includes whether
  combat feedback can sit in sympathy with the beat to create flow state.

The two failure modes we are testing against:
1. Moving-and-shooting feels mushy / floaty / imprecise (gunplay not clean).
2. Techno is just background music, not integrated into the feel (no flow).

---

## 3. Engine and tech decisions

**Engine: a custom native C++ engine (Win32 + Direct3D 11).** Not Unreal, not
Godot, not Unity.

Rationale, the deciding factor is the **development model: autonomous AI
development.** Claude must be able to build, run, AND *verify* the game in a tight
loop on its own. That demands an engine that is all-text source Claude reads and
edits, compiles in seconds, runs headless, and dumps frames/audio Claude can
inspect and iterate on. Unreal fails this (binary `.uasset` content Claude can't
edit, multi-minute builds, editor-bound runtime); Godot has a thin polished-FPS
track record and a clunkier headless-capture path; commodity engines optimise for
a human-in-editor workflow, not an AI build/run/verify loop. The custom engine is
the most AI-instrumentable option, and that instrumentability is the point.

Quality is not traded away for this. A custom engine has no inherent quality
cap, the best-looking shooters ship on custom engines (id Tech/Doom,
Northlight/Alan Wake 2, RE Engine). section 3a-section 3e are how a small, AI-driven team
reaches a premium, atmospheric look without a studio. The binding constraint on
visuals is art-direction coherence + the lighting/post pipeline, NOT the engine.

Notes:
- Target **120+ fps** in grey-box. Tight gunplay lives or dies on frame rate and
  input latency; if grey-box can't hold high, stable frames, fix that first.
- Input: mouse + keyboard primary. Controller support is out of scope for the
  prototype.

### 3a. Rendering pipeline, the "flatter" stack

Build this once; it is the difference between "premium" and "jank." Priority order:

1. **Linear/sRGB colour management + AgX (filmic) tonemapping.** This single
   change removes roughly half of all indie "jank." Do it first.
2. **PBR materials**, metallic/roughness, with normal / AO / emissive maps.
3. **Shadows**, cascaded shadow maps for dynamic lights, plus **baked lightmaps**
   for the static arena (offline-GI-quality lighting at zero runtime cost, ideal
   for a mostly-static room).
4. **SSAO**, contact shadows; grounds objects so nothing looks like it floats.
5. **Bloom + a subtle colour grade.**
6. **TAA**, kills shimmer/aliasing, a major jank source.
7. **Volumetric fog**, the core of the dark, moody, atmospheric identity.
8. **Screen-space reflections / reflection probes**, and **decals**.
9. **A GPU particle system**, muzzle flash, impacts, debris, ambient motes.
10. *Later, optional:* SDF/voxel GI-lite for dynamic light bounce.

### 3b. Assets, engine is asset-agnostic; SOURCE quality, don't only generate it

The engine loads standard **glTF/FBX + PBR texture sets**, so it uses the same
content UE/Unity can. Asset quality is NOT capped by AI generation:

- **Quixel Megascans (free)**, film-grade scanned environment assets/materials.
- **Marketplaces** (Fab, Sketchfab) and photogrammetry for hero props.
- **Blender + AI generation** for bespoke pieces (weapon, hands, enemies,
  viewmodel), see Part III of the plan (the asset pipeline).
- Maintain a **coherent material + texel-density spec** so sourced and authored
  assets sit together instead of clashing.

### 3c. The AI build/run/verify loop (development model)

- All engine + game source is **text Claude edits directly**; builds are seconds,
  runs are headless (capture flags like `--pose`, `--bot-test`, `--render-sfx`).
- **Visual verification is automated:** headless frame capture -> a **vision model
  critiques the frame** ("shadow detached at base", "materials read plastic",
  "scene lacks atmospheric depth") -> Claude fixes -> re-capture. This closes the
  loop a code-only AI couldn't, and is what makes a high visual ceiling reachable
  autonomously.
- **Feel and motion stay the developer's call.** No vision model can judge a dash
  or recoil from a still frame. AI + vision own the *look*; the developer owns the
  *feel*. The loop: Claude builds/iterates -> developer playtests -> reports what
  feels wrong in concrete terms -> Claude adjusts. Taste is the developer's; Claude
  is the hands.
- **Feel/tuning values live in a hot-reloadable config** (`config/pulse.tuning`,
  F5 to reload) so every feel number, move speed, dash, recoil, fire rate, TTK,
  shake, FOV, hit-stop, tunes live without recompiling. This is not optional;
  feel-tuning is the point of the prototype.

### 3d. Audio, real files, not pure synthesis

- The foundation is **real audio assets**: recorded SFX samples (gunshot, reload,
  impact, hit, kill) plus a driving four-on-the-floor techno score built from
  short, beat-locked stems. See `docs/PULSE_MUSIC_SYSTEM.md` for the runtime music
  method and stem contract.
- Do not treat a single long track as the score. The score must be state-driven:
  hub/reward/combat/boss/run-over choose the musical section, and run intensity
  crossfades the combat layers.
- Procedural/adaptive layers may prototype the method, but the target is authored
  replacement stems and recorded SFX. Synthesised gunshots/music are a fallback,
  not the final identity.

### 3e. Honest quality ceiling

- **Target (reachable):** premium, coherent, atmospheric, near-AAA fidelity for
  one contained arena via sourced assets + baked lighting + the full post stack.
  Reference bar: DOOM 2016-tier lighting/material competence; the *mood* of
  Returnal at far lower effect density.
- **Not the target (out of reach autonomously):** Returnal's particle-*spectacle*
  tier or open-world AAA, those need a specialist VFX/performance team. Aim for a
  distinctive, coherent stylised look, never a fidelity arms race.

---

## 4. Build scope, the one room

### 4.1 The space
- A single grey-box arena. Boxed, with cover blocks, a few elevation changes,
  and room to move at speed. No art, no textures beyond default grey.
- Sized for fast movement, large enough to run, dodge and reposition, small
  enough that combat is constant (no dead walking time).

### 4.2 Player movement (FAST path)
This is half the feel. Tune obsessively.
- High base move speed.
- A **dash/dodge** with a short cooldown, the core kinetic verb. Should feel
  snappy and responsive, give brief i-frames or at least a clear evasive
  payoff. This is the Returnal/Doom dance enabler.
- Air control / strafe responsiveness that feels fluid, not floaty.
- Acceleration/deceleration tuned so the player feels *connected* to input ,
  no skating, no mud. Movement should feel like an extension of intent.
- No fall damage, no stamina in the prototype. Keep movement pure.

### 4.3 The one gun (90% of the effort, see section 5)
- A single hitscan or fast-projectile weapon. Recommend **hitscan** for the
  prototype to isolate "clean feel" without projectile-travel complexity.
- Automatic or burst, pick whichever feels cleaner in testing; do both and
  compare.
- Must feel good to fire *while moving* (the whole thesis).

### 4.4 Enemies (2-3 types, readable behaviour)
- **Type A, Rusher:** moves toward the player, forces repositioning. Tests
  whether the dash/movement loop feels good under pressure.
- **Type B, Ranged/telegraphed:** fires a clearly telegraphed shot the player
  can dodge. Tests read-and-react.
- **Type C (optional), Tanky/positional:** slightly more health, forces
  target prioritisation. Only add if A and B already feel good.
- Enemies must **die fast and satisfyingly** (snappy, decisive, see TTK).
  Readable telegraphs over smart AI; behaviour clarity beats behaviour depth at
  prototype stage.
- Simple spawn system: waves or continuous trickle into the room.

### 4.5 The loop
- Spawn into room -> fight -> die -> **instant restart** (no menu, no load).
- A score or wave counter so "do better than last time" exists in the crudest
  form. This is the roguelike heartbeat in embryo.
- That's it. No upgrades, no choices, no map. Just: enter, fight, die, again.

---

## 5. Gunplay feel, the make-or-break checklist

The entire prototype lives here. "Clean COD-crisp gunplay while moving" is a
craft, not a setting. Layer these and tune each obsessively:

**Hit feedback (most important):**
- Crisp, punchy fire sound with weight and high-frequency snap.
- Distinct, immediate **hitmarker** (visual + audio) on landing a shot.
- Clear impact effects on enemies (flinch, hit flash, particle).
- A satisfying, distinct **kill** confirmation (sound + visual), the kill
  should feel different and better than a hit.

**Time-to-kill:**
- Keep it LOW. Enemies should die in a small, decisive number of clean hits.
- Tune so killing feels *snappy and rewarding*, never spongy. If you're
  emptying a mag into something, the feel is wrong.

**Weapon responsiveness:**
- Instant trigger response, no perceptible delay between click and shot.
- Recoil/kick that has weight but stays *controllable while moving* (COD-clean,
  not CS-punishing). The gun should feel like it responds to you.
- Tight, fast, satisfying reload animation and sound.

**Moving-and-shooting (the thesis):**
- Accuracy while moving must be viable and feel good, movement should not
  cripple aim the way CS does. This is the core differentiator from a
  precise/static shooter; get it wrong and the fast path collapses.
- The feel of dashing, then snapping onto a target and dropping it cleanly,
  is THE moment the prototype must nail.

**Camera/juice:**
- Subtle screen shake / kick on fire (tuned, too much ruins clean feel).
- FOV and motion tuning that makes speed feel fast without nausea.
- Hit-stop / micro-pause on kills is worth experimenting with for crunch.

**What to avoid (kills "clean"):**
- Bullet-sponge enemies. Floaty movement. Input lag. Mushy/quiet hit feedback.
- Random spray (recoil should be learnable, not slot-machine).
- Over-juicing (excessive shake/effects that obscure rather than satisfy).

---

## 6. The techno integration test (the novel hypothesis)

This is the riskiest, most original part of the concept, so test a rough
version EARLY, not after everything else is built.

**Tier 1 (minimum):** A driving techno track loops under the combat. Does fast
shooting simply *feel better* with a relentless beat underneath? (Hotline
Miami's baseline effect.)

**Tier 2 (the real test):** Sync combat feedback to the music, even crudely.
Do gunshots / hits / kills feel better when they sit in rhythmic or tonal
sympathy with the track? Experiment with:
- Quantising or pitching feedback sounds to the track's key/tempo.
- Intensity layering: the music builds/adds layers the longer the player
  survives, dropping back on death. Survival = musical escalation.

**Tier 3 (stretch, only if Tier 2 sings):** Light rhythm-reward, a subtle
bonus or feedback flourish for actions on the beat. Do NOT build a rhythm game;
this is seasoning, not a mechanic. Flow should come from feel, not from forcing
the player to play in time.

The question: does "techno + clean fast shooting" produce *flow state*, or is
it just music playing over a shooter? If Tier 1-2 don't elevate the feel,
that's critical information about the whole concept, learn it in week two.

---

## 7. Success criteria (how we judge the prototype)

The prototype succeeds if, with NO art and NO content beyond one room:

- [ ] Frame rate is high and stable; input feels instant.
- [ ] Firing the gun feels crisp and satisfying *on its own* (just shooting a
      wall should feel good).
- [ ] Shooting accurately *while moving fast* feels clean and skillful, not
      mushy or punished.
- [ ] Kills feel snappy and decisive, no sponginess.
- [ ] The dash + move + shoot loop produces a feeling of *flow*.
- [ ] The techno underneath measurably improves the feel (Tier 1 minimum;
      Tier 2 ideally).
- [ ] **The honest test:** the developer catches themselves replaying the
      single room for five+ minutes without being asked to, because it's fun.

That last one is the real green light. A grey box you can't stop playing is a
game waiting to happen. A polished room you put down after one go is not.

---

## 8. What comes AFTER a successful prototype (not now)

Listed only to show the build order, NOT to build yet:
1. Enemy variety + readable behaviour depth.
2. Run structure, multiple rooms, escalation, the full roguelike loop.
3. Upgrades / build variety (the roguelike depth and replayability).
4. **Then** art direction and atmosphere, the dark, beautiful, moody look
   that is the game's market differentiator. (Deliberately last: atmosphere is
   the differentiating *layer*, applied once the core is proven fun.)
5. Full reactive techno score, integrated with the run's intensity curve.

Feel first. Depth second. Beauty and music as the identity layer on top.

---

## 9. Current state & next priorities

**Already built (the core loop exists):** the custom C++ engine, grey-box arena,
fast movement + dash, a hitscan weapon with CS2-style learnable recoil, the
three enemy types (rusher / ranged-projectile / tank) with telegraphed attacks
and separation, ammo/health/shield pickups, an on-screen damage-direction
indicator, instant-restart with score, procedural techno + SFX, and a
Blender-authored AK viewmodel with hands. The thesis question (does fast movement
+ clean shooting + techno feel like flow?) is now testable.

**Next priorities**, bring the prototype from "works" to "premium," per section 3:

1. **Real audio (section 3d).** Swap procedural SFX/music for real files: recorded
   gunshot/reload/impact samples plus authored adaptive techno stems following
   `docs/PULSE_MUSIC_SYSTEM.md`. This is the fastest single quality win and removes
   the synthesis dead-end without falling back to a random long track.
2. **The "flatter" rendering pipeline (section 3a), in order:** colour management + AgX
   tonemapping -> PBR materials -> shadows + baked lightmaps -> SSAO -> bloom/grade ->
   TAA -> volumetric fog. Verify each stage with the AI vision loop (section 3c).
3. **Lock an art direction (section 3b/section 3e)**, palette, material language, fog/mood ,
   then upgrade assets (Megascans for the arena, bespoke for hero pieces). Art
   coherence is the #1 premium-vs-jank lever.
4. **Keep feel as the gate.** No new content/systems until the single room is
   genuinely fun and looks intentional. Feel is judged by playtesting (developer);
   look is iterated by Claude + the vision loop.

The honest test still stands (section 7): the developer catches themselves replaying the
one room for five+ minutes because it's fun, *and* it now looks like a game you'd
stop to screenshot.

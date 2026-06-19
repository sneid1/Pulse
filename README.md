# Pulse

A contained, atmospheric arena FPS on a clean-sheet custom engine: **Direct3D 12 +
DXR**, a render-graph RHI, bindless resources, and an AI build/run/verify loop
(all-text source, headless capture, vision-model critique).

This is a clean-sheet repo. It inherits no prior renderer and no prebuilt assets —
the engine and all content are built from scratch against the plan, so nothing old
can be incorrectly reused.

## Start here

- **The plan:** [`docs/Plan — PULSE engine and assets.txt`](docs/Plan%20—%20PULSE%20engine%20and%20assets.txt)
  — engine design (Part I), the concrete M0 interface (Part II), and the asset
  pipeline (Part III).
- **Spec:** [`docs/PROTOTYPE_SPEC.md`](docs/PROTOTYPE_SPEC.md) — the experience target
  (the plan's `§` references point here).
- **Rules:** [`docs/PROJECT_RULES.md`](docs/PROJECT_RULES.md).
- **What's in the seed and what isn't:** [`SEED_NOTES.md`](SEED_NOTES.md).

## Status

M0 (engine foundation) is not built yet — **the project does not compile.** The
seeded gameplay (`src/Game/PulseGame.*`, `src/main.cpp`) holds the game and is the
source to port; its render/audio submission layer is rewritten against the new engine
in M0. See SEED_NOTES.md.

## Build

```powershell
.\Build-Pulse.ps1   # cmake configure + build (MSVC + Ninja). Will not link until M0.
```

## Assets

There are **no runtime assets** in this repo by design. Every model, texture, and
sound is authored from scratch to the glTF + BC7/BC5 PBR contract in Part III of the
plan and validated in-engine via the headless capture + vision-critique loop. Asset
creation uses the bundled Blender (`tools/blender/`, gitignored) and the
`sneidgame-asset-forge` skill.

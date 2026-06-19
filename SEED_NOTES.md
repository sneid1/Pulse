# Seed notes — what this clean-sheet repo contains, and what it deliberately doesn't

This repo is a clean-sheet start built against the plan. It carries the *method* and
the *game*, and builds the *engine* and all *content* from scratch. Nothing prior is
referenced at runtime or build time — the repo is fully self-contained.

## In the seed

- **Docs / method:** the canonical plan (`docs/Plan — PULSE engine and assets.txt`,
  engine + asset pipeline in one) and the spec (`docs/PROTOTYPE_SPEC.md`).
- **Gameplay source, to PORT:** `src/Game/PulseGame.{hpp,cpp}`, `src/Game/Tunables.hpp`,
  `src/main.cpp`. These hold the actual game (movement, combat, enemies, the bot
  test, scripted captures, CLI flags). Their render/audio submission still uses the
  old engine's types, so **they do not compile as-is** — that layer is rewritten
  against the new engine in M0 (Part II of the plan has the old→new mapping).
- **Engine-agnostic utilities:** `src/Engine/Config.{hpp,cpp}` (tuning loader),
  `src/Engine/Input.hpp`, `src/Engine/Math.hpp`.
- **Game config:** `config/pulse.tuning`.
- **Asset-pipeline tools:** a self-contained Blender 5.1 (`tools/blender/`,
  gitignored), the deterministic Blender authoring scripts
  (`tools/blender/build_pulse_*.py`, `render_pulse_weapon_inspection.py`) as patterns
  to extend to the glTF+PBR contract, and `tools/*.py` helpers.

## Deliberately NOT in the seed

- **Any prior renderer/audio:** no CPU rasterizer, no D3D11 presenter, no `waveOut`
  audio, no old GPU scene/mesh renderer, no old window/present harness. The D3D12 +
  render-graph core and a WASAPI audio backend are built fresh per the plan. Do not
  introduce a D3D11 path (PROJECT_RULES.md).
- **All prebuilt assets:** no inherited weapon mesh, textures, sourced model packs, or
  audio files. Every model/texture/sound is authored from scratch to the Part III
  contract and validated in-engine. `assets/{models,textures,shaders,source}/` are
  empty targets.

## Build status

**Does not compile yet.** M0 must land first: the RHI over D3D12, the render graph,
the WASAPI audio backend, the new window/swapchain present path, the PulseGame
submission rewrite, and a rewritten `main.cpp`. Until then this is a scaffold +
source-to-port + the method to build everything else.

# tools/meshy - Meshy AI asset generation

Helper for the Neon Ink Brutalism asset regeneration (workstream W9 in
`docs/Plan_PULSE_neon_ink_brutalism.md`). Drives the Meshy text-to-3d OpenAPI to
generate raw geometry; the Pulse LOOK comes from the engine (shading bands, ink
outlines, and the locked master-material library in `config/pulse.style`), NOT
from Meshy textures.

## Discipline (do not skip)

Meshy gives FORMS, the engine gives the LOOK. Per the art direction (section 13),
production assets must be converted to the shared master materials and clamped to
the approved palette. So:

- Generate PREVIEW-only meshes for static props/architecture (textures discarded).
- Use a refine (texture) pass ONLY where rigging needs it (Meshy rigging wants a
  textured humanoid).
- Raw output lands in `assets/meshy/raw/` (gitignored staging). Only promote an
  ACCEPTED asset into `assets/` deliberately; keep originals until then.

## API key

Never commit the key. Resolution order:
1. environment variable `MESHY_API_KEY`
2. `tools/meshy/key.txt` (gitignored)

## Usage

    tools\meshy\meshy.bat -Command balance
    tools\meshy\meshy.bat -Command generate -Prompt "..." -Out assets\meshy\raw\name.glb
    tools\meshy\meshy.bat -Command generate -Prompt "..." -Out out.glb -Model meshy-5 -Polycount 8000 -Topology triangle
    tools\meshy\meshy.bat -Command status   -TaskId <id>
    tools\meshy\meshy.bat -Command download -TaskId <id> -Out out.glb

`-Refine` chains a texture pass after the preview (needed before rigging).

## Credit cost (meshy-6 / lowpoly tier; meshy-5 preview is cheaper, ~5)

    preview 20   refine 10   image-to-3d 20/30   retexture 10
    remesh 5     auto-rig 5  animation 3/clip     convert 1   resize 1

Failed tasks are refunded. Check remaining credits with `-Command balance`.

## Prompt guidance for the art direction

Favor bold, simple, confident forms: "brutalist", "monolithic", "sharp beveled
edges", "blocky", "simple bold geometry", "low surface detail", "no clutter",
"game-ready". Avoid asking Meshy for grunge, greebles, photoreal texture, or
neon paint - color and emission are applied in-engine via the master materials.

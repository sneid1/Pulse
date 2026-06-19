# assets/

**Empty by design.** This repo inherits no prebuilt content; every asset is authored
from scratch to the contract in Part III of `docs/Plan — PULSE engine and assets.txt`
and validated in-engine via the headless-capture + vision-critique loop.

Target layout (populated as content is authored):

- `models/`   — runtime geometry, glTF 2.0 binary (`.glb`), one logical asset per file.
- `textures/` — runtime PBR sets, BC7/BC5 `.dds` with full mip chains
                (`<asset>/<asset>_basecolor|_normal|_orm|_emissive.dds`).
- `shaders/`  — HLSL (SM6.6 + DXR), compiled by dxc.
- `source/`   — editable source kept OUT of the runtime path (`.blend`, source PNG/EXR
                maps, raw generator output, audio source). Never ship raw 4K source.

See Part III §1 for the full asset contract (orientation, scale, texel density tiers,
naming) and §6 for the verify loop.

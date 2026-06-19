# Pulse Project Rules

- **No silent fallbacks.** A missing required engine path, shader, or asset must fail
  loudly with a clear error — never a quiet substitute.
- **Canonical engine.** The only engine is the Direct3D 12 + render-graph core
  described in `docs/Plan — PULSE engine and assets.txt`. Do not introduce a D3D11
  path, a CPU software rasterizer, a `waveOut` audio backend, or a per-frame
  GPU→CPU→GPU readback. The renderer is GPU-only; readback happens once, on capture.
- **Assets are authored from scratch** to the Part III contract (glTF `.glb` +
  BC7/BC5 PBR sets, tangents, mips). No prototype content is inherited. The
  first-person weapon, enemies, props, and environment are all produced via the asset
  pipeline and validated in-engine — not dropped in from the old tree.
- **Acceptance is in-engine.** An asset or a look change is "done" only when it reads
  correctly in the native renderer under the headless-capture + vision-critique loop.
  A generator preview or a Blender viewport does not get a vote.
- **Automated playtests and captures must exercise the same required game path** as
  development builds.

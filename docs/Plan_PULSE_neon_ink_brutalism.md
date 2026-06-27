# Implementation Plan: Neon Ink Brutalism

Plan date: 2026-06-21
Source art direction: docs/PULSE_art_style_decision.md (decision) refined by the
"Neon Ink Brutalism" art-direction bible.
Status: PLAN ONLY. No engine code or assets changed yet. D1 resolved (hybrid
wash); asset-regeneration workstream W9 (Meshy AI) added 2026-06-21.

## 0. How this relates to the existing decision

The committed decision doc picked "bold graphic stylization" (primary) with
"hard cel-shaded" (secondary). Neon Ink Brutalism is a sharper, fully specified
version of exactly that direction: graphic shading bands + ink outlines + a
locked neon palette (cyan = player, magenta = enemy, amber = navigation),
sitting on matte violet brutalist architecture with sparse polished obsidian and
ray-traced emissive light. It supersedes the decision doc as the canonical art
target; it does not contradict it.

Mix target: ~70% playable graphic novel, ~25% obsidian brutalism, ~5% low-poly
production discipline.

## 1. Engine reality (what we are building on)

Verified by reading the actual pipeline. Anchors are approximate file:line.

- Rendering is hybrid: a simple forward path (forward.hlsl) and the primary
  DEFERRED path (gbuffer.hlsl writes a 5-target G-buffer; resolve.hlsl computes
  lighting). All stylized-shading work lives in the deferred path.
- Direct lighting BRDF: resolve.hlsl brdf() around lines 79-97. Returns
  (diffuse + specular) * radiance * NoL. Sun direct around line 221, point
  lights around 236-244. Indirect/ambient is computed and added SEPARATELY
  (around 198-216), so direct can be stylized without touching indirect.
- G-buffer channels: RT0 albedo (RGBA8), RT1 world normal (R10G10B10A2),
  RT2 material R=AO G=rough B=metal A=flags (RGBA8; A currently uses only 1 bit
  for "isViewmodel", 7 bits free), RT3 emissive (R11G11B10F), RT4 velocity
  (RG16F = curUV - prevUV). Depth is reverse-Z float32. There is NO object/
  material ID channel yet.
- Post order (Engine.cpp around 914-1420): gbuffer -> [decal] -> cluster_cull ->
  fog -> (rt_trace + denoise | shadow/ssao) -> resolve -> [ssgi] -> [ssr] ->
  taa -> [particles] -> bloom(down,blurh,blurv) -> tonemap -> [ui]. No FXAA in
  the main path (TAA replaces it).
- Tonemap: AgX (tonemap.hlsl). Hand grade is hard-coded around lines 105-110
  (cool tint, ~1.18x sat, ~1.08x contrast). Exposure is STATIC (no auto-exposure)
  and set per-scene via SceneFrame PostParams.exposure. Bloom threshold ~1.0,
  knee ~0.6, intensity ~0.55 (hard-coded in Engine.cpp). Vignette/grain/sharpen
  exist and are subtle but hard-coded. Chromatic aberration exists in tonemap.
- Sky: procedural gradient in resolve.hlsl around 104-146 (horizon = clearColor
  lerped to a zenith, sun disc, 5-octave FBM clouds). Not a cubemap. Sky feeds
  ambient only as a single scalar (SunLight.ambient ~0.18).
- RT/GI: RT tier traces 1-bounce diffuse GI + emissive (emissive DOES spill into
  RT GI). Raster tier uses SSGI, which currently does NOT pick up emissive.
  Reflections are RT (RT tier) or SSR (raster), gated GLOBALLY per tier; there is
  NO per-material reflection flag. pt_reference.hlsl is the offline path tracer.
- Materials: MatEntry (Engine.hpp ~166-169): baseTex, factor, emissive, metallic,
  roughness, normal, orm, uvScale, metalScale, roughBoost. No string-keyed
  material library/registry. Wasteland.cpp hard-codes matte roughness ~0.96.
- Config: key=value (pulse.tuning, settings via Settings module) and
  pipe-delimited tables (pulse.content); pulse.weapons is INI-style. Config.cpp
  has trim/lower/parse helpers and a candidate-path loader; F5 hot-reload exists.
- Settings module (new, src/Game/Settings.*): audio + camera + comfort only.
  Persists to settings.cfg in %LOCALAPPDATA%\Pulse. NO graphics-quality presets.
  main.cpp parses --force-raster, --pathtrace/--pt-*, --gpu-info; no --quality.
- Enemies: CPU-skinned glTF rigs; emissive is one scalar applied to ALL submeshes
  of an enemy (buildFrame around 5798-5804). styleFor() assigns per-kind tints.
- Weapon viewmodels: warm/neutral tints, emissive fill tiered per weapon; muzzle/
  tracer colors from pulse.weapons. Audit found NO magenta on player effects
  (SMG muzzle is cyan, which is allowed). This already complies.
- HUD: GPU quad + glyph atlas, alpha-blended after tonemap. Palette in a "pal"
  namespace (textHi pale cyan-white, accent cyan, danger red-orange, warn amber,
  gold). No magenta. Corners partly used (boss bar top-center, ammo bottom-right).
- VFX/particles: WorldParticle carries color+emissive; particle.hlsl adds a
  white-hot core by luminance. CPU spawn is ALREADY player/enemy color-split via
  a "hostile" flag and enemyShotColor(); player effects are cyan/warm, enemy
  effects are per-kind (Ranged = magenta, others red/green/amber/orange).

## 2. Workstreams

Eight workstreams map the 16 doc sections to concrete engine changes. Each lists
goal, current state, the changes with anchors, effort (S/M/L), and risk.

### W1 - Stylized 3-region shading model  (doc 5)
Goal: quantize DIRECT diffuse into ~3 soft bands (deep shadow / midtone / lit),
keep SPECULAR smooth and continuous, leave INDIRECT smooth. Per-material toggle.
Current: pure PBR; no banding anywhere. Direct and indirect already separated.
Changes:
- resolve.hlsl brdf(): split the final "* NoL" so it multiplies the diffuse term
  only; build a quantize3(NoL, t0, t1, softness) using smoothstep for soft band
  edges; apply to diffuse, NOT to specular. Apply at sun (~221) and point
  (~236-244) calls.
- Add band thresholds + softness as a GLOBAL FrameCB block first (shared look),
  plus a 1-bit per-material "stylize on/off" in the free G-buffer material.a bits
  (gbuffer.hlsl writes it, resolve.hlsl reads it). Per-material threshold variance
  is a later refinement, not needed for the slice.
- Mirror the band logic onto the viewmodel shading branch (resolve.hlsl ~248)
  so the weapon reads as illustrated too.
Effort: M. Risk: M (must not quantize specular or indirect; viewmodel branch is
separate code). This is the single highest-impact change for the identity.

### W2 - Ink outline system  (doc 5)
Goal: dark blue-black outlines from depth + normal + ID discontinuities;
silhouette lines thicker than internal lines; stable (no crawl).
Current: no outline pass. Have world normals, reverse-Z depth, velocity. No ID.
Changes (phased):
- W2a screen-space outline post pass (template: the FXAA/TAA full-screen pass).
  Sample depth + RT1 normal, detect discontinuities, composite blue-black lines.
  Place AFTER taa, BEFORE tonemap (jitter-stable, no glow halo). Width scales
  with resolution; thinner internal vs silhouette via edge strength.
- W2b add an object/material ID channel: cheapest is a new G-buffer RT5 (R32_UINT)
  fed from a uint id added to InstanceData; enables ID-edge detection so an enemy
  separates from a same-depth wall. (Alternative: steal more material.a bits, but
  8 bits is too few for stable IDs.)
- W2c inverted-hull silhouette pass for flagged enemies/weapons (back-face
  expand along normal), drawn after gbuffer; gives thick, stable hero outlines
  that screen-space edges cannot match on thin geometry.
- W2d temporal stabilization: reproject prior-frame outline via RT4 velocity and
  blend (mirror taa.hlsl history + AABB clip), to kill crawl during fast motion.
Effort: L (a-d). Risk: M. W2a alone already delivers most of the look; b-d are
quality escalation.

### W3 - Palette-locked grading + post discipline  (doc 12, 2)
Goal: protect saturated cyan/magenta, keep environment restrained, no exposure
swings, restrained bloom that preserves source shape, mild sharpen, minimal
vignette/grain, no permanent DoF or chromatic aberration in gameplay.
Current: AgX + hard-coded grade; exposure already static (rule already met);
bloom threshold ~1.0 can blow shapes; CA present in tonemap.
Changes:
- tonemap.hlsl: insert a palette-protect grade AFTER AgX, BEFORE vignette. Pull
  cyan/magenta hues toward locked targets and protect their saturation while
  gently desaturating the rest. (LUT or hue-anchored selective sat.)
- Raise bloom threshold toward ~1.5-2.0 and/or tighten kernel so glowing sources
  keep their shape; expose threshold/knee/intensity in PostParams.
- Expose sharpen / vignette / grain in PostParams (keep subtle). Gate chromatic
  aberration OFF during gameplay (allow only for menus/cinematics/finishers).
- Document exposure as locked; optionally expose a runtime knob (no auto-adapt).
Effort: M. Risk: L. Mostly knob exposure + one grade function.

### W4 - Illustrated sky + atmosphere  (doc 7)
Goal: deep indigo zenith to peach/coral horizon, broad painterly cloud masses,
long horizontal streaks; sky drives warm-key/cool-fill ambient; blue-violet
distance fog that separates depth WITHOUT hiding enemies.
Current: gradient sky + FBM clouds already exist; ambient is a flat scalar; fog
exists (volumetric inject/integrate).
Changes:
- resolve.hlsl sky: add explicit skyZenith + skyHorizon colors to FrameCB; bias
  horizon warm (peach/coral), zenith deep indigo; art-direct FBM clouds toward
  fewer, larger masses with horizontal streaks (lower frequency, anisotropic).
- Hemisphere ambient: derive a warm key (from above-horizon sky) and a cool
  indigo fill (shadow side) from the two sky colors instead of one scalar, so the
  warm/cool composition from doc 6 is automatic per arena.
- Tune fog toward blue-violet, density set so distant geometry separates but
  combatants stay readable. Reserve cyan/magenta scatter near energy sources for
  later.
Effort: M. Risk: L.

### W5 - Master materials + locked style library  (doc 4, 13)
Goal: a small family of master materials (matte architectural 0.75-0.95;
painted metal 0.35-0.70; polished obsidian 0.06-0.18; emissive cyan/magenta) and
a LOCKED style library so AI-generated assets are converted to shared materials
and clamped to approved ranges.
Current: per-call createMaterial(); no registry; Wasteland hard-codes roughness.
Changes:
- Define master presets in a new config/pulse.style (reuse Config.cpp parser,
  F5 hot-reload): approved palette, roughness/emission ranges, outline widths,
  band thresholds, and a preset table (id | category | rough range | metal |
  emissive | flags).
- Add a MaterialLibrary registry: string id -> MaterialHandle; createMaterial
  clamps PBR values to the preset's locked ranges. AI assets reference a preset
  by name; out-of-range values are clamped, not honored.
- Add per-material MASK flags in the free G-buffer material.a bits: outline
  include/exclude, reflection importance, ink/hatch intensity, material category.
  (Emission/damage masks can come from existing emissive + later channels.)
- Repoint Wasteland.cpp and other content to the matte-architectural preset.
Effort: L. Risk: M (touches material creation across content). Foundational:
unblocks W1 flags, W2 outline flags, W6 reflection flags.

### W6 - RT selectivity + quality tiers  (doc 6, 14)
Goal: emissive GI bounce (cyan/magenta spill) and SELECTIVE RT reflections gated
by per-material importance; a 4-tier ladder where turning RT OFF preserves
silhouettes, outlines, palette, sky, and enemy cores.
Current: emissive feeds RT GI but NOT SSGI; reflections are global per tier; no
quality presets; only --force-raster.
Changes:
- Add reflectionMask to MatEntry + MaterialDesc; gate RT reflection rays
  (rt_trace.hlsl) and the reflection composite (resolve.hlsl ~205-208) so only
  obsidian/wet/weapon surfaces reflect. On raster, reflection importance picks
  SSR vs probe fallback.
- Feed emissive into SSGI (ssgi.hlsl samples the emissive G-buffer) so cyan/
  magenta bounce survives with RT off. This is the key "RT-off parity" fix.
- Extend Settings with a graphicsQuality enum (Ultra/High/Medium/Low) + rtGi /
  rtReflections / ssgi / ssr toggles; persist to settings.cfg; add --quality and
  per-feature flags in main.cpp; gate the passes in Engine recordGraph.
- Acceptance gate: capture the slice at Ultra and at Low; enemy/objective/outline
  readability and palette meaning must hold at Low.
Effort: L. Risk: M (cross-cutting; the RT-off parity test is the real bar).

### W7 - Enemy core + silhouette language  (doc 8)
Goal: charcoal/obsidian bodies, ONE bright magenta core, thin emissive cracks,
unique readable silhouettes, damage reveals brighter cracks + magenta burst.
Current: one emissive scalar per enemy across all submeshes; per-kind tints.
Changes:
- Per-submesh emissive override in buildFrame (~5798-5804): boost emissive +
  force magenta on "core"/"eye"/"chest" submeshes; keep body dark (charcoal tint
  via styleFor()).
- Damage state: raise crack emissive + emit a short magenta burst (VFX exists).
- Silhouette uniqueness per class is largely an ASSET/animation task (delegate to
  asset agents); flag here, not solved in-engine.
Effort: M (engine) + asset work. Risk: M (submesh naming must be reliable across
the bumstrum rigs; needs a per-rig core-submesh map).

### W8 - HUD + VFX lock  (doc 10, 11)
Goal: clean graphic-novel HUD in corners (pale-cyan text, cyan ammo/ability,
amber navigation, white critical flash, magenta danger); VFX locked to player =
cyan/white/amber, enemy = magenta/violet/white-hot, short-lived, shape-first.
Current: HUD palette exists (danger is red-orange, no magenta); VFX already
player/enemy split but enemy colors are per-kind.
Changes:
- Centralize all HUD colors in the "pal" namespace (kill one-off literals like the
  boss-bar color); add a corner-layout helper so combat center stays clear.
- VFX: lock the player branch to cyan/white/amber; ensure short lifetimes and
  strong initial shapes (already largely true).
Effort: S-M. Risk: L.

### W9 - Meshy asset regeneration pipeline  (doc 3, 13; depends on W5)
Goal: regenerate the asset library (enemies, weapons, environment kit, props)
from scratch via the Meshy AI REST API, conformed to the locked style. Meshy
provides FORMS / silhouettes; the ENGINE provides the look (W1 bands, W2 outlines,
W3 grade, W5 master materials). Raw Meshy textures are stripped, not shipped.

Grounded facts (verified 2026-06-21 against the live API + docs; key valid,
balance 3055 credits):
- Async REST: POST a task, poll (or SSE), download. Outputs glb/gltf (engine
  native) plus fbx/obj/usdz/stl. There is NO Meshy MCP tool, so drive it with a
  small reusable script under tools/meshy/ (curl or PowerShell).
- Text-to-3D is two-stage: preview (mesh only) then refine (texture). Pulse wants
  preview-only meshes for most assets (textures discarded). Controls:
  target_polycount (100-300k), topology quad/triangle, model_type=lowpoly,
  should_remesh, decimation_mode. NOTE: art_style is NOT supported by meshy-6; for
  stylized control use the prompt + polycount, or use meshy-5 with art_style.
- Rigging + animation: auto-rig 5 cr, animation 3 cr/clip. Bipedal HUMANOID only,
  needs a TEXTURED mesh, face toward +Z, < 300k faces. Quadrupeds / fantastical
  morphologies unsupported. Library has 600+ motions for idle/attack/etc.
- Per-task cost (meshy-6 / lowpoly tier): preview 20, refine 10; image-to-3D 20
  (no tex) / 30 (tex); retexture 10; remesh 5; rig 5; animation 3; convert 1;
  resize 1. meshy-5 preview is cheaper (~5). Failed tasks are refunded.

Budget (3055 credits, rough first full pass):
- 5 enemies: preview 20 + refine 10 (refine needed for rig quality) + rig 5 +
  ~3 anims x 3 ~= ~44 each => ~220.
- Environment modular kit (~18 pieces, preview-only): ~360.
- Hero props (monument, objective, cover, crates, barrels ~12): ~240.
- Static weapons (~7, hero detail, preview + refine): ~210.
- Re-roll overhead (text-to-3D often needs 2-3 tries): budget ~50% on top.
A full first pass fits comfortably in 3055 with headroom for iteration.

Key handling: the supplied key is a LIVE secret. Store it in a GITIGNORED file
(tools/meshy/key.txt) or env var MESHY_API_KEY; never commit it. Add the ignore
pattern BEFORE any generation.

Hard constraints / risks:
- R1 Enemy rig INTEGRATION is an ENGINE change, not just art. Today the engine
  loads one baked glTF clip and segments it into idle/walk/attack windows; Meshy
  emits a standard humanoid skeleton + separate library clips. Either teach the
  loader to consume multi-clip rigs, or bake Meshy clips into one segmented
  timeline matching the existing convention. Plan loader work.
- R2 FPS viewmodels (animated arms + weapon) are outside Meshy's strength.
  Recommend regenerating only the STATIC weapon hero meshes and keeping the
  existing arm / viewmodel animation rig (or do arms as a separate Blender task).
  Do NOT expect Meshy to produce animated FPS viewmodels.
- R3 Assets only look art-directed AFTER W5 (and ideally W1/W2). Generate geometry
  early if useful, but judge the look only once the engine pipeline exists ->
  slice asset set first, mass-regen after the look is proven.
- R4 Replacing tracked binary assets (glb/gltf/bin/obj) is large, effectively
  irreversible git churn. Keep originals until replacements are accepted; do NOT
  delete bumstrum/sketchfab sources mid-flight.
- R5 Scale/orientation differs per source (the bumstrum root-scale gotcha already
  bit viewmodels); every Meshy import needs a verified per-asset transform.
Effort: L (tooling S; generation iterative; enemy-loader integration M).
Risk: M-H (rig integration + iteration count are the real unknowns).

## 3. Open decisions (need your call; defaulted in the plan)

These are genuine art-direction choices, not engineering unknowns. The plan
assumes the first option unless you say otherwise.

D1. Enemy color coding. RESOLVED 2026-06-21: option (c) HYBRID WASH.
    The doc says magenta = ALL hostile energy. The game today color-codes enemy
    projectiles by KIND (Ranged magenta, Rusher amber, Stalker green, Tank red,
    boss orange). Options considered:
    (a) Unify to a magenta/violet family, distinguish kinds by SHAPE/silhouette/
        animation only (most on-brand, weakest at-a-glance kind ID).
    (b) Keep per-kind colors for fast threat-type reading (breaks the strict
        magenta=hostile rule but is arguably better gameplay).
    (c) [CHOSEN] Hybrid: magenta is the shared "hostile" tone for cores/auras and
        the enemy emissive language; projectiles keep a kind hue but always get a
        magenta/violet wash + a white-hot core. W7 (enemy core) and W8 (VFX lock)
        implement this; enemy core/aura = magenta, projectile = kind-hue-with-wash.

D2. HUD danger color. Doc says magenta for danger indicators; HUD uses red-orange.
    Default: move danger toward magenta to match the language, keep amber for
    warnings, reserve red strictly for critical-health (which doc allows as
    coral/white).

D3. Per-material band thresholds. Default: GLOBAL shared thresholds for the slice
    (cheaper, more consistent); add per-material variance only if a material needs
    it (e.g., obsidian wants fewer bands).

D4. Outline scope for the slice. Default: ship W2a (screen-space depth+normal) +
    W2b (ID channel) for the slice; treat W2c inverted-hull and W2d temporal
    stabilization as the polish pass right after.

## 4. Phased roadmap (prioritized to the doc-16 vertical slice)

Phase 0 - Foundation (unblocks everything, low risk)
  W5 config/pulse.style + MaterialLibrary + master presets + material mask flags;
  W8 HUD palette centralization; W3 expose PostParams knobs (no behavior change
  yet); W9 Meshy pipeline tooling (tools/meshy/ script + gitignored key).
  Deliverable: locked palette + presets in config (hot-reloadable) and a working
  one-command Meshy generate->poll->download script.

Asset track (W9, runs in PARALLEL with Phases 1-3)
  Generate the SLICE asset set first (a handful: see section 5) to prove the
  Meshy->W5 conversion and the look on real geometry. Only after the slice look is
  accepted, mass-regenerate the full library (enemies via the W9-R1 rig path,
  environment kit, props; static weapons; viewmodel arms handled per W9-R2). Keep
  original assets until each replacement is accepted (W9-R4).

Phase 1 - The look (this IS the graphic-novel identity)
  W1 3-band diffuse shading; W2a screen-space outlines; W3 palette grade + bloom
  restraint; W4 illustrated sky + warm/cool ambient. Deliverable: a frame that
  reads as a hand-inked neon graphic novel, RT on.

Phase 2 - Selectivity + RT-off parity + enemy core
  W2b ID channel (clean enemy-vs-wall edges); W6 per-material reflections +
  emissive->SSGI + quality tiers; W7 magenta enemy core. Deliverable: obsidian
  monument reflects selectively; the look survives RT off; enemy core reads.

Phase 3 - Polish
  W2c inverted-hull hero outlines + W2d temporal stabilization; W8 VFX lock + HUD
  corners; object-space ink/hatch in shadow bands (doc 5); fog energy scatter.

## 5. Vertical-slice target (doc 16) mapped to the work

Build ONE arena with: warm illustrated sunset sky (W4); matte violet brutalist
architecture (W5 + W1); one ramp + layered cover (existing Wasteland kit or a
small modular set); one polished obsidian monument or shallow reflective floor
(W5 obsidian preset + W6 selective reflection); one cyan floating objective (W8/
emissive + cyan light); one skeletal/fragmented enemy with a magenta chest core
(W7); one finished weapon with cyan strips (exists; W8 polish); RT emissive bounce
+ selective reflection (W6); graphic outlines + stable 3-band shading (W1 + W2);
basic HUD + combat VFX (W8).

Meshy-generated slice assets (W9): the brutalist modular kit (slab, ramp, wall,
monolith), the obsidian monument, the cyan objective prop, and the ONE skeletal
enemy (preview -> refine -> rig -> idle/walk/attack via the R1 path). The weapon
stays the existing viewmodel (R2). Everything binds to W5 master materials with
Meshy textures discarded -- this slice is also the proof that the Meshy->W5
conversion yields art-directed, not raw-generated, assets.

Acceptance questions (must all pass):
- Enemy instantly visible? (W1 bands + W2 outline + W7 core + negative space)
- Looks illustrated, not unfinished? (W1 + W2 + W3 + W4)
- RT enriches rather than overpowers the graphic style? (W6 one stable bounce)
- Cyan consistently = player/objective, magenta consistently = enemy?
  (W5 palette lock + W8 + D1)
- Reflections limited to meaningful surfaces? (W6 reflection mask)
- Still attractive with RT disabled? (W6 emissive->SSGI parity gate)
- Independently created assets look like one world? (W5 locked library)

## 6. Build order note

Everything above is config + shader + C++ in the existing deferred path; no
architectural rewrite. ASCII-only rule (CLAUDE.md) applies to every new shader,
source, config, and doc. The cheapest first proof is Phase 1 on the existing
Wasteland arena: W1 + W2a + W3 + W4 alone will show whether the identity lands
before any new content is built.

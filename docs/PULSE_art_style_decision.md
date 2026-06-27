# PULSE Art Style Review

Decision date: 2026-06-21

This review uses only the actual image names in `design_options`. Scores are out of 25:

- PULSE fit: fast first-person gun roguelite, dark techno combat, low enemy TTK.
- Readability: enemy, pickup, weapon, HUD, and silhouette clarity in motion.
- Custom engine feasibility: can look intentional before a full expensive renderer exists.
- Distinctiveness: avoids generic sci-fi shooter visuals.
- Production scalability: can be built repeatedly across rooms, enemies, weapons, and props.

## Recommendation

Choose **bold graphic stylization** as the primary art style.

It is the best balance of readable combat, distinct visual identity, and realistic custom-engine scope. It has clear enemy silhouettes, strong magenta/cyan gameplay accents, a dark industrial arena, simple enough materials, and a look that can be supported by direct lighting, fog, emissive bloom, flat/controlled shading, decals, and optional outlines.

Use **hard cel-shaded** as the secondary reference for clean form language and enemy/pickup readability. Do not use it as the primary mood because it is cleaner and less threatening than PULSE should feel.

## Ranked Shortlist

| Rank | Image | Score | Verdict |
|---:|---|---:|---|
| 1 | bold graphic stylization | 22 | Best primary choice. Dark, readable, stylized, feasible, and close to PULSE's FPS identity. |
| 2 | hard cel-shaded | 21 | Very strong readability and engine feasibility, but slightly too clean/safe as the main mood. |
| 3 | clean flat matte color fills with bold confident ink outlines on every form | 20 | Good engine-friendly variant, but the color temperature is warmer and less focused than the top pick. |
| 4 | comic-book  graphic-novel rendering | 18 | Distinct and readable, but the sketch/hatching density may get noisy in fast motion and is harder to render consistently in 3D. |
| 5 | bold low-poly | 18 | Easiest to build and very readable, but risks looking below the intended AA indie bar unless art direction is extremely disciplined. |
| 6 | PS1 lo-fi | 17 | Strong custom-engine practicality and mood, but commits the whole game to a retro identity. Good fallback, not the best AA target. |

## Full Review

| Image | Score | Notes |
|---|---:|---|
| bold graphic stylization | 22 | Best overall. Dark industrial space, strong silhouette, simple enough materials, clear magenta threat and cyan pickup language. |
| hard cel-shaded | 21 | Excellent clarity. Strong edge treatment and readable forms. Slightly too clean and bright for the desired pressure-cooker mood. |
| clean flat matte color fills with bold confident ink outlines on every form | 20 | Practical and readable. Better for production than realism. Needs darker palette and less warm environmental noise for PULSE. |
| comic-book  graphic-novel rendering | 18 | Memorable and stylish. Risk: ink strokes, hatching, and panel-like composition may become noisy during fast FPS movement. |
| bold low-poly | 18 | Very feasible in a custom engine. Risk: can look prototype-grade unless supported by excellent lighting, silhouette design, and color discipline. |
| PS1 lo-fi | 17 | Distinct, cheap, moody, readable. But this is a full retro commitment and conflicts with the current premium AA ambition. |
| 80s synthwave retro-future | 17 | Strong brand color, but too familiar and neon-arcade. Also leans on glossy reflections and bloom to work. Use only for accent ideas. |
| near-greyscale world of raw concrete and shadow | 17 | Great mood and simplicity. Weak as primary because it risks monochrome combat mud; needs stronger threat/pickup color hierarchy. |
| painterly indie | 16 | Attractive and atmospheric, but painterly assets are harder to make coherent across a 3D FPS pipeline without lots of hand-authored work. |
| grounded near-future | 15 | Serviceable but generic. Looks like many sci-fi shooters and depends on realistic material/lighting quality to stand out. |
| photorealistic PBR materials | 15 | Impressive still image but high-risk for a custom engine. It becomes a fidelity race and the visual identity is generic. |
| realistic proportions | 15 | Similar to grounded/photoreal, with better composition. Still too dependent on realism and not distinctive enough. |
| clean high-tech sci-fi | 14 | Readable and polished but sterile, white, and generic. Wrong mood for dark techno pressure. |
| bold flat-color neon | 14 | Easy to read and stylized, but too soft, toy-like, and abstract for PULSE's weapon/combat tone. |
| PlayStation-1 era lo-fi 3D | 14 | Clear retro identity, but less readable and less charming than PS1 lo-fi. Texture noise could fight combat clarity. |
| grounded cosmic-horror alien | 13 | Strong mood, but organic detail and wet alien surfaces are expensive and pull PULSE away from its industrial FPS identity. |
| dark fantasy | 11 | Good mood, wrong genre language. Gothic fantasy clashes with guns/techno unless the whole game pivots. |
| occult weird-west | 10 | Distinct, but it redirects the game into weird-west fantasy. Not aligned with PULSE's existing identity. |
| steampunk | 10 | Strong production identity, wrong game. Weapon/prop language would become brass/gear-heavy and not techno-clean. |
| ancient mythic | 9 | Clear and high energy, but it is fantasy/mythic rather than dark FPS techno. |
| stylish indie | 9 | Very readable, but too bright, clean, and arcade-like. Feels closer to a playful hero shooter than PULSE. |

## Decision

Primary reference: **bold graphic stylization**.

Secondary reference: **hard cel-shaded**.

Do not average all references into a vague hybrid. The actual target should be:

- Darker than `hard cel-shaded`.
- Less sketchy than `comic-book  graphic-novel rendering`.
- Less retro than `PS1 lo-fi`.
- Less generic and less renderer-dependent than `grounded near-future` or `photorealistic PBR materials`.

## Implementation Direction

- Build around matte industrial surfaces, broad shadow shapes, emissive gameplay colors, and readable outlines/rims.
- Prioritize enemy silhouettes, magenta threat cores, cyan interactables, and high-value separation.
- Use fog and bloom sparingly to support depth and mood, not to hide weak asset work.
- Avoid glossy realism as the visual foundation. PULSE should look art-directed before it looks expensive.

## Enemy Attack VFX Color Semantics

Enemy attack colors must communicate behavior:

- Magenta: standard aimed orb, the baseline dodge projectile.
- Orange: heavy fan, nova, and zoning shots; dodge through a gap or leave the area.
- Violet: rapid burst pressure and spiral patterns; keep moving laterally.
- Crimson: lock-on beam; step off the line before it fires.
- Boss colors: Warden uses volt-violet, Smelter uses pyro-orange, Choir uses cryo-violet.

Cyan remains reserved for player, friendly, and interactable tech.

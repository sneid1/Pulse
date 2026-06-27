# Quaternius "Modular SciFi MegaKit" - Asset Catalog (for room design)

CC0 kit under `assets/quaternius/Modular SciFi MegaKit[Pro]/glTF/`. Everything is on a **4 m grid**
(1 cell = 4 m), Y-up, metric. The room assembler builds arenas by snapping these pieces on that
grid. This catalog lists every building piece grouped by its **design role**, with dimensions
(W x H x D, metres) and a one-line description, so a room designer knows the full palette.

Reference contact sheets (rendered thumbnails, one per category):
`build/kit_sheet_Walls.png`, `kit_sheet_Platforms.png`, `kit_sheet_Columns.png`,
`kit_sheet_Props.png`, `kit_sheet_Decals.png`.

Convention notes:
- **Wall panels** run along their local Z (4 m long), are ~3 m tall, and the panel sits on the
  cell's edge (thin depth in X). Each wall FAMILY ships: `_Straight` (+ variants) and corner pieces
  `_Corner_Square_Inner/Outer` and `_Corner_Round_Inner/Outer`. Inner = concave (room interior
  corner); Outer = convex; Round = chamfered/curved corner; Square = right-angle.
- **Tops** sit at y=3..5 (cap above a 3 m wall). **Bottoms** are thin floor-line trims (y~0).
- A full finished wall = Bottom (optional) + Wall panel + Top.

================================================================================
## FLOOR TILES  (Platforms, flat 4x4, lay one per cell)
================================================================================
All are 4 x 0 x 4 (flat, y=0) unless noted. Pick one as a room's base floor; mix for zoning.
- Platform_Simple / Platform_Simple2        - plain panel floor (default, clean)
- Platform_Metal / Platform_Metal2          - metal plate floor
- Platform_DarkPlates                       - dark riveted plates (moody)
- Platform_WidePlates                       - large plates (calmer, big rooms)
- Platform_Squares                          - small square grid (busy, tech)
- Platform_3Plates / Platform_CenterPlate   - plated variants with a centre motif
- Platform_RedAccent                        - floor with a red accent stripe (zoning/hazard)
- Platform_X                                - X-pattern plate
- Platform_Padded (4x0.11x4)                - slightly raised padded floor
- *_Curve variants (CenterPlate/DarkPlates/Metal/Metal2/RedAccent/Simple/Simple2/Squares/WidePlates)
                                            - same tiles with a curved/rounded edge motif (room edges)
- Platform_Round1 / Platform_Round2 (6x~0.15x6) - large round dais (great as a focal arena centre)
- Platform_Window_Thin (2.88x0.41x4) / Platform_Window_Wide (4x0.41x4)
                                            - glass floor panels (catwalk-over-void look)

================================================================================
## WALL PANELS  (full-height ~3 m; perimeter + interior partitions)
================================================================================
Each family = `_Straight` + corners (Square/Round, Inner/Outer). Depth in parentheses.
- WallAstra        (1.21 deep) - hero wall: trim + red decal + cabling. Variants: `_Broken`,
                     `_Divided`, `_Flat` (0.1 thin), `_Flat_Window`, `_Window` (glass slit).
- WallPadded       (0.43 deep) - clean padded panels (calm, default-friendly). Curve corners only.
- WallBand         (0.21 deep) - panel with a horizontal band/stripe. Variant `_Broken`.
- WallWideBand     (0.11 deep) - thin panel, wide band. Variants `_TallBand`, `_Transition`,
                     `_Transition2` (height-step transitions between sections).
- WallPipe         (0.33 deep) - panel with surface piping (industrial).
- WallWindow       (0.32 deep, 3.19 tall) - large window panel (glass; sightline/skybox feel).
                     Caps: WallWindow_Cap_A / _B (small window end caps).

WALL TOPS  (cap a 3 m wall; sit y=3..5, 2 m tall):
- TopSimple, TopPlates, TopPlastic, TopAstra, TopPadded_Flat, TopWindow - flat caps (+ corners).
- TopCables / TopCables_Straight_Hanging - cap with cable runs; Hanging drops cables to the floor.
- TopPadded_Incline / TopAstra_Curve - sloped/curved caps (height transitions, ceilings).

WALL BOTTOMS  (thin floor-line skirting, y~0, 0.6 deep):
- BottomSimple, BottomMetal, BottomAccent - each Straight + Square/Round Inner/Outer corners.

================================================================================
## LOW COVER / RAILINGS  (ShortWall; waist-to-chest height cover lines)
================================================================================
4 m long, run along an edge; `_Straight` + `_Corner_Inner/Outer`. Height in parentheses.
- ShortWall_Simple1 / Simple2 / MetalPlates / Triangles / DarkPlastic / AccentStrip   (~1.0 m)
                                            - low cover (shoot OVER while standing; vault feel)
- ShortWall_Metal2 / DarkMetal2 / Band2 / WhitePlate2                                  (~2.0 m)
                                            - chest/shoulder cover (crouch-safe; stronger cover line)

================================================================================
## COLUMNS / PILLARS  (vertical structure + hard cover + sightline breaks)
================================================================================
- Column_Simple (0.5x5x1)   - slim support pillar
- Column_Round (1x5x1)      - round pillar (good corner/feature column)
- Column_Dark / Column_SimpleSquare / Column_BentSquare (1x5x1) - square pillars (dark/plain/kinked)
- Column_Hollow (1.2x5x1.2) - hollow square pillar (chunkier)
- Column_Pipes (0.87x5x0.87)- pillar bundled with pipes
- Column_Astra (0.33x3x1.21)- short wall-mounted buttress (decor, not full height)
- Column_Large2 / Large3 (1.1-1.5 x 10 x 5+) - MASSIVE structural columns (double height; landmark)
- Column_Large_Straight (1.4x10x2)          - tall straight mega-column
- Column_MetalSupport (0.96x0.04x4) / _Curve - flat horizontal ceiling/beam support (overhead)
- Column_MetalSupports (0.87x5x0.04)        - thin vertical support strut
- Column_SmallSupport (2x0.11x0.02)         - small bracket

================================================================================
## VERTICALITY  (ramps, stairs, raised + railed platforms)
================================================================================
Ramps/stairs RISE 1-3 m; place against a raised platform to reach it.
- Platform_Ramp_2 / Ramp_2Short (rise ~1 m, 2 m run) - short ramp
- Platform_Ramp_4 (rise 2 m) / Ramp_4Wide (4 m wide, rise 2 m) - full ramp to one storey
- Platform_Ramp2_* - alternate ramp styling (same sizes)
- Platform_Stairs_2 (rise 1 m) / Stairs_4 / Stairs_4Wide (rise 2 m) / Stairs_4WideTall (rise 3 m)
- Platform_Rails_2 / Rails_4 / Rails_4Wide / Rails_4WideTall
                                            - raised railed platform decks (highground + railing cover;
                                              Tall = ~3.7 m deck). Use as the top of a level.

================================================================================
## DOORS  (frame + sliding panel; one opening per door cell)
================================================================================
Frames (~4.85 wide x 5 tall, frame a 4 m opening):
- Door_Frame_Square / Door_Frame_Square_Symmetric - standard square door frame
- Door_Frame_SquareTall                          - taller frame
- Door_Frame_A                                    - heavier industrial frame (with side/middle parts)
- Door_Frame_Square_Blocked                       - frame with a SEALED panel (a dead/locked door)
- *_Middle / *_Side                               - sub-parts for wider/custom openings
Sliding panels (2.11 wide x 4 tall, slide inside a frame):
- Door_Metal (engine default leaf) / Door_Simple / Door_DarkMetal

================================================================================
## FOCAL / LARGE PROPS  (room centrepieces + landmarks)
================================================================================
- Prop_Pod (2.1x4.1x2.1) / Prop_Pod_Short (2.1x3.4x2.1) - cryo/stasis pod (great hero centrepiece)
- Prop_Fan_Big (3.25x0.5x3) / Prop_Fan_Small (1.7x0.4x1.7) - ceiling/floor fan (vent shaft motif)
- Prop_Teleporter (4.4x0.8x4.4)            - floor teleporter pad (objective/exit landmark)
- Prop_PipeHolder (4.1x0.9x1.2)            - big wall pipe assembly (industrial wall feature)
- Prop_Chest (1.5x0.7x0.75)                - loot chest (reward landmark)
- Prop_ItemHolder (0.82x0.44x0.73)         - pedestal/holder (place a reward on)
(Also the Column_Large* mega-columns above read as landmarks.)

================================================================================
## COVER PROPS  (movable-looking hard cover; ~1 m, jump-on / shoot-over)
================================================================================
- Prop_Crate1 / Crate2 / Crate3 (1x1x1) / Crate4 (1.1 cube) - crates (stack/cluster for cover)
- Prop_Barrel_Large (0.5x1.1) / Prop_Barrel_Small (0.4x0.8) - barrels (scatter accents / cover)

================================================================================
## DRESSING PROPS  (detail; wall + floor clutter, mostly low/no collision)
================================================================================
- Prop_Computer (0.74x1.6x0.56)            - console/terminal (wall-side detail)
- Prop_AccessPoint / _Short (wall-mounted, y~1.1) - wall control panel
- Prop_Cable_1..4 (floor cable runs, up to ~5.7 long) - cables snaking across floor/under walls
- Prop_Pipe_Small/Medium/Thick _Straight/_Curve (4 m runs) - pipe runs (wall + ceiling industrial)
- Prop_Pipe_Capped (0.27x0.27x4)           - capped pipe end
- Prop_Vent_Big / _Small / _Wide (flat, ~2 m) - floor/wall vent grilles
- Prop_Light_Floor / _Small / _Wide / _Corner (flat emissive) - light fixtures (pools of light)
- Prop_Rail_2/3/4 (0.86 tall) / Rail_Round_Big/Small / Rail_Incline_* - standalone railings
- Prop_Clamp / Prop_WallBand_BrokenPlate   - small mechanical detail / damaged-panel accent

================================================================================
## DECALS  (flat floor/wall markings, y=0 - paint, do not block)
================================================================================
- Numbers Decal_0..9, letters Decal_A/K/V/X/Z, Decal_XSign
- Hazard/sign: Decal_Caution, Decal_Warning, Decal_Authorized, Decal_Arrows, Decal_Dashes,
  Decal_Open, Decal_Unlock, Decal_Sign, Decal_Code, Decal_Code_2, Decal_STRNOV
- Branding: Decal_Logo, Decal_Logo_Letters, Decal_Logo_Small, Decal_AccessPoint
- Floor guide lines: Decal_Line_Straight, Decal_Line_90 (+ _Round, _Round_Large),
  Decal_Line_Bend1/2_L/R  - paint walkway/lane lines on the floor (readability + life)

Use decals to letter rooms/doors, paint hazard zones around the focal/objective, and run guide
lines along the main combat lanes - cheap, high-impact "this place is used" detail.

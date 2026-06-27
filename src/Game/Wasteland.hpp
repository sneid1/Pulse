#pragma once

// Procedural OUTDOOR arenas, themed by BIOME. Loads a library of CC-BY DJMaesen kits
// (medieval-sceneray + the sketchfab rock/tree/grass/cliff sets) as categorised prop pools -
// rocks, small rocks, crates, trees, grass, structures - each kit auto-scaled to a sensible
// world size regardless of its authored units, then scatters them onto an open bounded ground
// to build a varied combat arena per run-room. A Biome picks which pools + ground material +
// scatter densities to use, so the run can travel through visually distinct outdoor rooms
// (rocky canyon, forest, ...). Emits the same draw list + collision grid + spawn the rest of
// the game speaks (mirrors Dungeon's interface), so it drops in as the environment.

#include "Engine/Engine.hpp"
#include "Engine/Core/Mat.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulse {

// One renderable piece of a placed tile/prop: shared GPU mesh + material + per-instance
// world transform. (Formerly in Dungeon.hpp; the dungeon mode was removed.)
struct DungeonDraw {
    MeshHandle     mesh{};
    MaterialHandle material{};
    Mat4           transform = Mat4::identity();
};

enum class Biome { Rocky, Forest, Ruins, Count };

// M4: brutalist biome identities (the run travels one biome per sector). Names + the element
// each biome's loot leans toward (ties the place to the M2/M3 status build the room rewards).
//   Rocky  -> "Foundry"   (cyan/magenta industrial; Volt-leaning loot)
//   Forest -> "Furnace"   (amber/red smelting hall;  Pyro-leaning loot)
//   Ruins  -> "Reliquary" (violet/green eerie vault;  Cryo-leaning loot)
const char* biomeName(Biome b);
int biomeRewardElement(Biome b);   // 0 none, 1 Burn, 2 Shock, 3 Cryo, 4 Corrode (maps to Status Element)

// A curated brutalist layout's footprint. The arena is no longer one fixed room: the run
// strings together areas of different SIZES, picked per room by pacing (see PulseGame). Each
// size maps to a hand-authored layout template (open-rect bounds + cover + door openings).
enum class AreaSize { Small, Mid, Big, Corridor, Count };

// Which perimeter wall a door sits in. Inward normal derives from the side: N is the low-z
// wall (inward +Z), S the high-z wall (inward -Z), W the low-x wall (inward +X), E the high-x.
enum class Side { N, E, S, W };

// One door opening in an area's perimeter wall. The environment owns the geometry + collision;
// PulseGame reads these to place per-door frame draws, trigger volumes, and the entrance spawn.
// doors()[0] is always the ENTRANCE (where the player lands on arrival, stays sealed); the rest
// are EXITS that open on clear and bind to the run's next-room options.
struct Door {
    Side  side = Side::N;
    float worldX = 0.0f, worldZ = 0.0f;   // center of the opening (world units) = trigger center
    float inwardX = 0.0f, inwardZ = 0.0f; // unit inward normal (into the room)
    int   spawnX = 16, spawnZ = 12;       // grid cell just inside (player landing on entry)
    bool  open = false;                   // runtime seal/open state (false = sealed during combat)
    int   fgx0 = 0, fgz0 = 0, fgx1 = 0, fgz1 = 0;  // fine-cell rect of the opening (for re-stamp)
};

class Wasteland {
public:
    // Load the whole kit library + per-biome ground materials (self-loaded: the textures are
    // read straight off disk via the engine). Returns false if the core kits are missing.
    bool load(Engine& engine);

    // Build an arena for the given biome + run-room index (deterministic from seed+index):
    // fills grid_, fine_ and draws_. Safe to call repeatedly.
    void generate(Biome biome, uint64_t seed, int roomIndex);

    // Neon Ink Brutalism slice: when set BEFORE load(), the arena becomes a flat-matte
    // BLOCKY brutalist layout (beveled boxes bound to the W5 matte/obsidian palette)
    // instead of the realistic rocky kit. Reuses the same draw list + collision grid +
    // spawn, so combat/movement are unchanged. See docs/Plan_PULSE_neon_ink_brutalism.md.
    void setBrutalist(bool b) { brutalist_ = b; }
    // Dev/QA only: omit the sealed ceiling + the wall->ceiling filler band + overhead emissive trim
    // so the near-overhead --topdown inspection camera can see into the room. Off in normal play
    // (rooms are fully enclosed interiors). Does not affect layout/collision/lighting rig positions.
    void setOpenTop(bool b) { openTop_ = b; }

    // Pick which curated layout the NEXT generate() builds (brutalist path only). Set by the
    // run pacing before generate (boss -> Big, opener -> Small, connector -> Corridor, ...).
    void setAreaSize(AreaSize s) { pendingSize_ = s; }
    // Current sector index. pickRoomTemplate avoids repeating a layout within one sector, so a run
    // surfaces more of each biome's 10 distinct rooms; the used-set resets when the sector changes.
    void setSector(int s) { pendingSector_ = s; }
    // Dev/QA: force the NEXT generate() to build a specific named template (case-insensitive),
    // overriding the size picker and adopting the template's own biome. Empty = normal pick.
    // Wired to the --room <name> capture flag so any of the 30 rooms can be inspected headlessly.
    void setForcedTemplate(const std::string& name) { forcedTemplateName_ = name; }

    bool ready() const { return ready_; }
    const std::array<std::string, 24>& grid() const { return grid_; }
    const std::vector<DungeonDraw>&     draws() const { return draws_; }

    // The current area's doors (brutalist path). [0] is the entrance; [1..] are exits. PulseGame
    // binds exits to run options on clear, draws frames, and tests the trigger volumes.
    const std::vector<Door>& doors() const { return doors_; }
    int  doorCount() const { return static_cast<int>(doors_.size()); }
    // Shared unit cube (0..0 to 1..1) + the matte palette, so PulseGame can draw the sealed-door
    // force-field slabs in the door gaps using the same brutalist look.
    MeshHandle     boxMesh() const { return unitBoxMesh_; }
    // M4: the brutalist palette is now PER BIOME (Foundry/Furnace/Reliquary), selected by the
    // active biome, so each sector reads as a distinct place, not the one fixed concrete/cyan look.
    MaterialHandle brutalMaterial(int i) const {
        const int b = static_cast<int>(activeBiome_);
        return brutalMat_[b][i < 0 ? 0 : (i >= kMat ? kMat - 1 : i)];
    }
    Biome activeBiome() const { return activeBiome_; }
    // The sci-fi sliding door panels (real-asset kit), drawn game-side at SEALED doors only so a
    // doorway shows a closed door in combat and a clear opening when it unlocks.
    const std::vector<DungeonDraw>& envDoorParts() const { return envDoor_.parts; }
    // Quaternius door LEAF (a single kit door panel, native ~2.1 m wide x 4 m tall, in its XY plane
    // facing +-Z). PulseGame places two of these per opening, scaled to half the gap, sliding apart
    // by the animation phase - so the doors are the same Quaternius family as the walls. The leaf is
    // PER BIOME (spec biome.doorPiece): Foundry Door_Metal, Furnace Door_DarkMetal, Reliquary Door_Simple.
    const std::vector<DungeonDraw>& doorLeafParts() const { return activeDoorLeaf().parts; }
    float doorLeafWidth()  const { return activeDoorLeaf().sizeX; }
    float doorLeafHeight() const { return activeDoorLeaf().sizeY; }
    // The sci-fi corridor segment (real-asset kit) drawn game-side behind OPEN doors, so a cleared
    // exit shows a real connecting passage. spanZ is its length along its local travel axis (Z).
    const std::vector<DungeonDraw>& hallwayParts() const { return hallway_.parts; }
    float hallwaySpan() const { return hallway_.halfZ * 2.0f; }
    // The current area's footprint, so decorative focal props sit centred and enemy spawning can
    // sample the real open area regardless of size (the authored spawn ring assumes a big arena).
    float    centerX() const { return centerX_; }
    float    centerZ() const { return centerZ_; }
    float    halfExtentX() const { return halfX_; }
    float    halfExtentZ() const { return halfZ_; }
    AreaSize areaSize() const { return lastSize_; }
    // Interior ceiling height (metres) of the room just built - the game hangs its biome light rig
    // (overhead strips / shaft sources) at this height. Rooms are enclosed, so the sun is occluded.
    float    ceiling() const { return ceilingHeightFor(activeBiome_); }
    // Toggle a single door's collision (open -> walkable gap; sealed -> solid wall). The visual
    // (closed slab vs glow frame) is drawn by PulseGame from doors()[i].open.
    void setDoorOpen(int i, bool open);
    void sealDoors();   // all doors -> solid (combat start)
    void openDoors();   // all doors -> walkable

    static constexpr int kSub = 4;
    // Brutalist master-material slots: 0 violet, 1 light concrete, 2 obsidian, 3 neon cyan
    // (emissive), 4 neon magenta (emissive), 5 dark metal. One baked submesh per slot.
    static constexpr int kMat = 6;
    int  subRes() const { return kSub; }
    bool solidFineCell(int fx, int fz) const {
        if (fx < 0 || fz < 0 || fx >= 32 * kSub || fz >= 24 * kSub) return true;
        return fine_[static_cast<size_t>(fz) * (32 * kSub) + static_cast<size_t>(fx)] != 0;
    }
    // World-space top height of the solid cover at this fine cell (0 if open). Lets the
    // player jump over / stand on low cover while tall rock/cliffs/boundary stay unscalable.
    float solidFineHeight(int fx, int fz) const {
        if (fx < 0 || fz < 0 || fx >= 32 * kSub || fz >= 24 * kSub) return 99.0f; // boundary: unscalable
        return fineHeight_[static_cast<size_t>(fz) * (32 * kSub) + static_cast<size_t>(fx)];
    }

    int spawnX() const { return spawnX_; }
    int spawnZ() const { return spawnZ_; }

private:
    // A loaded prop: its submeshes (mesh + material) re-centred on its footprint, plus the
    // collision footprint radius (world units; 0 = walk-through decoration) and its size.
    struct Prop {
        std::string              name;
        std::vector<DungeonDraw> parts;        // transform set per placed instance
        float                    radius = 0.0f;   // collision footprint (0 = no collision)
        float                    halfX = 0.0f, halfZ = 0.0f, height = 0.0f;
    };

    enum class Cat { Rock, SmallRock, Crate, Tree, Grass, Structure };
    enum class Mode { Pieces, Merged };   // Pieces: each named tile is a prop; Merged: whole kit is one prop

    void place(std::vector<DungeonDraw>& out, const Prop& p, float wx, float wz, float yaw, float scale);
    void stampSolidCircle(float wx, float wz, float radius, float height);
    void stampSolidRect(float x0, float z0, float x1, float z1, float height);
    // Stamp a WALKABLE linear ramp in [x0,x1]x[z0,z1]: the collision top height rises linearly from
    // hLow to hHigh along the ascent direction so the player walks up onto a raised deck (instead of
    // hitting a flat box). ascend: 0 = +x, 1 = -x, 2 = +z, 3 = -z (toward hHigh / the deck).
    void stampRamp(float x0, float z0, float x1, float z1, float hLow, float hHigh, int ascend);

    // Neon Ink Brutalism slice (W5/asset): a flat-matte BLOCKY arena, now a CURATED SET of
    // layout templates at different sizes (see AreaSize). loadBrutalist bakes one merged mesh
    // triple (per master-material) PER TEMPLATE once; generateBrutalist selects the template
    // for pendingSize_, carves its open-rect + door channels into the collision grid, and emits
    // the baked draws. Doors start sealed; PulseGame opens the exits on clear.
    bool loadBrutalist(Engine& engine);
    void generateBrutalist(AreaSize size, uint64_t seed, int roomIndex);
    bool                        brutalist_ = false;
    bool                        openTop_ = false;   // dev/QA: --topdown omits the ceiling so the camera sees in
    int                         pendingSector_ = -1;       // current sector (drives no-repeat room pick)
    mutable int                 pickSectorTag_ = -2;       // sector the used-set below belongs to
    mutable std::vector<std::string> pickUsed_;            // template names already used this sector
    AreaSize                    pendingSize_ = AreaSize::Mid;
    AreaSize                    lastSize_ = AreaSize::Mid;   // size built by the last generate
    Biome                       pendingBiome_ = Biome::Rocky;  // M4: biome for the NEXT generate (set in generate())
    Biome                       activeBiome_ = Biome::Rocky;   // M4: biome the last generate built (drives the palette)
    // M4: per-biome brutalist palette - kMat material slots for each Biome.
    std::array<std::array<MaterialHandle, kMat>, static_cast<size_t>(Biome::Count)> brutalMat_{};
    // Baked merged mesh per material slot, per template (indexed by AreaSize).
    std::array<std::array<MeshHandle, kMat>, static_cast<size_t>(AreaSize::Count)> templateMesh_{};
    // A real-asset glTF prop loaded with its AUTHORED PBR materials (textures + factors), as a set
    // of submesh draws centred at origin + scaled. Reused for the Sketchfab sci-fi kit pieces.
    struct KitProp {
        std::vector<DungeonDraw> parts;     // each: mesh + engine material (transform set per place)
        float halfX = 0.0f, halfZ = 0.0f, height = 0.0f;
        std::string source;                 // source GLB path, used for round-aware placement/audit
    };
    struct MeshyBiomeProps {
        std::vector<KitProp> focals;   // replaces generic kit focal on X cells
        std::vector<KitProp> anchors;  // non-colliding pocket/prop dressing
        std::vector<KitProp> floorDetails;
        std::vector<KitProp> wallDetails;
        std::vector<KitProp> baseAnchors;
        std::vector<KitProp> baseFloorDetails;
        std::vector<KitProp> baseWallDetails;
    };
    struct MeshyCommonProps {
        KitProp doorSide, doorLintel, doorThreshold;
        KitProp wallSeam, wallAlcove, baseTrim;
        KitProp ceilingDuct, ceilingSpine;
        KitProp deckSupport, stairFinisher, floorHatch;
        std::vector<KitProp> ceilingDucts, stairFinishers;
    };
    KitProp       loadKitProp(Engine& engine, const std::string& gltfPath, float targetSize);
    void          loadMeshyEnvironmentProps(Engine& engine);
    void          placeKitProp(std::vector<DungeonDraw>& out, const KitProp& p,
                               float wx, float yOff, float wz, float yaw, float uniformScale = 1.0f) const;
    TextureHandle loadKitTexture(Engine& engine, const std::string& file, bool srgb);

    // Real-asset sci-fi modular TILESET (CC-BY hamraj15 "Sci-Fi Game Tileset"): one shared
    // atlas material (baseColor + normal + ORM + emissive) applied to a few extracted tile
    // meshes (floor / plain wall / cyan-trace wall), tiled to build the room's WALL + FLOOR
    // shell - the real-geometry replacement for the procedural wall/floor boxes. Collision
    // stays the grid (the tiles are visual; the perimeter solid is unchanged).
    struct TileMesh { MeshHandle mesh{}; float w = 0.0f, h = 0.0f, d = 0.0f; };  // world dims (scaled)
    bool      loadTileset(Engine& engine);
    TileMesh  loadTileMesh(Engine& engine, const std::string& gltfPath, float scale);
    // Tile the floor (grid-fit) + perimeter walls (segment-fit, skipping door gaps) into draws_.
    void      emitTiledShell(float ox0, float oz0, float ox1, float oz1);
    MaterialHandle tilesetMat_{};
    MeshHandle floorTileMesh_{}, wallTileMesh_{}, wallTracedMesh_{};
    float      floorTileW_ = 3.0f, floorTileD_ = 3.0f;   // floor tile world footprint
    float      wallTileW_ = 3.0f, wallTileH_ = 4.5f;     // wall panel world width + height
    bool       tilesetReady_ = false;

    // Quaternius "Modular SciFi MegaKit" (CC0) modular ASSEMBLER. The whole arena shell is built
    // from this one kit family - floor platforms, wall panels (+ tops, columns), door frames and
    // cover props - on the kit's native 4 m grid, so the world reads as one coherent family. All
    // pieces share a small atlas material set (Trim_01/02/03, PaddedWall...), cached by texture set.
    // generateQuaternius assembles the room (collision + draws) per AreaSize; preferred over the
    // procedural/tiled brutalist shell whenever the kit loads. See generateQuaternius.
    struct QuatPiece {
        std::vector<DungeonDraw> parts;          // submesh draws (mesh + shared material), placed by transform
        float sizeX = 0.0f, sizeY = 0.0f, sizeZ = 0.0f;   // world bbox extent (kit is metric: 1 unit = 1 m)
    };
    static constexpr float kQuatCell = 4.0f;     // the kit's modular grid (metres)
    bool          loadQuaternius(Engine& engine);
    QuatPiece     loadQuatPiece(Engine& engine, const std::string& gltfPath);
    MaterialHandle quatMaterial(Engine& engine, const std::string& dir, const struct TileSubmesh& sm);
    // Place a piece at world (wx,wz)+yOff with yaw, pushing its submesh draws into out.
    void          placeQuat(std::vector<DungeonDraw>& out, const QuatPiece& p, float wx, float yOff, float wz, float yaw);
    void          generateQuaternius(AreaSize size, uint64_t seed, int roomIndex);

    // DATA-DRIVEN HAND-CRAFTED ROOMS. Designed rooms live in config/pulse.rooms as ASCII-grid
    // templates (the same format the design-agent produces: NAME/SIZE/FAMILY/.../GRID with
    // pipe-delimited rows). loadRoomTemplates parses them; loadQuaternius pre-loads every kit piece
    // a template names (by name, any of the 272). generate() picks a template for the AreaSize and
    // assembleQuaterniusRoom builds it (floor + auto-walls on the footprint + doors + cover/pillars/
    // focal/props + collision). No template for a size -> generateQuaterniusProcedural fallback. So
    // the design agent's output drops into config/pulse.rooms with no recompile (just a relaunch).
    struct RoomTemplate {
        std::string name;
        AreaSize    size = AreaSize::Mid;
        Biome       biome = Biome::Count;   // Count = unspecified (matches any sector); else routed to that biome
        std::string family, floor, top, cover, pillar, focal;   // chosen kit pieces (empty = default)
        std::string daisFloor;                                  // floor piece for raised 'H' decks (empty = floor)
        std::string ramp;                                       // room/biome ramp or stair piece
        std::string doorFrame, doorLeaf;                         // room/biome door kit binding
        std::string cratePiece;                                  // biome crate for 'c'
        std::vector<std::string> dressingPool;                   // biome detail/anchor props
        std::vector<std::string> decalGroup;                     // biome decal kit
        std::vector<std::string> grid;                          // rows; 1 char per 4 m cell; ' ' = void
    };
    bool             loadRoomTemplates(const std::string& path);
    // Step-9 import validation (the buildContract): "" = ok, else a human-readable reason. Checks
    // exactly one 'E', the size's door count, equal-width rows, and that every walkable cell is
    // reachable from 'E' (void/#/o/X block; crates/low-walls/props/ramps + H-via-ramp pass).
    std::string      validateRoomTemplate(const RoomTemplate& t) const;
    // Pick a template for this sector: prefer ones tagged for (biome AND size); fall back to
    // biome-agnostic ones of the size; nullptr -> the procedural fallback.
    const RoomTemplate* pickRoomTemplate(AreaSize size, Biome biome, uint64_t seed, int roomIndex) const;
    void             scanQuatAssets();                                           // filename registry
    std::string      resolveQuatAssetName(const std::string& name) const;        // bare/stem -> real asset name
    const QuatPiece* quatPieceByName(Engine& engine, const std::string& name);   // load+cache (load time)
    const QuatPiece* cachedPiece(const std::string& name) const;                 // lookup (assemble time)
    // Resolve a header piece name: try the bare name first (e.g. "WallAstra_Straight_Broken",
    // "Platform_DarkPlates"), then "<name>_Straight" (e.g. family "WallPipe", top "TopAstra",
    // cover "ShortWall_Band2"). nullptr if neither is cached. Lets headers name either form.
    const QuatPiece* resolveCached(const std::string& name) const;
    void             assembleQuaterniusRoom(const RoomTemplate& t, uint64_t seed, int roomIndex);
    void             generateQuaterniusProcedural(AreaSize size, uint64_t seed, int roomIndex);  // fallback
    struct QuatAssetRef {
        std::string root;   // kit glTF root
        std::string rel;    // path below root
    };
    std::string                 quatBase_;        // MegaKit glTF dir (primary shell kit)
    std::unordered_map<std::string, QuatAssetRef> quatAssetRelPath_;  // asset key -> kit root + relative path
    std::string                 forcedTemplateName_;  // dev/QA: --room forces this named template
    std::vector<RoomTemplate>   roomTemplates_;
    std::vector<std::pair<std::string, QuatPiece>> quatPieceCache_;   // by piece name (e.g. "Prop_Pod")

    bool                        quatReady_ = false;
    std::vector<QuatPiece>      quatFloor_;        // 4x4 floor platform variants
    std::vector<QuatPiece>      quatWall_;         // 3 m wall panels (run along Z, face -X), variants
    QuatPiece                   quatTop_;          // wall top cap (sits at the wall height)
    QuatPiece                   quatColumn_;       // corner column (5 m)
    QuatPiece                   quatDoorFrame_;    // door frame at openings (static)
    QuatPiece                   quatDoorLeaf_;     // single door panel (PulseGame slides two per opening) [fallback]
    // Per-biome door leaf (spec biome.doorPiece). activeDoorLeaf() returns the active biome's leaf.
    std::array<QuatPiece, static_cast<size_t>(Biome::Count)> quatDoorLeafBiome_;
    const QuatPiece*            activeDoorLeafOverride_ = nullptr; // forced room template DOOR_LEAF, when present
    const QuatPiece& activeDoorLeaf() const {
        if (activeDoorLeafOverride_ && !activeDoorLeafOverride_->parts.empty()) return *activeDoorLeafOverride_;
        const size_t b = static_cast<size_t>(activeBiome_);
        if (b < static_cast<size_t>(Biome::Count) && !quatDoorLeafBiome_[b].parts.empty()) return quatDoorLeafBiome_[b];
        return quatDoorLeaf_;
    }
    std::vector<QuatPiece>      quatCover_;        // crates / barrels for cover ('c') [fallback pool]
    std::vector<QuatPiece>      quatDress_;        // small dressing props ('p'; chunky grounded pieces collide at placement) [fallback]
    // Per-biome 'p'/'c' pools (spec biome.dressing): Foundry cables/pipes, Furnace vents/pipes,
    // Reliquary lights/chests. Selected by activeBiome_ at assemble time so each biome's filler
    // reads as its own place. Empty pool -> the legacy quatDress_/quatCover_ fallback.
    std::array<std::vector<QuatPiece>, static_cast<size_t>(Biome::Count)> quatDressBiome_;
    std::array<std::vector<QuatPiece>, static_cast<size_t>(Biome::Count)> quatCrateBiome_;
    QuatPiece                   quatRamp_;         // ramp piece ('/') for Foundry/Furnace
    QuatPiece                   quatStairs_;       // stair piece ('/') for Reliquary (monumental)
    std::vector<std::pair<std::string, MaterialHandle>> quatMatCache_;  // keyed by baseColor|normal|orm uris

    // ENCLOSURE materials (rooms are sealed INTERIORS, not open-topped pads): a dark ceiling/shell
    // material that seals the room overhead + above the kit wall band so no sky shows, and a
    // per-biome EMISSIVE trim material (HDR = baseColorFactor * emissive) for conduit strips
    // (Foundry cyan), molten floor seams (Furnace orange), and ceiling light-shaft panels
    // (Reliquary cold blue) - these bloom and read as the biome's signature glow.
    MaterialHandle              quatCeilingMat_{};
    std::array<MaterialHandle, static_cast<size_t>(Biome::Count)> quatStructMat_{};
    std::array<MaterialHandle, static_cast<size_t>(Biome::Count)> quatFloorMassMat_{};
    std::array<MaterialHandle, static_cast<size_t>(Biome::Count)> quatDeckMassMat_{};
    std::array<MaterialHandle, static_cast<size_t>(Biome::Count)> quatTrimMat_{};
    MaterialHandle              quatExitMat_{};
    MaterialHandle              quatEntryMat_{};
    // Interior ceiling height per biome (metres): Furnace low + cramped, Foundry mid, Reliquary
    // monumental. Reported to the game so its light rig can hang fixtures at the right height.
    float ceilingHeightFor(Biome b) const {
        switch (b) {
            case Biome::Forest: return 5.2f;    // FURNACE: low, cramped, heavy
            case Biome::Ruins:  return 9.5f;    // RELIQUARY: monumental nave
            default:            return 6.4f;    // FOUNDRY: industrial hall
        }
    }
    float ceilingHeight() const { return ceilingHeightFor(activeBiome_); }

    KitProp                     reactor_;           // legacy sci-fi hero structure (procedural fallback room centre)
    KitProp                     envDoor_;           // whole sci-fi door wall section, drawn at SEALED doorways (game-side)
    KitProp                     hallway_;           // sci-fi corridor segment, drawn behind OPEN doors (game-side)
    std::array<KitProp, static_cast<size_t>(Biome::Count)> sketchfabHeroFocal_{}; // per-biome landmark at 'X'
    std::array<MeshyBiomeProps, static_cast<size_t>(Biome::Count)> meshyProps_{};
    MeshyCommonProps            meshyCommon_{};
    MeshHandle                  unitBoxMesh_{};     // shared unit cube for procedural cover + door slabs
    float                       centerX_ = 16.0f, centerZ_ = 12.0f;  // current open-rect centre
    float                       halfX_ = 15.0f, halfZ_ = 11.0f;      // current open-rect half-extents
    std::vector<Door>           doors_;             // current room's doors ([0] = entrance)
    const Prop* pick(const std::vector<Prop>& pool, uint32_t h) const {
        return pool.empty() ? nullptr : &pool[h % static_cast<uint32_t>(pool.size())];
    }
    std::vector<Prop>& poolFor(Cat c);

    // Load a kit's geometry into the matching pool, auto-scaling each prop to targetHeight.
    void loadKit(Engine& engine, const std::string& dir, Cat cat, Mode mode,
                 float targetHeight, float coverFrac, float trunk);
    // Build (cached) a matte material from <dir>/textures/<mat>_{diffuse|baseColor}.png + _normal.png.
    MaterialHandle kitMaterial(Engine& engine, const std::string& dir, const std::string& mat);
    MaterialHandle groundMaterial(Engine& engine, const std::string& dir, const std::string& mat, float uvTile);

    std::vector<Prop>           rocks_, smallRocks_, crates_, trees_, grass_, structures_;
    Prop                        skyline_;   // bg mountains, ringed around the arena on the horizon
    MeshHandle                  groundMesh_{};
    std::array<MaterialHandle, static_cast<size_t>(Biome::Count)> groundMat_{};
    bool                        ready_ = false;

    std::array<std::string, 24> grid_{};
    std::vector<uint8_t>        fine_;
    std::vector<float>          fineHeight_;   // per-fine-cell world top height of the cover (0 = open)
    std::vector<DungeonDraw>    draws_;
    int                         spawnX_ = 16;
    int                         spawnZ_ = 12;

    // material cache keyed by "dir|mat"
    std::vector<std::pair<std::string, MaterialHandle>> matCache_;
};

} // namespace pulse

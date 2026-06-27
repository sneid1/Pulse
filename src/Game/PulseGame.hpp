#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <optional>

#include "Engine/Input.hpp"
#include "Engine/Engine.hpp"
#include "Engine/Core/AnimatedGltf.hpp"
#include "Engine/SceneFrame.hpp"
#include "Engine/Config.hpp"
#include "Engine/UI/UiDrawList.hpp"
#include "Engine/Math.hpp"
#include "Game/Tunables.hpp"
#include "Game/Run.hpp"
#include "Game/RunMods.hpp"
#include "Game/Pulse.hpp"
#include "Game/Status.hpp"
#include "Game/Build.hpp"
#include "Game/Meta.hpp"
#include "Game/Settings.hpp"
#include "Game/Wasteland.hpp"
#include "Game/WeaponProfile.hpp"

namespace pulse {

class AudioSystem;
enum class EnemyEventType;
enum class FeedbackEventType;

class PulseGame {
public:
    PulseGame();

    bool loadConfig(bool announce = false);
    void resetForSim();   // balance-sim: clean in-memory profile, deterministic first run, no disk
    void abandonRun();    // balance-sim: drop a stuck/over-long run and start the next one
    void setForcedMods(std::vector<RunModifier> mods); // balance-sim: force a RunMods set each run
    void setSimHeat(int heat) { forcedHeat_ = heat; }  // balance-sim: force a heat level (bypasses the maxHeatUnlocked clamp)
    void buildBotInput(InputState& input, float elapsedSeconds) const;
    void debugBeginScriptedCapture();
    void debugPose();    // place a couple of drones in front for a close-up screenshot
    void debugKillAll(); // shatter all active drones (to inspect death FX)
    void debugFire(AudioSystem& audio, float pitch); // fire a wall tracer at a set pitch
    // QA: turn ON element infusions ('+'-joined burn/shock/cryo/corrode/leech, or "all") so a
    // --fire-pose capture exercises the weapon-flavored element/combo muzzle effects.
    void debugSeedShotElements(const std::string& spec);
    void debugForceWeapon(const std::string& id);    // headless capture helper
    void debugReloadPose(float progress); // freeze the viewmodel at a reload phase for capture
    void debugRewardScreen();             // force the RoomCleared reward-choice UI for capture
    void debugHubScreen();                // force the Hub UI (with meta) for capture
    void debugPathScreen();               // force the ChoosePath UI for capture
    void debugDoorsScreen();              // force the spatial DoorsOpen state (open exits) for capture
    void debugCodexScreen(int tab = 1);   // force the SYSTEMS field-manual overlay (tab 0..8) for capture
    void debugEnemyTracerPose();          // force a clean enemy-shot/tracer readability pose
    void debugShopScreen();               // force the Shop UI for capture
    void debugEventScreen();              // force the Event (deal) UI for capture
    void debugRunOverScreen();            // force the Run Report (RunOver) UI for capture
    void debugStandOnCover();             // place the player atop a low rock (jump-onto proof)
    void debugBossPose();                 // spawn the boss + elites in front for capture
    void debugMenuScreen(const std::string& which); // force a front-end screen (main/pause/options) for capture
    // The game has ONE arena mode (the Quaternius brutalist arena); mode selection was removed.
    void setForcedBiome(int b) { forcedBiome_ = b; }  // M4 dev: force a biome (0 Foundry,1 Furnace,2 Reliquary) for capture
    void setForcedRoom(const std::string& name) { forcedRoomName_ = name; }  // dev/QA: --room forces a named template (and its biome)
    void setTopDownCapture(bool b) { topDownCapture_ = b; }  // dev/QA: --topdown overrides the camera to a near-overhead view
    void setMarketingBot(bool b) { marketingBot_ = b; }       // capture-only bot: less hopping, more visible dashes
    // Headless (--bot-test / --balance-sim): resolve the spatial doors instantly by bot policy
    // instead of requiring the bot to walk through a door, so the sim never softlocks at a door.
    void setAutoResolveDoors(bool enabled) { autoResolveDoors_ = enabled; }
    void update(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH);
    // Build the engine SceneFrame (3D instances + GPU HUD) for this game state.
    // The returned frame references internal storage valid until the next call.
    const SceneFrame& buildFrame(Engine& engine, int screenW, int screenH);

    // Front-end shell (main menu / pause / options). Enabled ONLY for interactive
    // windowed play via enterMainMenu(); headless/sim/capture paths never call it, so
    // frontEnd_ stays false and their behavior is byte-for-byte unchanged. While a screen
    // is open the live sim is frozen and the OS cursor is released for navigation.
    enum class MenuScreen { None, Main, Pause, Settings };
    void enterMainMenu();   // windowed entry point: load+apply settings, show the main menu
    bool wantsMouseCapture() const {
        if (frontEnd_ && menuScreen_ != MenuScreen::None) return false;
        switch (phase_) {
            case RunPhase::Hub:
            case RunPhase::ChoosePath:
            case RunPhase::Shop:
            case RunPhase::Event:
            case RunPhase::RoomCleared:
            case RunPhase::RunOver:
                return false;
            default:
                return true;
        }
    }
    bool wantsQuit() const { return wantsQuit_; }
    // Window presentation chosen in Options: 0 = Windowed, 1 = Borderless fullscreen. The
    // windowed host polls this each frame and switches the actual window when it changes.
    int  displayMode() const { return settings_.displayMode; }

    const Tunables& tunables() const { return tunables_; }
    const StyleConfig& style() const { return style_; }   // W5 locked style library (config/pulse.style)
    int score() const { return score_; }
    int bestScore() const { return bestScore_; }
    int playerHp() const { return player_.hp; }
    int playerShield() const { return player_.shield; }
    int activeEnemyCount() const;
    // Run/phase telemetry (Phase A) for the headless --bot-test verify.
    int runRoomIndex() const { return run_.roomIndex(); }
    int runRoomCount() const { return run_.roomCount(); }
    int roomsCleared() const { return roomsClearedTotal_; }
    int runsEnded() const { return runsEndedTotal_; }
    int bossesReached() const { return bossesReachedTotal_; }
    int runsWon() const { return runsWonTotal_; }
    int buildItemCount() const { return build_.totalItems(); }
    int loadoutSize() const { return static_cast<int>(loadout_.size()); }
    int metaCurrency() const { return meta_.currency(); }
    int metaUnlocked() const { return meta_.unlockedCount(); }
    int runScrap() const { return scrap_; }   // Feature 2 economy telemetry (per-run)
    int roomTypeEntered(int type) const { return (type >= 0 && type < 6) ? roomTypeCounts_[static_cast<size_t>(type)] : 0; }
    // M1 Pulse telemetry: sampled over in-combat frames across the whole sim batch.
    double avgPulse() const { return pulseSampleFrames_ > 0 ? pulseMeterSum_ / static_cast<double>(pulseSampleFrames_) : 0.0; }
    double pulseTierFraction(int tier) const {
        if (tier < 0 || tier > 4 || pulseSampleFrames_ == 0) return 0.0;
        return static_cast<double>(pulseTierFrames_[static_cast<size_t>(tier)]) / static_cast<double>(pulseSampleFrames_);
    }
    double statusUptime() const { return statusEnemyFrames_ > 0 ? static_cast<double>(statusActiveFrames_) / static_cast<double>(statusEnemyFrames_) : 0.0; }
    const WeaponProfileRegistry& weaponProfiles() const { return weaponProfiles_; }
    bool runWeaponSelfTest(std::string& report) const;
    const char* phaseName() const;
private:
    enum class EnemyKind {
        Rusher,
        Ranged,
        Tank,
        Stalker
    };
    static constexpr int EnemyKindCount = 4;

    enum class EnemyVisual {
        Rusher001,
        Gunner002,
        Tank003,
        Stalker004,
        Drone005,
        Warden006,
        Choir008,
        Fast009,
        Shielded010,
        Volatile011,
        Regen012,
        Husk013,
        Summoner014,
        Count
    };
    static constexpr int EnemyVisualCount = static_cast<int>(EnemyVisual::Count);

    // Elite affixes recombine the threat space cheaply (Phase D, combat-depth lever):
    // they change the puzzle, never inflate base HP. Fast punishes camping, Shielded
    // forces commitment/priority, Volatile punishes careless proximity on the kill,
    // Regen punishes leaving a target half-dead.
    enum class EliteAffix { None, Fast, Shielded, Volatile, Regen };

    struct EnemyStyle {
        float scale = 0.46f;
        uint32_t body = 0;
        uint32_t wing = 0;
        uint32_t eye = 0;
    };

    struct Player {
        Vec2 pos{16.0f, 12.0f};
        Vec2 vel{};
        float yaw = -1.5707963f;
        float pitch = 0.0f;
        int hp = 100;
        int shield = 0;
        float dashTime = 0.0f;
        float dashCooldown = 0.0f;
        float dashSpeed = 0.0f;  // dash velocity baked at start (air dashes go further); held for dashTime
        Vec2 dashDir{};
        float height = 0.78f; // eye height above the floor (0..1 room height)
        float vz = 0.0f;     // vertical velocity for jumping/falling
        bool grounded = true;
        int   airJumps = 0;      // remaining mid-air jumps this airtime (reset on landing)
        float coyote = 0.0f;     // grace window to still ground-jump just after leaving an edge
        float jumpBuffer = 0.0f; // remembers a recent jump press so it fires the instant you can
        float jumpFromH = 0.0f;  // feet height of the surface a jump launched from (for a relative
                                 // apex cap, so a jump off a raised deck does not snap you through it)
        bool dead = false;
    };

    struct Weapon {
        int ammo = 30;
        int reserve = 90;
        float timeSinceShot = 99.0f;
        float queuedBurstTimer = 0.0f;
        int queuedBurstShots = 0;
        float reloadRemaining = 0.0f;
        float shellReloadTimer = 0.0f;
        bool reloading = false;
        bool reloadMagOutPlayed = false;
        bool reloadInsertPlayed = false;
        bool reloadEndPlayed = false;
    };

    // Enemy ranged attack archetypes (Returnal-style variety). Chosen per shot by kind so
    // the threats read differently: a single aimed orb, a fanned spread, a quick burst stream,
    // or a telegraphed lock-on beam. The boss runs its own radial pattern.
    enum class EnemyAttack { Orb, Fan, Burst, Beam };

    struct Enemy {
        EnemyKind kind = EnemyKind::Rusher;
        EliteAffix affix = EliteAffix::None;
        int visual = -1;          // -1 = derive the Meshy body from kind/affix
        float maxHealth = 100.0f;   // for Regen clamping + elite shield read
        float regenCooldown = 0.0f; // Regen: time since last hit before healing resumes
        bool boss = false;          // Phase D boss entity (large, telegraphed, summons)
        float bossAttackTimer = 0.0f;
        int bossPhase = 0;
        // M5 boss roster: a distinct boss per biome with its own moveset. bossKind selects the
        // pattern set; bossPattern cycles attack variants; bossVulnTimer is the post-attack
        // WEAK-POINT window (read the telegraph, dodge, then punish the recovery for bonus damage).
        int bossKind = 0;           // 0 Warden (Volt), 1 Smelter (Pyro), 2 Choir (Cryo)
        int bossPattern = 0;
        float bossVulnTimer = 0.0f; // > 0 = exposed (bonus damage taken + a bright tell)
        Vec2 pos{};
        Vec2 vel{};
        float health = 100.0f;
        float hurtTimer = 0.0f;
        float telegraphRemaining = 0.0f;
        float attackCooldown = 0.0f;
        float bobPhase = 0.0f;  // per-enemy hover offset so the swarm isn't in lockstep
        float animTime = 0.0f;  // steady idle/breathing clock (loops at a fixed rate)
        float walkPhase = 0.0f; // walk clock driven by DISTANCE travelled (anti foot-slide)
        float hitPunch = 0.0f;  // brief scale-up on taking a hit, for impact
        int   comboKind = 0;    // last element-pair reaction, render-only emphasis
        float comboTimer = 0.0f;
        float comboCooldown = 0.0f;
        float lungeTime = 0.0f; // rusher/stalker: remaining time in an active burst
        float recover = 0.0f;   // post-attack recovery: can't act until it elapses
        bool struck = false;    // melee wind-up has resolved this attack
        bool pendingRanged = false; // the active telegraph resolves to a shot, not a melee strike
        // Ranged attack variety (chosen at telegraph start in beginShot).
        EnemyAttack pendingAttack = EnemyAttack::Orb;
        Vec2  attackAim{};          // aim direction locked for the pattern (fan centre / burst line)
        int   burstShotsLeft = 0;   // Burst: orbs still to fire
        float burstTimer = 0.0f;    // Burst: countdown to the next orb
        // Lock-on beam: dir locks partway through the wind-up (the dodge window), then the beam
        // fires anchored to the LIVE enemy muzzle so it never floats in thin air as the enemy moves.
        bool  beamLocked = false;   // telegraph has frozen beamDir (final dodge window)
        Vec2  beamDir{};            // locked beam direction (ground plane)
        float beamLen = 0.0f;       // hitscan length to the wall/range
        float beamFireTimer = 0.0f; // > 0 while the bright beam is firing (drives the render)
        // Stuck-recovery (walled arenas): the straight-line AI can wedge against cover with the
        // player behind it. stuckTimer grows while it wants to advance but cannot + has no LOS;
        // it then steers tangentially around the obstacle, and blinks to open floor as a last resort.
        Vec2  prevPos{};
        float stuckTimer = 0.0f;
        float blinkFlash = 0.0f;    // brief glow after a recovery blink (render only)
        StatusState status;         // M2: per-enemy elemental status (burn/shock/cryo/corrode)
        bool active = true;
    };

    // A slow, visible orb fired by a Ranged enemy. It travels through the world
    // so the player can always see and dodge the threat (no hitscan surprises).
    struct Projectile {
        Vec2 pos{};
        Vec2 vel{};
        Vec2 origin{};   // where it was fired from, for the damage-direction cue
        float height = 0.5f;
        float age = 0.0f;
        float life = 5.0f;
        int damage = 18;
        bool active = true;
        bool hostile = true;     // true = enemy orb (hits player); false = player bolt (hits enemies)
        float splashRadius = 0.0f; // player bolt AoE on impact (0 = single target)
        Vec3f color{ 1.45f, 0.55f, 0.32f }; // HDR tint of the orb, its trail and its light
        uint32_t effectMask = 0;             // player shot elements/leech for in-world stripes/glows
        int   shape = 0;                    // 0 smooth gem, 1 spike, 2 shard (see enemyShotShape)
        EnemyKind sourceKind = EnemyKind::Ranged;
        EnemyAttack sourceAttack = EnemyAttack::Orb;
        bool sourceBoss = false;
        int sourceBossKind = 0;
    };

    // An instant, telegraphed enemy BEAM/ray: a fixed line of energy that re-emits its
    // particle lance each frame for its short life. Dodged during the wind-up by not
    // standing on the line when it fires (damage is applied once, on spawn).
    struct Beam {
        Vec3f from{};
        Vec3f to{};
        Vec3f color{ 1.55f, 0.40f, 1.70f };
        float age = 0.0f;
        float life = 0.16f;
    };

    // A fading wedge drawn at the screen edge pointing toward a damage source,
    // so a hit always reads as coming "from there", never out of nowhere.
    struct DamageMarker {
        float worldAngle = 0.0f; // atan2 of (source - player) at the moment of the hit
        float age = 0.0f;
        float life = 1.5f;
        float intensity = 1.0f;
    };

    struct CombatText {
        Vec3f pos{};
        Vec3f vel{};
        std::string text;
        uint32_t color = 0xFFFFFFFFu;
        float age = 0.0f;
        float life = 0.75f;
        float scale = 1.0f;
    };

    // A spinning shard of a destroyed drone (one per source triangle).
    struct Debris {
        Vec2 pos{};
        float height = 0.0f;
        Vec2 vel{};
        float vh = 0.0f;
        float yaw = 0.0f;
        float spin = 0.0f;
        float vx[3] = {0, 0, 0};
        float vy[3] = {0, 0, 0};
        float vz[3] = {0, 0, 0};
        uint32_t color = 0;
        float age = 0.0f;
        float life = 0.8f;
    };

    // One triangle of the procedural enemy "drone" mesh, in local space
    // (+X forward toward the player, +Y up, +Z right). kind drives shading.
    struct MeshTri3 {
        float vx[3] = {0, 0, 0};
        float vy[3] = {0, 0, 0};
        float vz[3] = {0, 0, 0};
        int part = 0; // 0 = body, 1 = eye (emissive), 2 = wing
    };

    struct Tracer {
        Vec2 start{};
        Vec2 end{};
        float startHeight = 0.0f;
        float endHeight = 0.0f;
        float age = 0.0f;
        float duration = 0.12f;
        bool hit = false;
        uint32_t effectMask = 0;
        Vec3f color{ 0.45f, 0.95f, 1.45f };
    };

    struct EnemyBeamLine {
        Vec3f from{};
        Vec3f to{};
        Vec3f color{ 1.55f, 0.40f, 1.70f };
        float intensity = 1.0f;
        float worldWidth = 0.02f;
    };

    struct ScreenCasing {
        float x = 0.0f;
        float y = 0.0f;
        float vx = 0.0f;
        float vy = 0.0f;
        float angle = 0.0f;
        float spin = 0.0f;
        float age = 0.0f;
        float life = 0.42f;
        float size = 1.0f;
    };

    struct Impact {
        Vec2 pos{};
        float height = 0.5f;
        float age = 0.0f;
        float duration = 0.34f;
        bool hit = false;
    };

    // A CPU-simulated, GPU-rendered additive particle (spark / ember / mote). The
    // engine draws these as camera-facing billboards composited into the HDR scene.
    struct WorldParticle {
        Vec3f pos{};
        Vec3f vel{};
        float life = 0.0f;
        float maxLife = 0.5f;
        float size = 0.04f;
        Vec3f color{ 1.0f, 0.7f, 0.3f };
        float emissive = 3.0f;
        float gravity = 1.2f;
        float drag = 1.5f;
        float stretch = 0.0f;   // >0 = motion-streak billboard (elongated along velocity)
    };

    enum class PickupKind {
        Health,
        Shield,
        Ammo
    };

    struct Pickup {
        PickupKind kind = PickupKind::Health;
        Vec2 pos{};
        float age = 0.0f;
        float phase = 0.0f;
    };

    // World-space impact pop spawned where an enemy dies. Drawn as an expanding,
    // fading starburst so a kill reads as decisive (spec section 5).
    struct Burst {
        Vec2 pos{};
        float age = 0.0f;
        float duration = 0.30f;
        bool headshot = false;
    };

    struct MeshVertex {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    struct MeshUv {
        float u = 0.0f;
        float v = 0.0f;
    };

    struct MeshNormal {
        float x = 0.0f;
        float y = 1.0f;
        float z = 0.0f;
    };

    struct MeshTriangle {
        int a = 0;
        int b = 0;
        int c = 0;
        int ta = 0;
        int tb = 0;
        int tc = 0;
        int na = -1;
        int nb = -1;
        int nc = -1;
        uint32_t color = 0x00FFFFFFu;
        float emissive = 0.0f;
    };

    struct MeshAsset {
        std::vector<MeshVertex> vertices;
        std::vector<MeshUv> uvs;
        std::vector<MeshNormal> normals;
        std::vector<MeshTriangle> triangles;
        MeshVertex center{};
        bool loaded = false;
    };

    struct AnimatedViewmodelRuntime {
        AnimatedGltfModel model;
        std::vector<AnimatedGltfSubmesh> sampled;
        std::vector<AnimatedGltfSubmesh> neutralIdle;
        std::vector<MeshHandle> meshes;
        std::vector<MaterialHandle> materials;
        int idleClip = -1;
        int fireClip = -1;
        int reloadClip = -1;
        float idleStart = 0.0f, idleEnd = 0.0f;
        float fireStart = 0.0f, fireEnd = 0.0f;
        float reloadStart = 0.0f, reloadEnd = 0.0f;
        float idleDampScale = 1.0f;
        bool hideSupportHandUntilReload = false;
        bool loaded = false;
        bool gpuReady = false;
    };

    // The bumstrum enemy glTFs bake every motion into one unlabelled clip, so each
    // gameplay state maps to a [start,end] window (seconds) inside that single clip.
    // hasAttack is false for models whose clip carries no strike (Tank reuses walk).
    struct EnemyClipRanges {
        float idle0 = 0.0f, idle1 = 0.0f;
        float walk0 = 0.0f, walk1 = 0.0f;
        float atk0 = 0.0f, atk1 = 0.0f;
        bool hasAttack = false;
    };

    // A skinned enemy archetype loaded from a bumstrum glTF. One per EnemyKind.
    // Animation is CPU-skinned via AnimatedGltfModel (same path as the pistol
    // viewmodel); vertCounts/indices capture the fixed submesh topology so each
    // drawn enemy can own a small pool of dynamic meshes refreshed per frame.
    struct AnimatedEnemyModel {
        AnimatedGltfModel model;
        std::vector<MaterialHandle> materials;
        std::vector<uint32_t> vertCounts;          // per submesh
        std::vector<std::vector<uint32_t>> indices; // per submesh
        std::vector<MeshHandle> staticMeshes;       // shared GPU meshes for no-clip static GLBs
        std::vector<int> submeshMaterial;          // per submesh -> material slot
        std::vector<uint8_t> submeshVisible;       // per submesh -> 0 hides it (e.g. the cultist's gun)
        EnemyClipRanges ranges;
        // Multi-clip (Mixamo) rigs carry named role clips; when present we drive a small
        // locomotion + action state machine (clip-by-name) instead of windowing one baked clip.
        bool multiClip = false;
        bool stylizedTextured = false;  // rig carries its own texture (e.g. KayKit) -> skip the obsidian shadow-smoke aura
        struct RoleClips {
            int idle = -1, walk = -1, run = -1, back = -1, strafeL = -1, strafeR = -1;
            int cast = -1, castHeavy = -1, channel = -1, lunge = -1, hit = -1, hitHeavy = -1, death = -1;
        } role;
        int clip = -1;
        float worldScale = 1.0f;  // glTF units -> world (target height / bind height)
        float worldHeight = 1.0f; // target standing height in room units (anchors the magenta core)
        float collisionRadius = 0.35f; // gameplay footprint derived from the visible body
        float footY = 0.0f;       // bind-pose min Y in glTF units (grounds the feet)
        float poseAnchorX = 0.0f; // idle-pose centroid; used to strip root drift from clips
        float poseAnchorY = 0.0f;
        float poseAnchorZ = 0.0f;
        float yawOffset = 0.0f;   // facing correction so +forward points at the player
        float hoverY = 0.0f;      // flyers (drones): lift the grounded model off the floor by this much
        bool loaded = false;
        bool gpuReady = false;
    };

    // One pooled GPU instance: a dynamic mesh per submesh, reused frame to frame so
    // each live enemy of a kind can be posed independently without reallocating.
    struct AnimatedEnemyInstance {
        std::vector<MeshHandle> meshes;
    };

    // A transient death-animation body spawned when a skinned enemy dies. Pure visual (no
    // gameplay state): the live enemy despawns as usual, this plays the rig's `death` clip
    // once and dissolves. Rendered after the live enemies, reusing the per-kind pose pool.
    struct EnemyCorpse {
        EnemyKind kind = EnemyKind::Rusher;
        int   visual = -1;
        bool  boss = false;   // use the unique boss model for the corpse, not the Tank model
        int   bossKind = 0;
        Vec2 pos{};
        Vec2 facing{ 1.0f, 0.0f };
        float age = 0.0f;
        float dur = 1.4f;     // total life (death clip play + dissolve)
        float scale = 1.0f;   // worldScale * bossScale captured at death
        // Flyers (drones) have no death clip: instead of playing one, the corpse freezes its idle
        // pose and physically falls + tumbles to the floor under gravity, then dissolves.
        bool  fall = false;
        float fallY = 0.0f;   // current height above the grounded rest position (starts at hover)
        float vy = 0.0f;      // vertical velocity (gravity-integrated while airborne)
        float spin = 0.0f;    // tumble angle while falling
    };

    struct RayHit {
        float distance = 1000.0f;
        int side = 0;
        char cell = '#';
        float wallX = 0.0f; // fractional hit position along the wall, for texture U
        bool cover = false; // interior obstacle (gets the accent texture) vs boundary
    };

    struct Texture {
        struct Level {
            std::vector<uint32_t> pixels;
            int width = 0;
            int height = 0;
        };
        // levels[0] is full resolution; each subsequent level is half size. Mips
        // are selected by distance to stop floor/ceiling texturing from shimmering.
        std::vector<Level> levels;
        bool valid() const { return !levels.empty() && levels.front().width > 0; }
        int maxLevel() const { return static_cast<int>(levels.size()) - 1; }
    };

    struct Projection {
        bool visible = false;
        int left = 0;
        int right = 0;
        int top = 0;
        int bottom = 0;
        float depth = 0.0f;
        float side = 0.0f;
    };

    void resetRun();
    void updateCombat(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH);
    void updatePlayer(const InputState& input, AudioSystem& audio, float dt);
    void updateWeapon(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH);
    void updateEnemies(AudioSystem& audio, float dt);
    void updateProjectiles(AudioSystem& audio, float dt);
    void updateSpawning(float dt);
    // Phase A run/room state machine (above the flat combat update).
    void beginRun();
    void beginRoom();
    void startWave(int index);
    // RunMods foundation: seedRunMods clears activeMods_ and re-seeds it each run from
    // forcedMods_ (sim) and, in M4, the heat table; recomputeMods folds it into mods_.
    void seedRunMods();
    void recomputeMods();
    bool roomComplete() const;
    void enterRoomCleared(AudioSystem& audio);
    void advanceToNextRoom(AudioSystem& audio);
    void enterChoosePath(AudioSystem& audio);   // Feature 1: pick the next room from typed options
    // Spatial doors (brutalist arenas only): the Hades-style merged choice. On clear the run
    // advances and each next-room option binds to an open exit door carrying a reward preview;
    // walking through door i commits that reward + route in one act, replacing RoomCleared+ChoosePath.
    bool envHasDoors() const { return wasteland_.doorCount() > 0; }
    void enterDoorsOpen(AudioSystem& audio); // advance the run, roll a reward per option, open the exits
    void commitDoor(int doorIndex, AudioSystem& audio); // grant reward + chooseOption + load the next area
    void grantDoorReward(const std::string& rewardId, AudioSystem& audio); // apply one reward (no advance)
    int  doorAtPlayer() const;               // open-door index the player overlaps, else -1
    int  botDoorPick() const;                // deterministic headless door choice (policy, no RNG)
    void leaveServiceRoom(AudioSystem& audio); // Shop/Event exit: doors when available, else the menu
    // Feature 2: in-run shop. Entered from a Shop room; spend scrap on gear / heal / reroll.
    void enterShop();
    void rollShopStock();
    void buyShopItem(int index, AudioSystem& audio);
    void shopHeal(AudioSystem& audio);
    void shopReroll(AudioSystem& audio);
    void shopForge(AudioSystem& audio);   // M6: pour scrap into the active weapon (+power, unlocks aspects)
    // Feature 3: risk/reward deals. An Event room rolls 1-2 deals; accept applies a boon +
    // pushes curse RunModifiers (via the M0 read-sites), decline resolves with nothing.
    void enterEvent();
    void acceptDeal(int slot, AudioSystem& audio);
    void expireSectorCurses();   // drop sector-scoped deal mods when the sector advances
    void enterRunOver(bool won, AudioSystem& audio);
    void enterHub();              // Phase C: between-runs meta hub (spend, then START)
    void updateHub(const InputState& input, AudioSystem& audio, int screenW, int screenH);
    // Front-end shell internals. applySettings folds settings_ into tunables_ (idempotent);
    // updateMenu navigates the active screen; buildMenuOverlay draws it.
    void applySettings();
    void updateMenu(const InputState& input, AudioSystem& audio);
    void buildMenuOverlay(UiDrawList& ui, int screenW, int screenH);
    float runIntensityFloor() const;
    // Phase B stacking build: effective caps after build bonuses, kill/hit hooks,
    // and the reward-choice flow.
    int effectiveMaxHealth() const { return tunables_.playerMaxHealth + build_.stats().maxHealthBonus + meta_.mirrorBonusHealth(); }
    int effectiveMaxShield() const { return tunables_.playerMaxShield + build_.stats().maxShieldBonus; }
    // RunMods: heal-reduction curses scale the AMOUNT recovered (kill heals, pickups,
    // shop heal) - never a price. Default healMult is 1.0 (identity, no-op).
    int healAmount(int base) const { return std::max(0, static_cast<int>(std::lround(base * mods_.healMult))); }
    float reloadDuration() const {
        return std::max(0.1f, weaponBaseReload() / std::max(0.2f, build_.stats().reloadSpeedMult));
    }
    void onEnemyKilled(AudioSystem& audio, const Enemy& enemy, bool headshot);
    // Applies dmg (Shielded halves it, Corrode amps, frozen shatters); true if it kills. The optional
    // corrodeBonus/shatterBonus out-params report how much of the applied damage came from the corrode
    // amp and the freeze-shatter mult, so a caller can pop them as their own numbers (display only).
    bool damageEnemy(Enemy& e, float dmg, float* appliedDamage = nullptr,
                     float* corrodeBonus = nullptr, float* shatterBonus = nullptr);
    // Pop the corrode (+green) and shatter (+blue) shares of a hit as their own small numbers.
    void spawnStatusBonusText(const Enemy& e, float corrodeBonus, float shatterBonus, float baseHeight);
    // M2 status-element layer: apply stacks of an element to an enemy on hit (Corrode amps the
    // application); tick all enemies' statuses each frame (DoT, freeze countdown, chain discharge).
    void applyElement(Enemy& e, Element elem, float stacks, AudioSystem* audio = nullptr);
    void applyBuildElements(AudioSystem* audio, int enemyIndex);     // apply the build's per-hit element stacks to a struck enemy
    void applyShotElements(AudioSystem& audio, int enemyIndex);      // build elements + active weapon aspect on a struck enemy
    void applyLifeLeech(AudioSystem& audio, float appliedDamage, Vec3f from);
    void triggerElementCombo(Enemy& e, Element incoming, float stacks,
                             bool hadBurn, bool hadShock, bool hadCryo, bool hadCorrode,
                             AudioSystem* audio);
    void playElementFeedback(AudioSystem* audio, Element elem, float volume = 0.65f);
    void playComboFeedback(AudioSystem* audio, float volume = 0.86f);
    void playLeechFeedback(AudioSystem* audio, float volume = 0.56f);
    void updateStatuses(AudioSystem& audio, float dt);
    void spawnBoss();                      // M5: the boss-room boss (kind picked by biome)
    void updateBoss(Enemy& e, AudioSystem& audio, float dt, Vec2 dir, float dist, bool hasLos);
    int  summonBossAdds(Enemy& e, int count, bool preferStalker);  // M5: queue adds under the crowd cap
    static const char* bossName(int kind); // M5: Warden / Smelter / Choir (per biome)
    void applyAreaDamage(AudioSystem& audio, Vec2 center, float radius, float damage, int ignoreIndex,
                         bool carryShotEffects = false);
    void grantReward(int index, AudioSystem& audio);
    // Phase B.2 weapon archetype kernel: the active weapon's resolved stats, the
    // shared per-ray resolver, player projectiles, and loadout acquire/swap.
    const WeaponDef& activeWeaponDef() const;
    const WeaponProfile& activeWeaponProfile() const;
    const WeaponProfile& weaponProfileForId(const std::string& id) const;
    int activeWeaponPower() const;
    // M3 weapon aspects: the active slot's current form (nullptr = base), how many forms its
    // power has unlocked, and the firing multipliers the form contributes. cycleAspect rotates
    // the active weapon among its unlocked forms.
    const WeaponAspect* activeAspect() const;
    int aspectsUnlocked() const;       // forms available at the current power (1 = base only)
    float aspectDamageMult() const { const WeaponAspect* a = activeAspect(); return a ? a->damageMult : 1.0f; }
    float aspectFireRateMult() const { const WeaponAspect* a = activeAspect(); return a ? a->fireRateMult : 1.0f; }
    float aspectReloadMult() const { const WeaponAspect* a = activeAspect(); return a ? a->reloadMult : 1.0f; }
    uint32_t activeShotEffectMask() const;
    Vec3f shotEffectTint(uint32_t mask) const;
    void cycleAspect(AudioSystem& audio);
    float weaponBaseDamage() const;
    float weaponBaseFireRate() const;
    int weaponMagazine() const;
    int weaponReserveMax() const;
    float weaponBaseReload() const;
    void resolveHitscan(AudioSystem& audio, float shotYaw, float shotPitch, float baseDamage, int screenW, int screenH);
    void spawnPlayerProjectile(float shotYaw, float shotPitch, float damage, float speed, float splashRadius);
    void acquireWeapon(const std::string& id, AudioSystem& audio);
    void swapWeapon(AudioSystem& audio);
    // Quick-draw toggle between the pistol sidearm and the last-used main weapon.
    void quickSwapPistol(AudioSystem& audio);
    // Shared swap mechanics (park/restore ammo, settle, kick, equip cue).
    void performSwapTo(int target, AudioSystem& audio);
    // Phase B.3 abilities (the timing/placement skill axis). Charged by aggression
    // (kills), not a wall clock, so flow play buys agency and turtling starves it.
    void addAbilityCharge(AudioSystem& audio, float tactical, float ultimate);
    void throwGrenade(AudioSystem& audio);     // tactical: arcing AoE bolt
    void activateUltimate(AudioSystem& audio);  // ultimate: Overdrive damage/fire-rate window
    float overdriveDamageMult() const { return overdriveTimer_ > 0.0f ? 1.8f : 1.0f; }
    float overdriveFireRateMult() const { return overdriveTimer_ > 0.0f ? 1.5f : 1.0f; }
    void updatePickups(AudioSystem& audio, float dt);
    void updateTimers(float dt);
    void tryFire(AudioSystem& audio, int screenW, int screenH);
    bool fireProfileShot(AudioSystem& audio, const WeaponProfile& profile, int screenW, int screenH, bool consumeAmmo);
    int acquireTarget(float shotYaw, float shotPitch, int screenW, int screenH, bool& outHeadshot) const;
    Vec2 separationForce(const Enemy& self) const;
    void spawnProjectile(Vec2 from, float fromHeight, Vec2 dir, int damage,
                         Vec3f color = { 1.45f, 0.55f, 0.32f }, int shape = 0,
                         EnemyKind sourceKind = EnemyKind::Ranged, bool sourceBoss = false,
                         EnemyAttack sourceAttack = EnemyAttack::Orb, int sourceBossKind = 0);
    void addDamageMarker(Vec2 source, float intensity);
    void damagePlayer(AudioSystem& audio, int amount, Vec2 source);
    bool spawnEnemy(const WaveSpec& wave);
    void spawnPickup(PickupKind kind);
    void removeDeadEnemies();
    void addShake(float degrees);
    void spawnBurst(Vec2 pos, bool headshot);
    void spawnImpact(Vec2 pos, float height, bool hit);
    void spawnCombatText(Vec3f pos, const std::string& text, uint32_t color, float scale = 1.0f);
    void spawnCasing(const WeaponProfile& profile, int screenW, int screenH);
    // Persistent projected decal (bullet mark kind 0 / scorch kind 1) on a world
    // surface. center is 3D (X=pos.x, Y=height 0..1, Z=pos.y); normal is the surface
    // normal. Ring-buffered so the count stays bounded.
    void spawnDecal(Vec3f center, Vec3f normal, uint32_t kind, float size, Vec3f color, float alpha);
    void buildEnvDecals();   // per-room biome floor markings (chevrons/code/sigils), edge + focal weighted
    void updateAmbientVfx(float dt);   // per-biome atmosphere particles (embers/motes/shimmer/spark)
    // Spawn a burst of GPU-rendered additive particles (sparks/embers/motes) at a 3D
    // world point with a base velocity and spread.
    void spawnParticles(Vec3f origin, Vec3f baseVel, int count, float spread,
                        float life, float size, Vec3f color, float emissive,
                        float gravity = 1.2f, float drag = 1.5f, float stretch = 0.0f);
    void updateParticles(float dt);
    // Per-kind HDR colour of an enemy's energy bolt (its orb, trail and light).
    const char* enemyAudioBankId(EnemyKind kind, bool boss) const;
    const char* enemyAudioBankId(const Enemy& e) const;
    void playEnemyAudio(AudioSystem& audio, const Enemy& e, EnemyEventType event, float volume);
    void playFeedback(AudioSystem& audio, FeedbackEventType event, float volume);
    // v4 (S3): open a per-frame music-context trace CSV (written during a --bot-test run) for the
    // pulse_music_trace.py analyzer. Empty path disables it. Public: set by main.cpp.
public:
    void setMusicTracePath(const std::string& path);
private:
    Vec3f enemyShotColor(EnemyKind kind, bool boss) const;
    Vec3f enemyShotColor(EnemyKind kind, EnemyAttack attack, bool boss, int bossKind) const;
    // Orb silhouette per threat type: 0 = smooth gem (casters), 1 = heavy spike (rushers/
    // tanks/boss), 2 = sharp shard/dart aligned to travel (stalkers). Reads the threat at a glance.
    int   enemyShotShape(EnemyKind kind, bool boss) const;
    int   enemyShotShape(EnemyKind kind, EnemyAttack attack, bool boss) const;
    // Fire a Returnal-style energy bolt from an enemy toward aimPoint: spawns the
    // projectile plus a bright muzzle bloom in the enemy's colour.
    void spawnEnemyShot(const Enemy& e, Vec2 aimPoint);
    // Reusable VFX primitives: a radial blast nova and a particle beam/ray.
    void spawnBlast(Vec3f at, Vec3f color, float power);
    void spawnAirJumpBurst();   // dust + energy kick at the feet on a mid-air (double) jump
    void spawnBeam(Vec3f from, Vec3f to, Vec3f color, float coreSize, float spacing);
    // Per-weapon muzzle character (barrel bloom + hot spark fan + powder/charge smoke) layered on
    // the base flash so each gun's report reads distinctly. Pure VFX - no gameplay/RNG-sequence
    // coupling; the recipe is keyed by weapon id with an archetype fallback.
    void spawnMuzzleSignature(const WeaponProfile& profile, Vec3f muzzle, Vec2 aimDir, Vec2 aimRight,
                              float flash, uint32_t shotMask);
    // The elemental tell at the barrel: each active element drawn with the weapon's character (cone,
    // reach, density), the matching pair-reaction muzzle when two elements are active, and a prismatic
    // overload for three-plus. Leech draws as a separate utility beam. Pure VFX.
    void spawnElementMuzzleFx(const WeaponProfile& profile, Vec3f muzzle, Vec2 aimDir, Vec2 aimRight,
                              float flash, uint32_t shotMask);
    // Fire an instant, telegraphed beam from an enemy toward aimPoint (hitscan vs walls
    // + a line hit-test on the player), registering a Beam that lances particles.
    void fireEnemyBeam(const Enemy& e, Vec2 aimPoint, AudioSystem& audio);
    void updateBeams(float dt);

    bool isWallCell(int x, int y) const;
    // feet = the world height of the player's feet; cover taller than feet blocks, cover at
    // or below feet is passable (jumped over / standable). Default 0 = ground-level (blocks
    // all cover) for enemies and other callers that never leave the floor.
    bool collides(Vec2 pos, float radius, float feet = 0.0f) const;
    bool pushOutOfCollision(Vec2& pos, float radius, float feet = 0.0f) const;
    void moveWithCollision(Vec2& pos, Vec2& vel, float radius, float dt, float feet = 0.0f) const;
    // Smooth support height under the player, else 0 (floor). Ramps sample near the player's
    // centre so fine collision cells do not make the camera bob like stairs.
    float groundHeightAt(Vec2 pos, float radius, float feet) const;
    float smoothProcEnvGroundHeightAt(Vec2 pos, float feet, float landTol) const;
    bool lineOfSight(Vec2 from, Vec2 to) const;
    // Height-aware line of sight for a SHOT: the ray from (from, fromH) to (to, toH) is blocked
    // only by an obstacle whose top rises above the ray at that point. Fixes the elevated-shooter
    // case where the flat 2D lineOfSight wrongly treats low cover (or the block you stand ON) as
    // a wall. Heights are world units (same scale as procEnvFineHeight / player_.height).
    bool shotClearTo(Vec2 from, float fromH, Vec2 to, float toH) const;
    RayHit castRay(Vec2 origin, float angle, float maxDistance) const;
    Projection projectEnemy(const Enemy& enemy, float yaw, float pitch, int screenW, int screenH) const;
    Projection projectPoint(Vec2 point, float yaw, float pitch, int screenW, int screenH, float size, float bodyY = 0.5f) const;
    // World Y of an enemy's body centre for aiming/projection: ground enemies sit at ~0.5; flyers
    // (drones) are lifted by their model hoverY, so the hit-box must follow them up off the floor.
    float enemyAimY(const Enemy& enemy) const;
    float enemyCollisionRadius(const Enemy& enemy) const;
    bool loadAnimatedWeaponViewmodel(const WeaponProfile& profile);
    bool loadAllWeaponViewmodels();
    bool ensureAnimatedViewmodelResources(AnimatedViewmodelRuntime& runtime, Engine& engine);
    bool ensureActiveWeaponViewmodelResources(Engine& engine);
    void updateAnimatedViewmodel(AnimatedViewmodelRuntime& runtime, Engine& engine);
    void updateActiveWeaponViewmodel(Engine& engine);
    AnimatedViewmodelRuntime* activeViewmodelRuntime();
    const AnimatedViewmodelRuntime* activeViewmodelRuntime() const;
    AnimatedEnemyModel& bossModel(int bossKind);
    const AnimatedEnemyModel& bossModel(int bossKind) const;
    int defaultEnemyVisual(EnemyKind kind) const;
    int chooseEnemyVisual(EnemyKind kind, EliteAffix affix, uint32_t salt) const;
    int enemyVisualIndex(const Enemy& enemy) const;
    int enemyVisualIndex(const EnemyCorpse& corpse) const;
    EnemyKind enemyVisualKind(int visual) const;
    bool enemyVisualIsFlyer(int visual) const;
    AnimatedEnemyModel& enemyRenderModel(const Enemy& enemy);
    const AnimatedEnemyModel& enemyRenderModel(const Enemy& enemy) const;
    AnimatedEnemyModel& enemyRenderModel(const EnemyCorpse& corpse);
    const AnimatedEnemyModel& enemyRenderModel(const EnemyCorpse& corpse) const;
    bool loadAnimatedEnemies();                        // load the enemy rigs/static GLBs
    bool ensureAnimatedEnemyResources(Engine& engine); // create their textured materials
    // Sample an enemy's clip for its current state and return the kind's pooled GPU
    // instance (creating/growing the pool as needed); empty span if not ready.
    AnimatedEnemyInstance* poseAnimatedEnemy(Engine& engine, const Enemy& e, size_t slot, Vec2 facing);
    AnimatedEnemyInstance* poseDeadEnemy(Engine& engine, const EnemyCorpse& corpse, size_t slot);
    AnimatedEnemyInstance* commitEnemyPose(Engine& engine, EnemyKind kind, size_t slot, bool boss = false, int bossKind = 0, int visual = -1); // pool grow + GPU upload from enemyPoseScratch_
    Vec3f animatedViewmodelMuzzle(const AnimatedViewmodelRuntime& runtime, const Mat4& viewmodelXf,
                                  const char* preferredA, const char* preferredB) const;
    Vec3f activeWeaponMuzzle(const WeaponProfile& profile, const Mat4& viewmodelXf) const;
    bool loadObjMesh(const std::string& relPath, MeshAsset& out) const;
    bool loadTexture(const std::string& path, Texture& out) const;
    void generateMips(Texture& tex) const;
    bool ensureGpuResources(Engine& engine);
    void buildHud(UiDrawList& ui, int screenW, int screenH);
    // M4: the full-run ROUTE RAIL - the run as a row of biome-colored step nodes with a "you are
    // here" marker, drawn on the path/door choice screens so the route reads as a journey.
    void drawRouteRail(UiDrawList& ui, float cx, float topY, float s) const;
    // M7: the SYSTEMS field manual - a full-screen tactical-telemetry legend explaining the four
    // elements, the six affinities + set bonuses, the Pulse tiers, and the rarity tiers. Toggled
    // with [C] in any non-combat menu so a player can learn what everything is and does.
    void drawCodex(UiDrawList& ui, int screenW, int screenH) const;
    // Subtle hover/focus animation: a soft outer glow around the focused/hovered card or button that
    // grows in (menuFocusAnim_ eases 0->1) and gently breathes. Shared by every run-phase menu.
    void menuFocusHalo(UiDrawList& ui, float x, float y, float w, float h, float cut, uint32_t col, float s) const;

    void buildEnemyMesh();
    static void pushTri(std::vector<MeshTri3>& mesh, float ax, float ay, float az,
                        float bx, float by, float bz, float cx, float cy, float cz, int part);
    static void pushOcta(std::vector<MeshTri3>& mesh, float fwd, float back, float up, float dn, float side);
    static void pushEye(std::vector<MeshTri3>& mesh, float cx, float cy, float r, float tipx);
    EnemyStyle styleFor(EnemyKind kind) const;
    void spawnDebris(const Enemy& enemy, bool headshot);
    void updateDebris(float dt);

    Tunables tunables_{};
    StyleConfig style_{};   // W5 Neon Ink Brutalism locked style library (config/pulse.style)
    Player player_{};
    Weapon weapon_{};
    std::unordered_map<std::string, AnimatedViewmodelRuntime> weaponViewmodels_;
    WeaponProfileRegistry weaponProfiles_;
    // Meshy enemy visuals. Gameplay still uses four EnemyKind brains, while visual
    // picks select from the full generated concept roster.
    std::array<AnimatedEnemyModel, EnemyVisualCount> enemyVisuals_;
    static constexpr int BossKindCount = 3;
    std::array<AnimatedEnemyModel, BossKindCount> bossModels_; // one boss mesh per biome/moveset
    MaterialHandle sharedEnemyMaterial_ = MaterialHandle::Invalid; // obsidian + magenta-energy body (whole family)
    std::array<std::vector<AnimatedEnemyInstance>, EnemyVisualCount> enemyVisualPools_;
    std::array<std::vector<AnimatedEnemyInstance>, BossKindCount> bossPools_; // instance pools per boss model
    std::vector<AnimatedGltfSubmesh> enemyPoseScratch_;
    std::vector<AnimatedGltfSubmesh> enemyPoseScratchB_;   // second pose for animation cross-fades
    Texture wallTex_;
    Texture floorTex_;
    Texture ceilingTex_;
    Texture coverTex_;
    // Per-row scratch for floor/ceiling casting, rebuilt each frame (sized to h).
    std::vector<float> rowPerp_;
    std::vector<float> rowLightFloor_;
    std::vector<float> rowLightCeil_;
    std::vector<uint8_t> rowFog_;
    std::vector<int> rowMip_;
    std::array<std::vector<MeshTri3>, EnemyKindCount> enemyMeshes_;
    std::array<MeshAsset, EnemyKindCount> enemyMeshAssets_; // authored Blender drones (optional)
    std::vector<Debris> debris_;
    bool debugMuzzleMarker_ = false;
    std::vector<Enemy> enemies_;
    std::vector<EnemyCorpse> corpses_;  // transient death-animation bodies (visual only)
    std::vector<Enemy> pendingEnemies_; // boss-summoned adds, flushed after the enemy loop
    std::vector<Projectile> projectiles_;
    std::vector<Beam> beams_;
    std::vector<DamageMarker> damageMarkers_;
    std::vector<CombatText> combatTexts_;
    std::vector<Pickup> pickups_;
    std::vector<Tracer> tracers_;
    std::vector<EnemyBeamLine> enemyBeamLines_;
    std::vector<ScreenCasing> casings_;
    std::vector<Impact> impacts_;
    std::vector<Burst> bursts_;
    std::vector<Decal> decals_;   // persistent bullet marks + scorch (projected)
    std::vector<Decal> envDecals_;  // per-room biome floor markings (chevrons/code/sigils); rebuilt on room gen
    float ambientSpawnAccum_ = 0.0f;  // accumulator pacing per-biome ambient particle spawns
    std::vector<Decal> frameDecals_;   // per-frame: persistent decals + dynamic contact shadows under actors
    std::vector<WorldParticle> particles_;     // CPU-simulated spark/ember pool
    std::vector<Particle> particleRender_;      // engine billboards rebuilt each frame
    std::vector<Particle> enemySmokeRender_;    // alpha-blended shadow-smoke aura, rebuilt each frame
    std::vector<Particle> enemyCoreRender_;     // additive white-hot enemy core glows, rebuilt each frame

    // Heat-haze refraction sources (scene UV warp): energy orbs in flight + brief expanding
    // impact shocks. heatPulses_ is the CPU sim (ages out); heatRender_ is rebuilt each frame.
    struct HeatPulse { Vec3f pos{}; float age = 0.0f; float life = 0.42f; float power = 1.0f; };
    std::vector<HeatPulse>  heatPulses_;
    std::vector<HeatSource> heatRender_;
    // Boss nova / area-burst SHOCKWAVE: an expanding ground ring with a hard leading edge over a
    // soft fill (the spec's "crucible wave"), spawned when the boss commits a burst. Pure VFX -
    // uses no rng and feeds no gameplay, so the deterministic sim stays byte-identical; the live
    // line-of-fire hit is still the orbs themselves. Aged in updateParticles, drawn in buildFrame.
    struct NovaWave { Vec3f pos{}; float age = 0.0f; float life = 0.6f; Vec3f color{ 1.95f, 0.26f, 1.05f }; };
    std::vector<NovaWave> novaWaves_;
    Rng rng_{0x50554C53u};
    bool scriptedDeterministic_ = false;
    bool marketingBot_ = false;   // headless capture profile; normal QA/balance bot stays unchanged

    // Engine resources (created once via ensureGpuResources).
    bool gpuResourcesReady_ = false;
    MeshHandle gpuArenaFloorMesh_{}, gpuArenaCeilingMesh_{}, gpuArenaWallMesh_{};
    float roomFloorY_ = 0.0f;       // world Y of the arena floor (0 for the Quaternius MegaKit)
    // The Quaternius brutalist arena is THE one and only environment (no mode flags).
    Wasteland wasteland_;
    int   forcedBiome_ = -1;         // M4 dev override: -1 = follow sector; >=0 forces the biome (capture)
    std::string forcedRoomName_;     // dev/QA override: non-empty forces a named template (capture)
    bool  topDownCapture_ = false;   // dev/QA override: near-overhead camera (inspect layout/decals/dressing)
    Biome currentBiome_ = Biome::Rocky;   // which outdoor biome this room is (drives the palette)
    // The procedural arena (the Quaternius brutalist Wasteland) behind one interface, so the
    // collision / draw / spawn sites stay environment-agnostic.
    bool procEnvReady() const { return wasteland_.ready(); }
    bool procEnvOutdoor() const { return wasteland_.ready(); }
    const std::array<std::string, 24>& procEnvGrid() const { return wasteland_.grid(); }
    int  procEnvSub() const { return wasteland_.subRes(); }
    bool procEnvSolidFine(int fx, int fz) const { return wasteland_.solidFineCell(fx, fz); }
    float procEnvFineHeight(int fx, int fz) const { return wasteland_.solidFineHeight(fx, fz); }
    const std::vector<DungeonDraw>& procEnvDraws() const { return wasteland_.draws(); }
    int  procEnvSpawnX() const { return wasteland_.spawnX(); }
    int  procEnvSpawnZ() const { return wasteland_.spawnZ(); }
    MeshHandle gpuPickupHealthMesh_{}, gpuPickupShieldMesh_{}, gpuPickupAmmoMesh_{};
    MeshHandle gpuTracerMesh_{}, gpuProjectileMesh_{};
    MeshHandle gpuOrbSmoothMesh_{}, gpuOrbShardMesh_{};  // per-kind enemy orb silhouettes (spike = gpuProjectileMesh_)
    MeshHandle gpuObjectiveMesh_{}; // cyan crystal objective (slice); enemy cores are now soft additive glows
    MeshHandle gpuEnergyOrbMesh_{};                      // smooth glowing sphere: enemy projectiles + charge-up (Returnal-style orb)
    MeshHandle gpuBeamMesh_{};                           // unit cylinder along +Z: solid lock-on beam (scaled + oriented per draw)
    // Meshy hero env GLBs (W9): bound to W5 materials, brutalist arena only. Invalid =
    // fall back to the procedural form. gpuCrystalGlbMesh_ swaps the objective orb.
    MeshHandle gpuCrystalGlbMesh_{}, gpuMonumentMesh_{}, gpuGatewayMesh_{};
    MaterialHandle matObsidianHero_{};                  // W5 polished-obsidian for the hero env props
    Vec3f      objectivePos_{};                          // objective world pos (set in buildFrame, lit in the lights pass)
    std::array<MeshHandle, EnemyKindCount> gpuEnemyMeshes_{};
    // M2.5 prop meshes (procedural crate / barrel / pillar / glowing panel).
    MeshHandle gpuCrateMesh_{}, gpuBarrelMesh_{}, gpuPillarMesh_{}, gpuPanelMesh_{};
    // Materials: floor/ceiling/wall/weapon are textured; dynamic props use a plain
    // untextured material plus per-instance tint + emissive.
    MaterialHandle matFloor_{}, matCeiling_{}, matWall_{}, matUntextured_{};
    // M2.5 named PBR material library for props (built from the CC0 sets + scalars).
    MaterialHandle matCrate_{}, matBarrel_{}, matPillar_{}, matPanel_{};

    // M2.5 a placed decorative prop. Solid: blocks player/enemy movement and shots
    // (a circular footprint of radius, up to height; a high enough shot clears it).
    struct Prop { Vec3f pos; float yaw; float scale; int kind; Vec3f tint; float emissive;
                  float radius; float height; };
    std::vector<Prop> props_;
    void buildArenaProps();       // populate props_ for the active arena
    void regenerateArena();     // rebuild the arena layout + spawn for the current room
    const std::array<std::string, 24>& activeMap() const;   // active arena grid
    // Nearest prop the ray (origin, unit dir, rising at verticalSlope from originHeight)
    // enters within maxDist; returns the entry distance and sets outProp, else maxDist.
    float propRayHit(Vec2 origin, Vec2 dir, float maxDist, float originHeight,
                     float verticalSlope, int& outProp) const;

    // Per-frame submission storage (referenced by the returned SceneFrame).
    SceneFrame frame_{};
    std::vector<MeshInstance> instances_;
    std::vector<LocalLight>   lights_;
    std::optional<UiDrawList> ui_;

    int score_ = 0;
    int bestScore_ = 0;

    // Phase A run/room state machine. phase_ drives update(); run_ holds the seeded
    // room/sector sequence + the dedicated run-RNG. Wave bookkeeping paces spawns
    // within a room; restartTimer_ doubles as the RunOver dwell.
    RunPhase phase_ = RunPhase::InRoom;
    Run run_{};
    Build build_{};                       // stacking passive inventory + aggregated stats
    Meta meta_{};                         // persistent meta-currency + content unlocks
    // Run-scoped modifiers (RunMods foundation). Deals push into activeMods_ mid-run;
    // forcedMods_ (and, in M4, the heat table) seed it every run; recomputeMods() folds
    // it into mods_, the combat-ready aggregate read at the spawn/damage/heal/reward sites.
    std::vector<RunModifier> activeMods_;
    std::vector<RunModifier> forcedMods_;
    int forcedHeat_ = -1;   // balance-sim heat override (>=0 wins over meta_.heat())
    RunMods mods_{};
    std::vector<std::string> rewardOptions_; // current 1-of-3 reward card ids (RoomCleared)
    // Spatial doors (RunPhase::DoorsOpen). One bind per next-room option, mapped to an open exit
    // door in the current arena; walking into its trigger commits the reward + that route.
    struct DoorBind {
        int         optionIndex = 0;    // -> run_.chooseOption / run_.currentOptions()[i]
        int         envDoorIndex = 0;   // -> wasteland_.doors()[envDoorIndex] (geometry + collision)
        RoomType    destType = RoomType::Combat;
        std::string rewardId;           // reward previewed/granted at this door ("" = none, e.g. Shop)
        Vec2        triggerCenter{};     // world XZ of the door (overlap-tested vs player_.pos)
        float       triggerRadius = 1.2f;
        bool        open = false;
    };
    std::vector<DoorBind> doors_;
    bool  autoResolveDoors_ = false;     // headless: commit the bot's door pick instantly (no walking)
    int   pendingDoorOption_ = -1;       // door chosen, awaiting the fade midpoint to load
    float doorFadeTimer_ = 0.0f;         // counts DOWN through the transition fade (0 = idle)
    float doorFadeDuration_ = 0.55f;     // total fade time (Returnal-style quick load)
    bool  doorFadeCommitted_ = false;    // beginRoom() already fired at the midpoint
    // Per-doorway slide animation: 0 = closed (the two door leaves meet, sealing the gap), 1 = fully
    // open (leaves retracted into the flanking wall). Eased each frame toward the wasteland door's
    // open flag so combat doors visibly slide apart when the room clears. Indexed by wasteland door.
    std::vector<float> doorAnim_;
    // Feature 2 in-run economy: scrap is a per-run currency (never persisted) earned from
    // kills and spent in Shop rooms. scrapFlash* drives the brief "+N" HUD pop on a kill.
    int scrap_ = 0;
    float scrapFlashTimer_ = 0.0f;
    int scrapFlashAmount_ = 0;
    // Shop state (RunPhase::Shop): stock ids (prefixed like rewardOptions_), parallel
    // prices, a sold flag per slot, and a reroll counter (cost rises each use).
    std::vector<std::string> shopStock_;
    std::vector<int> shopPrices_;
    std::vector<uint8_t> shopSold_;
    int shopRerollCount_ = 0;
    // Feature 3 deals (RunPhase::Event): the 1-2 rolled deals (indices into the file-local
    // catalog), and sector-scoped curses to expire when the sector advances (sourceId +
    // the sector they were applied in).
    std::vector<int> eventDeals_;
    std::vector<std::pair<std::string, int>> sectorCurses_;
    // Loadout: the active weapon's ammo lives in weapon_; inactive slots park theirs
    // in savedAmmo. activeWeapon_ indexes loadout_ (always starts with the pistol).
    struct WeaponSlot { std::string id = "pistol"; int power = 1; int aspect = 0; Weapon savedAmmo{}; };
    std::vector<WeaponSlot> loadout_;
    int activeWeapon_ = 0;
    int lastPrimaryWeapon_ = -1;   // last non-pistol slot, for the V quick-draw toggle
    // Ability charges (0..1) fill from aggression; overdriveTimer_ is the active ult window.
    float tacticalCharge_ = 0.0f;
    float ultimateCharge_ = 0.0f;
    float overdriveTimer_ = 0.0f;
    WaveSpec activeWave_{};       // composition/cadence of the wave currently spawning
    int waveIndex_ = 0;          // index of the active wave within the current room
    int waveSpawnsLeft_ = 0;     // enemies still to spawn in the active wave
    float waveSpawnTimer_ = 0.0f;
    float phaseTimer_ = 0.0f;    // RoomCleared banner dwell
    // Card-screen focus cursor (ChoosePath / Reward / Event): arrow/WASD move it,
    // Enter/Space commits it, number keys still jump+commit. Reset on phase change.
    int cardCursor_ = 0;
    RunPhase cardCursorPhase_ = RunPhase::Hub;
    // Immediate-mode menu clicks + hover animation, shared by the run-phase menus (Reward / Path /
    // Shop / Event / RunOver). buildHud publishes a clickable rect per element each frame; the
    // per-phase input reads them (hover -> focus, click -> commit). menuFocusAnim_ eases 0->1 after
    // the focused element changes so the focused/hovered element gets a subtle pop + breathing glow.
    struct MenuHit { float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; int id = 0; };
    std::vector<MenuHit> menuHits_;
    int   menuFocusKey_ = -1;
    float menuFocusAnim_ = 0.0f;
    bool  menuSliderDrag_ = false;   // an Options slider is being click-dragged this frame
    // Clickable action ids beyond the 0..n-1 card slots.
    enum { MenuIdHeal = 100, MenuIdReroll = 101, MenuIdForge = 102, MenuIdLeave = 103,
           MenuIdDecline = 104, MenuIdContinue = 105 };
    bool codexOpen_ = false;       // M7: the SYSTEMS field-manual overlay (legend for elements/affinities/Pulse/tiers)
    int  codexTab_ = 0;            // active field-manual tab (0..8: PULSE/AFFINITIES/STATUSES/SETS/RARITY/HEAT/PACTS/WEAPONS/ROUTES)
    // Tab hit-rects, written by drawCodex (const) and read by the codex input handler so clicking a
    // tab works with the variable-width labels (one-frame-lagged layout, which is imperceptible).
    mutable std::array<float, 9> codexTabX_{};
    mutable std::array<float, 9> codexTabW_{};
    mutable float codexTabBarY_ = 0.0f;
    mutable float codexTabBarH_ = 0.0f;
    int runCount_ = 0;           // runs begun; first run (0) is deterministic
    bool runWon_ = false;        // RunOver: win vs death (drives the HUD banner)
    int lastPayout_ = 0;         // meta earned by the last run (shown on RunOver/Hub)
    int roomsClearedTotal_ = 0;  // cumulative test telemetry (not reset per run)
    int runsEndedTotal_ = 0;
    int furthestSector_ = 0;     // deepest sector reached this session (Main Menu FURTHEST stat)
    std::string felledBy_;       // name of the enemy that ended the last run (Run Ended report)
    int bossesReachedTotal_ = 0;
    int runsWonTotal_ = 0;
    std::array<int, 6> roomTypeCounts_{}; // cumulative rooms entered by RoomType (M5 telemetry)
    // M1 Pulse telemetry (sim batch, sampled each in-combat frame; never reset per run).
    long long pulseSampleFrames_ = 0;
    double pulseMeterSum_ = 0.0;
    std::array<long long, 5> pulseTierFrames_{};   // frames in each PulseTier (Cold..Overpulse)
    // M2 status telemetry: enemy-frames carrying any element (uptime) over the sim batch.
    long long statusEnemyFrames_ = 0;
    long long statusActiveFrames_ = 0;

    float spawnTimer_ = 0.5f;
    float pickupSpawnTimer_ = 2.5f;
    float restartTimer_ = 0.0f;
    float hitmarkerTimer_ = 0.0f;
    float precisionMarkerTimer_ = 0.0f; // distinct gold hitmarker window after a headshot
    float killConfirmTimer_ = 0.0f;
    float damageFlashTimer_ = 0.0f;
    float shieldFlashTimer_ = 0.0f;
    float lifeLeechFlashTimer_ = 0.0f;
    std::array<float, 6> elementFeedbackCooldown_{};
    float dashInvulnTimer_ = 0.0f;     // >0 = dash i-frames active (all damage negated)
    float muzzleFlashTimer_ = 0.0f;
    float fireFovKick_ = 0.0f;
    float fireCameraKick_ = 0.0f;
    float recoilPitch_ = 0.0f;
    float recoilResidualPitch_ = 0.0f;
    // CS2-style recoil: a deterministic spray offset added to the aim for the
    // view AND the shot, accumulated along a fixed pattern while firing and
    // recovered once the trigger is released. The player counters it by dragging.
    float recoilOffsetPitch_ = 0.0f;
    float recoilOffsetYaw_ = 0.0f;
    int recoilShotIndex_ = 0;
    int fireSoundIndex_ = 0;   // free-running, walks the weapon's fire-sample series
    int enemySoundIndex_ = 0;  // free-running, walks enemy attack/body round-robin banks
    int feedbackSoundIndex_ = 0; // free-running, walks the player-feedback round-robin banks
    bool lowHealthLatched_ = false; // edge-triggers the low-health warning cue once per dip
    float weaponKick_ = 0.0f;
    float weaponKickSide_ = 0.0f;
    float cameraBobPhase_ = 0.0f;
    float landingKick_ = 0.0f;
    float strafeLean_ = 0.0f;
    float combatIntensity_ = 0.0f;  // music-driver value, now derived from the Pulse meter each frame
    Pulse pulse_;                   // M1: the momentum mechanic (drives grants + loot greed + music)
    bool  pendingBossIntroStinger_ = false;
    bool  musicOverpulseLatched_ = false;
    float musicOverpulseCooldown_ = 0.0f;
    // v4 reactive-tension music driver state.
    bool  bossPhaseLatched_ = false;       // BossPhase stinger edge latch (escalation >= 0.5)
    bool  bossEnrageLatched_ = false;      // BossEnrage stinger edge latch (escalation >= 0.8)
    float musicBossEscCooldown_ = 0.0f;    // shared cooldown so the crossings do not retrigger
    std::string musicTracePath_;           // S3: per-frame music-context CSV path (empty = off)
    double musicTraceTime_ = 0.0;          // S3: trace clock (seconds since trace start)
    bool  musicTraceInit_ = false;         // S3: header written + file truncated on first row
    float lifeLeechCarry_ = 0.0f;
    float roomPulsePeak_ = 0.0f;    // highest Pulse reached this room -> greed loot bias on clear
    float runPulsePeak_ = 0.0f;     // highest Pulse this run -> the run-over report stat
    float cameraShake_ = 0.0f;   // current shake energy in degrees, decays each frame
    float shakeTime_ = 0.0f;     // free-running clock driving the shake waveform
    float hitStopTimer_ = 0.0f;  // remaining hit-stop window in real seconds
    float renderViewYaw_ = 0.0f;   // player yaw + shake, used for drawing only
    float renderViewPitch_ = 0.0f; // player pitch + shake, used for drawing only
    // The viewmodel barrel tip projected to screen fractions each frame, so the
    // muzzle flash + tracer originate from the real muzzle (tracking kick/sway).
    float muzzleFracX_ = 0.57f;
    float muzzleFracY_ = 0.62f;
    float configMessageTimer_ = 0.0f;
    std::string configMessage_;

    // Front-end shell state (interactive windowed play only; see enterMainMenu).
    Settings   settings_{};
    bool       frontEnd_ = false;       // the shell is enabled (set by enterMainMenu)
    bool       settingsActive_ = false; // settings have been loaded + applied to tunables_
    bool       wantsQuit_ = false;      // user chose Quit to desktop
    MenuScreen menuScreen_ = MenuScreen::None;
    MenuScreen settingsReturn_ = MenuScreen::Main; // where Options returns to (Main/Pause)
    int        menuSel_ = 0;            // highlighted row on the active screen
    int        menuTab_ = 0;            // Options: active tab (Audio/Controls/Video/Accessibility/Gameplay)
    int        pendingPauseConfirm_ = -1; // destructive pause row awaiting a second confirm
    bool       settingsDirty_ = false;  // Options: unsaved changes since the screen opened
    float      baseSensitivity_ = 0.0022f; // config mouse sensitivity; settings multiply it
};

} // namespace pulse

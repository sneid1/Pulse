#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Input.hpp"
#include "Engine/GpuSceneRenderer.hpp"
#include "Engine/Math.hpp"
#include "Game/Tunables.hpp"

namespace pulse {

class AudioSystem;
class Renderer;

class PulseGame {
public:
    PulseGame();

    bool loadConfig(bool announce = false);
    void buildBotInput(InputState& input, float elapsedSeconds) const;
    void debugBeginScriptedCapture();
    void debugPose();    // place a couple of drones in front for a close-up screenshot
    void debugKillAll(); // shatter all active drones (to inspect death FX)
    void debugFire(AudioSystem& audio, float pitch); // fire a wall tracer at a set pitch
    void debugRenderWeaponPreview(Renderer& renderer);
    void update(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH);
    void render(Renderer& renderer);

    const Tunables& tunables() const { return tunables_; }
    int score() const { return score_; }
    int bestScore() const { return bestScore_; }
    int playerHp() const { return player_.hp; }
    int playerShield() const { return player_.shield; }
    int activeEnemyCount() const;
    int weaponMeshVertexCount() const { return static_cast<int>(weaponMeshVertices_.size()); }
    int weaponMeshTriangleCount() const { return static_cast<int>(weaponMeshTriangles_.size()); }
    int weaponTextureWidth() const { return weaponTextureWidth_; }
    int weaponTextureHeight() const { return weaponTextureHeight_; }

private:
    enum class EnemyKind {
        Rusher,
        Ranged,
        Tank
    };
    static constexpr int EnemyKindCount = 3;

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
        Vec2 dashDir{};
        float height = 0.5f; // eye height above the floor (0..1 room height)
        float vz = 0.0f;     // vertical velocity for jumping/falling
        bool grounded = true;
        bool dead = false;
    };

    struct Weapon {
        int ammo = 30;
        int reserve = 90;
        float timeSinceShot = 99.0f;
        float reloadRemaining = 0.0f;
        bool reloading = false;
    };

    struct Enemy {
        EnemyKind kind = EnemyKind::Rusher;
        Vec2 pos{};
        Vec2 vel{};
        float health = 100.0f;
        float hurtTimer = 0.0f;
        float telegraphRemaining = 0.0f;
        float attackCooldown = 0.0f;
        float bobPhase = 0.0f;  // per-enemy hover offset so the swarm isn't in lockstep
        float hitPunch = 0.0f;  // brief scale-up on taking a hit, for impact
        float lungeTime = 0.0f; // rusher: remaining time in an active lunge burst
        float recover = 0.0f;   // post-attack recovery: can't act until it elapses
        bool struck = false;    // melee wind-up has resolved this attack
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
    };

    // A fading wedge drawn at the screen edge pointing toward a damage source,
    // so a hit always reads as coming "from there", never out of nowhere.
    struct DamageMarker {
        float worldAngle = 0.0f; // atan2 of (source - player) at the moment of the hit
        float age = 0.0f;
        float life = 1.5f;
        float intensity = 1.0f;
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
    };

    struct Impact {
        Vec2 pos{};
        float height = 0.5f;
        float age = 0.0f;
        float duration = 0.34f;
        bool hit = false;
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
    void updatePlayer(const InputState& input, AudioSystem& audio, float dt);
    void updateWeapon(const InputState& input, AudioSystem& audio, float dt, int screenW, int screenH);
    void updateEnemies(AudioSystem& audio, float dt);
    void updateProjectiles(AudioSystem& audio, float dt);
    void updateSpawning(float dt);
    void updatePickups(AudioSystem& audio, float dt);
    void updateTimers(float dt);
    void tryFire(AudioSystem& audio, int screenW, int screenH);
    int acquireTarget(float shotYaw, float shotPitch, int screenW, int screenH, bool& outHeadshot) const;
    Vec2 separationForce(const Enemy& self) const;
    void spawnProjectile(Vec2 from, float fromHeight, Vec2 dir, int damage);
    void addDamageMarker(Vec2 source, float intensity);
    void damagePlayer(AudioSystem& audio, int amount, Vec2 source);
    void spawnEnemy();
    void spawnPickup(PickupKind kind);
    void removeDeadEnemies();
    void addShake(float degrees);
    void spawnBurst(Vec2 pos, bool headshot);
    void spawnImpact(Vec2 pos, float height, bool hit);

    bool isWallCell(int x, int y) const;
    bool collides(Vec2 pos, float radius) const;
    void moveWithCollision(Vec2& pos, Vec2& vel, float radius, float dt) const;
    bool lineOfSight(Vec2 from, Vec2 to) const;
    RayHit castRay(Vec2 origin, float angle, float maxDistance) const;
    Projection projectEnemy(const Enemy& enemy, float yaw, float pitch, int screenW, int screenH) const;
    Projection projectPoint(Vec2 point, float yaw, float pitch, int screenW, int screenH, float size) const;
    bool loadWeaponMesh();
    bool loadObjMesh(const std::string& relPath, MeshAsset& out) const;
    bool loadHandsMesh();
    bool loadWeaponTexture();
    bool loadTexture(const std::string& path, Texture& out) const;
    void generateMips(Texture& tex) const;
    bool renderGpuFrame(Renderer& renderer);
    bool ensureGpuResources();

    void buildEnemyMesh();
    static void pushTri(std::vector<MeshTri3>& mesh, float ax, float ay, float az,
                        float bx, float by, float bz, float cx, float cy, float cz, int part);
    static void pushOcta(std::vector<MeshTri3>& mesh, float fwd, float back, float up, float dn, float side);
    static void pushEye(std::vector<MeshTri3>& mesh, float cx, float cy, float r, float tipx);
    EnemyStyle styleFor(EnemyKind kind) const;
    void spawnDebris(const Enemy& enemy, bool headshot);
    void updateDebris(float dt);
    void renderHud(Renderer& renderer);

    Tunables tunables_{};
    Player player_{};
    Weapon weapon_{};
    std::vector<MeshVertex> weaponMeshVertices_;
    std::vector<MeshUv> weaponMeshUvs_;
    std::vector<MeshNormal> weaponMeshNormals_;
    std::vector<MeshTriangle> weaponMeshTriangles_;
    MeshAsset leftHandMesh_;
    MeshAsset rightHandMesh_;
    std::vector<uint32_t> weaponTexture_;
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
    MeshVertex weaponMeshCenter_{};
    int weaponTextureWidth_ = 0;
    int weaponTextureHeight_ = 0;
    float weaponMeshMinZ_ = 0.0f;
    MeshVertex weaponMuzzleVertex_{};
    bool weaponMeshLoaded_ = false;
    bool debugMuzzleMarker_ = false;
    std::vector<Enemy> enemies_;
    std::vector<Projectile> projectiles_;
    std::vector<DamageMarker> damageMarkers_;
    std::vector<Pickup> pickups_;
    std::vector<Tracer> tracers_;
    std::vector<Impact> impacts_;
    std::vector<Burst> bursts_;
    Rng rng_{0x50554C53u};
    bool scriptedDeterministic_ = false;
    std::unique_ptr<GpuSceneRenderer> gpuRenderer_;
    bool gpuResourcesReady_ = false;
    uint32_t gpuArenaFloorMesh_ = 0;
    uint32_t gpuArenaCeilingMesh_ = 0;
    uint32_t gpuArenaWallMesh_ = 0;
    uint32_t gpuWeaponMesh_ = 0;
    uint32_t gpuLeftHandMesh_ = 0;
    uint32_t gpuRightHandMesh_ = 0;
    uint32_t gpuWallTexture_ = 0;
    uint32_t gpuFloorTexture_ = 0;
    uint32_t gpuCeilingTexture_ = 0;
    uint32_t gpuWeaponTexture_ = 0;
    uint32_t gpuPickupHealthMesh_ = 0;
    uint32_t gpuPickupShieldMesh_ = 0;
    uint32_t gpuPickupAmmoMesh_ = 0;
    uint32_t gpuTracerMesh_ = 0;
    uint32_t gpuProjectileMesh_ = 0;
    std::array<uint32_t, EnemyKindCount> gpuEnemyMeshes_{};

    int score_ = 0;
    int bestScore_ = 0;
    float spawnTimer_ = 0.5f;
    float pickupSpawnTimer_ = 2.5f;
    float restartTimer_ = 0.0f;
    float hitmarkerTimer_ = 0.0f;
    float killConfirmTimer_ = 0.0f;
    float damageFlashTimer_ = 0.0f;
    float shieldFlashTimer_ = 0.0f;
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
    float cameraBobPhase_ = 0.0f;
    float landingKick_ = 0.0f;
    float strafeLean_ = 0.0f;
    float combatIntensity_ = 0.0f;
    float cameraShake_ = 0.0f;   // current shake energy in degrees, decays each frame
    float shakeTime_ = 0.0f;     // free-running clock driving the shake waveform
    float hitStopTimer_ = 0.0f;  // remaining hit-stop window in real seconds
    float renderViewYaw_ = 0.0f;   // player yaw + shake, used for drawing only
    float renderViewPitch_ = 0.0f; // player pitch + shake, used for drawing only
    float configMessageTimer_ = 0.0f;
    std::string configMessage_;
};

} // namespace pulse

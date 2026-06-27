#pragma once

namespace pulse {

struct Tunables {
    int windowWidth = 1920;
    int windowHeight = 1080;

    // Run pacing. Each biome/sector rolls: at least this many rooms, plus 0..extra,
    // then the boss. The fixed opener counts as one of these rooms.
    int runMinRoomsBeforeBoss = 5;
    int runExtraRoomsBeforeBoss = 2;

    float cameraFovDegrees = 95.0f;
    float mouseSensitivity = 0.0022f;
    float pitchLimitDegrees = 85.0f;   // near-straight up/down; kept <= the ~86 deg hitscan/render clamp so aim stays consistent
    float fireImpactRecovery = 18.0f;

    // Screen shake is render-only: it perturbs the rendered view, never the
    // aim ray, so "clean" gunplay (spec section 5) is preserved. Magnitudes are
    // in degrees of view kick; decay is an exponential settle rate per second.
    float cameraShakeFire = 0.72f;
    float cameraShakeKill = 1.10f;
    float cameraShakeHurt = 2.4f;
    float cameraShakeDecay = 13.0f;
    float cameraBobDegrees = 0.10f;
    float cameraBobSpeed = 9.0f;
    float cameraLandingKickDegrees = 0.85f;
    float cameraStrafeLeanDegrees = 0.55f;

    // Reactive crosshair: the reticle gap (px) grows with movement, accumulated
    // recoil, and each shot so the player reads their current accuracy state
    // (CS/COD/Valorant staple), then tightens to base when still and not firing.
    float crosshairBaseGap = 6.0f;
    float crosshairBloomScale = 14.0f;

    // Distance fog density for the textured world. Higher = walls/floor fade to
    // the dark fog colour sooner, which reads as a moodier, deeper space.
    float renderFogDensity = 0.05f;

    // Hit-stop: a micro freeze on a kill for crunch (spec section 5). During the
    // window gameplay time is scaled by hitstopScale; feedback/audio stay live.
    float feelHitstopKill = 0.045f;
    float feelHitstopScale = 0.08f;
    // Precision (headshot) crunch: the kill hit-stop is multiplied by this so a
    // crit kill freezes a touch longer than a body kill.
    float feelHitstopPrecisionMult = 1.6f;
    // Per-connect micro hit-stop: the base freeze when a SHOT LANDS (not just a kill),
    // scaled by the weapon's impactScale and reduced for automatics. This is the main
    // "weight behind the shots" lever -- each hit briefly bites the world.
    float feelHitstopHit = 0.030f;
    // Lifetime of the expanding impact burst spawned where an enemy dies.
    float feelKillBurstSeconds = 0.30f;
    float feelImpactHitSeconds = 0.50f;
    float feelImpactWallSeconds = 0.62f;
    // Backward shove an enemy gets when shot, and the outward speed of the
    // shards a drone shatters into when destroyed.
    float feelHitKnockback = 5.0f;
    float feelDebrisSpeed = 5.5f;

    float walkSpeed = 7.0f;
    float acceleration = 135.0f;
    float braking = 120.0f;
    float airControl = 1.0f;
    float playerRadius = 0.28f;

    int playerMaxHealth = 100;
    int playerStartHealth = 100;
    int playerMaxShield = 75;
    int playerStartShield = 25;

    float pickupSpawnInterval = 5.5f;
    int pickupMaxActive = 4;
    int pickupHealthAmount = 30;
    int pickupShieldAmount = 40;
    int pickupAmmoAmount = 60;   // reserve rounds restored per ammo crate
    float pickupCollectRadius = 0.55f;

    float dashImpulse = 10.0f;
    float dashDuration = 0.10f;
    float airDashMult = 1.4f;   // a dash started in the AIR covers this much more distance (air mobility)
    float dashCooldown = 1.0f;
    float dashFovPunch = 1.4f;
    // Invincibility window granted by a dash (a dodge tool). Runs a touch longer than
    // the dash movement so the escape feels generous; all incoming damage is negated
    // while it is active. 0 disables i-frames.
    float dashInvulnSeconds = 0.20f;

    // Jump/gravity. Heights are fractions of the 1-unit room height; eye sits at
    // eyeHeight when grounded and rises on a jump until gravity pulls it back.
    float eyeHeight = 0.78f;
    float jumpVelocity = 12.0f; // ground hop: clears low cover (apex ~ velocity^2 / 2*gravity)
    float gravity = 40.0f;      // rise gravity (snappy, not floaty); see fall/low-jump mults below
    float jumpApexCap = 0.9f; // indoor clamp so the eye never punches through the ceiling
    // Jump arc FEEL: the single jump should be snappy + weighty + controllable, not a floaty hang.
    //  - fallGravityMult: you fall FASTER than you rise (the classic "good jump" weight).
    //  - lowJumpMult: releasing jump early while rising cuts it short -> VARIABLE jump height
    //    (tap = small hop, hold = full jump).
    float fallGravityMult = 1.5f;
    float lowJumpMult = 2.2f;
    // Proper jump ability: a mid-air DOUBLE JUMP (arena movement), plus coyote time + input
    // buffering so the jump feels responsive rather than dropping inputs at the exact ground frame.
    int   airJumpCount = 1;        // extra jumps available in the air before landing (1 = double jump)
    float airJumpVelocity = 11.0f; // each air jump RESETS vertical speed to this (clean second boost)
    float coyoteTime = 0.10f;      // still allowed a ground jump for this long after walking off
    float jumpBufferTime = 0.12f;  // a jump pressed this soon before you can jump still fires

    bool weaponAutoFire = true;
    float weaponFireRate = 10.0f;            // AK-family full-auto cadence: 600 RPM
    float weaponDamage = 30.0f;
    float weaponSpreadDegrees = 0.02f;       // extra standing inaccuracy, kept tiny for learnable sprays
    float weaponFirstShotInaccuracyDegrees = 0.08f;
    float weaponSprayInaccuracyDegrees = 0.20f;
    float weaponMoveInaccuracyDegrees = 1.35f;
    float weaponRecoilPitchDegrees = 0.92f;  // scales the vertical AK-style spray path
    float weaponRecoilYawJitterDegrees = 0.46f; // scales the horizontal spray path
    float weaponRecoilRecoveryRate = 10.5f;
    float weaponRecoilResidualFraction = 0.16f; // settles between bursts
    float weaponRecoilResetSeconds = 0.34f;
    int weaponMagazineCapacity = 30;
    int weaponReserveAmmo = 90;
    float weaponReloadDuration = 1.35f;
    float weaponFireFovPunch = 4.8f;
    float weaponFireCameraKick = 4.9f;
    // Gunshot mix loudness (passed to AudioSystem::playFire). The mixer soft-clips
    // the master, so higher values push more of the shot's body toward the ceiling
    // (louder/punchier), at the cost of saturating the transient. F5 hot-tunable.
    float weaponFireVolume = 5.0f;
    float weaponViewmodelFovDegrees = 74.0f;
    float pistolViewmodelFovDegrees = 66.0f;
    float pistolIdleSwayScale = 0.35f;
    float weaponViewmodelSway = 1.0f;
    float weaponViewmodelKickScale = 1.0f;
    float weaponViewmodelKickRecovery = 18.0f;
    // Screen-space muzzle origin (fractions of width/height) for viewmodel
    // flash and the visible HUD-layer bullet tracer.
    float weaponMuzzleScreenX = 0.57f;
    float weaponMuzzleScreenY = 0.62f;

    float enemyMaxHealth = 135.0f;
    float enemyHeadshotMinFraction = 0.34f;
    float enemyRusherSpeed = 4.0f;
    float enemyRangedSpeed = 3.0f;
    int enemyRangedDamage = 18;
    float enemyRangedCooldown = 2.6f;
    float enemyRangedTelegraph = 0.8f;

    // Melee enemies (rusher/tank) wind up a telegraphed strike at contact range
    // instead of instantly deleting themselves on touch. The wind-up is the
    // player's window to dash clear, so the damage never lands "out of nowhere".
    float enemyMeleeRange = 1.7f;
    int enemyMeleeDamage = 28;
    float enemyMeleeTelegraph = 0.45f;
    float enemyMeleeRecover = 1.0f;
    float enemyMeleeLunge = 1.4f;   // forward step the strike commits

    // Ranged enemies fire a slow, visible, dodgeable orb rather than hitscan, so
    // there is always a projectile to read and evade.
    float enemyProjectileSpeed = 10.0f;   // > walkSpeed (7) so orbs read as INCOMING, not lingering
    float enemyProjectileRadius = 0.30f;
    float enemyProjectileLife = 2.6f;     // short range (~26m) so spent orbs do not become far-field soup

    // Every enemy kind can open fire with a dodgeable energy bolt out to this range.
    // Melee kinds still prefer their lunge/slam once inside their own attack range;
    // beyond it (but within shoot range) they fall back to shooting.
    float enemyShootRange = 15.0f;

    // Soft boid separation so the swarm spreads out and stays individually
    // readable instead of stacking into one blob.
    float enemySeparationRadius = 1.4f;
    float enemySeparationStrength = 9.0f;

    // Type C tank: a slow, high-health brute that forces target prioritisation.
    float enemyTankHealthMult = 3.6f;
    float enemyTankSpeed = 2.2f;
    int enemyTankMeleeDamage = 42;
    float enemyTankScale = 0.82f;
    // Rusher lunge: a telegraphed burst of speed when it closes in.
    float enemyRusherLungeRange = 6.5f;
    float enemyRusherLungeSpeed = 11.0f;
    // Stalker: a flanking ghoul that zigzags, then commits to a shorter pounce.
    float enemyStalkerSpeed = 4.8f;
    float enemyStalkerPounceRange = 5.2f;
    float enemyStalkerPounceSpeed = 13.0f;

    // On-screen directional hit indicator: how long a damage wedge lingers.
    float damageIndicatorSeconds = 1.5f;

    float spawnInterval = 0.85f;
    int spawnMaxConcurrent = 8;
    float spawnRingRadius = 14.0f;
    float spawnRangedChance = 0.30f;
    float spawnTankChance = 0.18f;
    float spawnStalkerChance = 0.16f;

    bool technoEnabled = true;
    float technoBpm = 140.0f;   // SneidGame-style hard techno tempo
    float technoBaseVolume = 0.26f;
    // v4 reactive-tension music. Off => the game drives music via the legacy (v3) setter: no
    // duress submerge/heartbeat, immediate transitions, hard-full boss, full-band duck. On => the
    // game feeds a MusicContext (duress from HP, boss escalation), quantized transitions, tilted
    // duck. Hot-reloadable via F5 (config key music.v4) so it can be A/B'd by feel and shipped dark.
    bool musicV4 = true;
};

} // namespace pulse

#pragma once

namespace pulse {

struct Tunables {
    int windowWidth = 1280;
    int windowHeight = 720;

    float cameraFovDegrees = 95.0f;
    float mouseSensitivity = 0.0022f;
    float pitchLimitDegrees = 42.0f;
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

    // Distance fog density for the textured world. Higher = walls/floor fade to
    // the dark fog colour sooner, which reads as a moodier, deeper space.
    float renderFogDensity = 0.05f;

    // Hit-stop: a micro freeze on a kill for crunch (spec section 5). During the
    // window gameplay time is scaled by hitstopScale; feedback/audio stay live.
    float feelHitstopKill = 0.045f;
    float feelHitstopScale = 0.08f;
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

    float dashImpulse = 16.0f;
    float dashDuration = 0.12f;
    float dashCooldown = 1.0f;
    float dashFovPunch = 1.4f;

    // Jump/gravity. Heights are fractions of the 1-unit room height; eye sits at
    // eyeHeight when grounded and rises on a jump until gravity pulls it back.
    float eyeHeight = 0.5f;
    float jumpVelocity = 4.4f;
    float gravity = 27.0f;
    float jumpApexCap = 0.9f; // clamp so the eye never punches through the ceiling

    bool weaponAutoFire = true;
    float weaponFireRate = 9.7f;             // compact carbine cadence: chunky, deliberate
    float weaponDamage = 30.0f;
    float weaponSpreadDegrees = 1.05f;
    float weaponRecoilPitchDegrees = 0.86f;  // hard climb you can ride and control
    float weaponRecoilYawJitterDegrees = 0.22f;
    float weaponRecoilRecoveryRate = 10.5f;
    float weaponRecoilResidualFraction = 0.16f; // settles between bursts
    int weaponMagazineCapacity = 30;
    int weaponReserveAmmo = 90;
    float weaponReloadDuration = 1.35f;
    float weaponFireFovPunch = 4.8f;
    float weaponFireCameraKick = 4.9f;
    float weaponViewmodelSway = 1.0f;
    // Screen-space muzzle origin (fractions of width/height) for viewmodel
    // flash and the visible HUD-layer bullet tracer.
    float weaponMuzzleScreenX = 0.57f;
    float weaponMuzzleScreenY = 0.62f;

    float enemyMaxHealth = 120.0f;
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
    float enemyProjectileSpeed = 8.0f;
    float enemyProjectileRadius = 0.30f;
    float enemyProjectileLife = 5.0f;

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

    // On-screen directional hit indicator: how long a damage wedge lingers.
    float damageIndicatorSeconds = 1.5f;

    float spawnInterval = 2.8f;
    int spawnMaxConcurrent = 5;
    float spawnRingRadius = 14.0f;
    float spawnRangedChance = 0.30f;
    float spawnTankChance = 0.18f;

    bool technoEnabled = true;
    float technoBpm = 146.0f;
    float technoBaseVolume = 0.42f;
};

} // namespace pulse

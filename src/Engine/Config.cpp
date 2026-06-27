#include "Engine/Config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pulse {
namespace {

std::string trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseBool(const std::string& value) {
    const std::string v = lower(trim(value));
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

float parseFloat(const std::string& value, float defaultValue) {
    try {
        return std::stof(value);
    } catch (...) {
        return defaultValue;
    }
}

int parseInt(const std::string& value, int defaultValue) {
    try {
        return std::stoi(value);
    } catch (...) {
        return defaultValue;
    }
}

std::vector<std::string> candidatePaths() {
    return {
        "config/pulse.tuning",
        "../config/pulse.tuning",
        "../../config/pulse.tuning"
    };
}

std::vector<std::string> styleCandidatePaths() {
    return {
        "config/pulse.style",
        "../config/pulse.style",
        "../../config/pulse.style"
    };
}

// Parse up to three comma-separated floats ("r, g, b") into a StyleColor,
// keeping the supplied default for any component the value omits.
StyleColor parseColor(const std::string& value, StyleColor def) {
    std::stringstream ss(value);
    std::string tok;
    float c[3] = { def.r, def.g, def.b };
    int i = 0;
    while (i < 3 && std::getline(ss, tok, ',')) {
        c[i] = parseFloat(trim(tok), c[i]);
        ++i;
    }
    return { c[0], c[1], c[2] };
}

} // namespace

ConfigLoadResult loadTunablesFromDisk(Tunables& tunables) {
    std::ifstream file;
    std::string usedPath;
    for (const std::string& path : candidatePaths()) {
        file.open(path);
        if (file.good()) {
            usedPath = path;
            break;
        }
        file.close();
    }

    if (!file.good()) {
        return {false, "", "Could not find config/pulse.tuning"};
    }

    int assignments = 0;
    std::string line;
    while (std::getline(file, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) {
            continue;
        }

        ++assignments;
        if (key == "run.rooms_before_boss_min") {
            tunables.runMinRoomsBeforeBoss = std::clamp(parseInt(value, tunables.runMinRoomsBeforeBoss), 2, 12);
            continue;
        }
        if (key == "run.rooms_before_boss_extra") {
            tunables.runExtraRoomsBeforeBoss = std::clamp(parseInt(value, tunables.runExtraRoomsBeforeBoss), 0, 6);
            continue;
        }
        if (key == "window.width") tunables.windowWidth = parseInt(value, tunables.windowWidth);
        else if (key == "window.height") tunables.windowHeight = parseInt(value, tunables.windowHeight);
        else if (key == "camera.fov_degrees") tunables.cameraFovDegrees = parseFloat(value, tunables.cameraFovDegrees);
        else if (key == "camera.mouse_sensitivity") tunables.mouseSensitivity = parseFloat(value, tunables.mouseSensitivity);
        else if (key == "camera.pitch_limit_degrees") tunables.pitchLimitDegrees = parseFloat(value, tunables.pitchLimitDegrees);
        else if (key == "camera.fire_impact_recovery") tunables.fireImpactRecovery = parseFloat(value, tunables.fireImpactRecovery);
        else if (key == "camera.shake_fire") tunables.cameraShakeFire = parseFloat(value, tunables.cameraShakeFire);
        else if (key == "camera.shake_kill") tunables.cameraShakeKill = parseFloat(value, tunables.cameraShakeKill);
        else if (key == "camera.shake_hurt") tunables.cameraShakeHurt = parseFloat(value, tunables.cameraShakeHurt);
        else if (key == "camera.shake_decay") tunables.cameraShakeDecay = parseFloat(value, tunables.cameraShakeDecay);
        else if (key == "camera.bob_degrees") tunables.cameraBobDegrees = parseFloat(value, tunables.cameraBobDegrees);
        else if (key == "camera.bob_speed") tunables.cameraBobSpeed = parseFloat(value, tunables.cameraBobSpeed);
        else if (key == "camera.landing_kick_degrees") tunables.cameraLandingKickDegrees = parseFloat(value, tunables.cameraLandingKickDegrees);
        else if (key == "camera.strafe_lean_degrees") tunables.cameraStrafeLeanDegrees = parseFloat(value, tunables.cameraStrafeLeanDegrees);
        else if (key == "crosshair.base_gap") tunables.crosshairBaseGap = parseFloat(value, tunables.crosshairBaseGap);
        else if (key == "crosshair.bloom_scale") tunables.crosshairBloomScale = parseFloat(value, tunables.crosshairBloomScale);
        else if (key == "render.fog_density") tunables.renderFogDensity = parseFloat(value, tunables.renderFogDensity);
        else if (key == "feel.hitstop_kill") tunables.feelHitstopKill = parseFloat(value, tunables.feelHitstopKill);
        else if (key == "feel.hitstop_scale") tunables.feelHitstopScale = parseFloat(value, tunables.feelHitstopScale);
        else if (key == "feel.hitstop_precision_mult") tunables.feelHitstopPrecisionMult = parseFloat(value, tunables.feelHitstopPrecisionMult);
        else if (key == "feel.hitstop_hit") tunables.feelHitstopHit = parseFloat(value, tunables.feelHitstopHit);
        else if (key == "feel.kill_burst_seconds") tunables.feelKillBurstSeconds = parseFloat(value, tunables.feelKillBurstSeconds);
        else if (key == "feel.impact_hit_seconds") tunables.feelImpactHitSeconds = parseFloat(value, tunables.feelImpactHitSeconds);
        else if (key == "feel.impact_wall_seconds") tunables.feelImpactWallSeconds = parseFloat(value, tunables.feelImpactWallSeconds);
        else if (key == "feel.hit_knockback") tunables.feelHitKnockback = parseFloat(value, tunables.feelHitKnockback);
        else if (key == "feel.debris_speed") tunables.feelDebrisSpeed = parseFloat(value, tunables.feelDebrisSpeed);
        else if (key == "movement.walk_speed") tunables.walkSpeed = parseFloat(value, tunables.walkSpeed);
        else if (key == "movement.acceleration") tunables.acceleration = parseFloat(value, tunables.acceleration);
        else if (key == "movement.braking") tunables.braking = parseFloat(value, tunables.braking);
        else if (key == "movement.air_control") tunables.airControl = parseFloat(value, tunables.airControl);
        else if (key == "movement.player_radius") tunables.playerRadius = parseFloat(value, tunables.playerRadius);
        else if (key == "player.max_health") tunables.playerMaxHealth = parseInt(value, tunables.playerMaxHealth);
        else if (key == "player.start_health") tunables.playerStartHealth = parseInt(value, tunables.playerStartHealth);
        else if (key == "player.max_shield") tunables.playerMaxShield = parseInt(value, tunables.playerMaxShield);
        else if (key == "player.start_shield") tunables.playerStartShield = parseInt(value, tunables.playerStartShield);
        else if (key == "pickup.spawn_interval") tunables.pickupSpawnInterval = parseFloat(value, tunables.pickupSpawnInterval);
        else if (key == "pickup.max_active") tunables.pickupMaxActive = parseInt(value, tunables.pickupMaxActive);
        else if (key == "pickup.health_amount") tunables.pickupHealthAmount = parseInt(value, tunables.pickupHealthAmount);
        else if (key == "pickup.shield_amount") tunables.pickupShieldAmount = parseInt(value, tunables.pickupShieldAmount);
        else if (key == "pickup.ammo_amount") tunables.pickupAmmoAmount = parseInt(value, tunables.pickupAmmoAmount);
        else if (key == "pickup.collect_radius") tunables.pickupCollectRadius = parseFloat(value, tunables.pickupCollectRadius);
        else if (key == "dash.impulse") tunables.dashImpulse = parseFloat(value, tunables.dashImpulse);
        else if (key == "dash.duration") tunables.dashDuration = parseFloat(value, tunables.dashDuration);
        else if (key == "dash.air_mult") tunables.airDashMult = parseFloat(value, tunables.airDashMult);
        else if (key == "dash.cooldown") tunables.dashCooldown = parseFloat(value, tunables.dashCooldown);
        else if (key == "dash.fov_punch") tunables.dashFovPunch = parseFloat(value, tunables.dashFovPunch);
        else if (key == "dash.invuln_seconds") tunables.dashInvulnSeconds = parseFloat(value, tunables.dashInvulnSeconds);
        else if (key == "movement.eye_height") tunables.eyeHeight = parseFloat(value, tunables.eyeHeight);
        else if (key == "movement.jump_velocity") tunables.jumpVelocity = parseFloat(value, tunables.jumpVelocity);
        else if (key == "movement.gravity") tunables.gravity = parseFloat(value, tunables.gravity);
        else if (key == "movement.jump_apex_cap") tunables.jumpApexCap = parseFloat(value, tunables.jumpApexCap);
        else if (key == "movement.air_jump_count") tunables.airJumpCount = static_cast<int>(parseFloat(value, static_cast<float>(tunables.airJumpCount)));
        else if (key == "movement.air_jump_velocity") tunables.airJumpVelocity = parseFloat(value, tunables.airJumpVelocity);
        else if (key == "movement.coyote_time") tunables.coyoteTime = parseFloat(value, tunables.coyoteTime);
        else if (key == "movement.jump_buffer_time") tunables.jumpBufferTime = parseFloat(value, tunables.jumpBufferTime);
        else if (key == "movement.fall_gravity_mult") tunables.fallGravityMult = parseFloat(value, tunables.fallGravityMult);
        else if (key == "movement.low_jump_mult") tunables.lowJumpMult = parseFloat(value, tunables.lowJumpMult);
        else if (key == "weapon.auto_fire") tunables.weaponAutoFire = parseBool(value);
        else if (key == "weapon.fire_rate") tunables.weaponFireRate = parseFloat(value, tunables.weaponFireRate);
        else if (key == "weapon.damage") tunables.weaponDamage = parseFloat(value, tunables.weaponDamage);
        else if (key == "weapon.spread_degrees") tunables.weaponSpreadDegrees = parseFloat(value, tunables.weaponSpreadDegrees);
        else if (key == "weapon.first_shot_inaccuracy_degrees") tunables.weaponFirstShotInaccuracyDegrees = parseFloat(value, tunables.weaponFirstShotInaccuracyDegrees);
        else if (key == "weapon.spray_inaccuracy_degrees") tunables.weaponSprayInaccuracyDegrees = parseFloat(value, tunables.weaponSprayInaccuracyDegrees);
        else if (key == "weapon.move_inaccuracy_degrees") tunables.weaponMoveInaccuracyDegrees = parseFloat(value, tunables.weaponMoveInaccuracyDegrees);
        else if (key == "weapon.recoil_pitch_degrees") tunables.weaponRecoilPitchDegrees = parseFloat(value, tunables.weaponRecoilPitchDegrees);
        else if (key == "weapon.recoil_yaw_jitter_degrees") tunables.weaponRecoilYawJitterDegrees = parseFloat(value, tunables.weaponRecoilYawJitterDegrees);
        else if (key == "weapon.recoil_recovery_rate") tunables.weaponRecoilRecoveryRate = parseFloat(value, tunables.weaponRecoilRecoveryRate);
        else if (key == "weapon.recoil_residual_fraction") tunables.weaponRecoilResidualFraction = parseFloat(value, tunables.weaponRecoilResidualFraction);
        else if (key == "weapon.recoil_reset_seconds") tunables.weaponRecoilResetSeconds = parseFloat(value, tunables.weaponRecoilResetSeconds);
        else if (key == "weapon.magazine_capacity") tunables.weaponMagazineCapacity = parseInt(value, tunables.weaponMagazineCapacity);
        else if (key == "weapon.reserve_ammo") tunables.weaponReserveAmmo = parseInt(value, tunables.weaponReserveAmmo);
        else if (key == "weapon.reload_duration") tunables.weaponReloadDuration = parseFloat(value, tunables.weaponReloadDuration);
        else if (key == "weapon.fire_fov_punch") tunables.weaponFireFovPunch = parseFloat(value, tunables.weaponFireFovPunch);
        else if (key == "weapon.fire_camera_kick") tunables.weaponFireCameraKick = parseFloat(value, tunables.weaponFireCameraKick);
        else if (key == "weapon.fire_volume") tunables.weaponFireVolume = parseFloat(value, tunables.weaponFireVolume);
        else if (key == "weapon.viewmodel_fov_degrees") tunables.weaponViewmodelFovDegrees = parseFloat(value, tunables.weaponViewmodelFovDegrees);
        else if (key == "weapon.pistol_viewmodel_fov_degrees") tunables.pistolViewmodelFovDegrees = parseFloat(value, tunables.pistolViewmodelFovDegrees);
        else if (key == "weapon.pistol_idle_sway_scale") tunables.pistolIdleSwayScale = parseFloat(value, tunables.pistolIdleSwayScale);
        else if (key == "weapon.viewmodel_sway") tunables.weaponViewmodelSway = parseFloat(value, tunables.weaponViewmodelSway);
        else if (key == "weapon.viewmodel_kick_scale") tunables.weaponViewmodelKickScale = parseFloat(value, tunables.weaponViewmodelKickScale);
        else if (key == "weapon.viewmodel_kick_recovery") tunables.weaponViewmodelKickRecovery = parseFloat(value, tunables.weaponViewmodelKickRecovery);
        else if (key == "weapon.muzzle_screen_x") tunables.weaponMuzzleScreenX = parseFloat(value, tunables.weaponMuzzleScreenX);
        else if (key == "weapon.muzzle_screen_y") tunables.weaponMuzzleScreenY = parseFloat(value, tunables.weaponMuzzleScreenY);
        else if (key == "enemy.max_health") tunables.enemyMaxHealth = parseFloat(value, tunables.enemyMaxHealth);
        else if (key == "enemy.headshot_min_fraction") tunables.enemyHeadshotMinFraction = parseFloat(value, tunables.enemyHeadshotMinFraction);
        else if (key == "enemy.rusher_speed") tunables.enemyRusherSpeed = parseFloat(value, tunables.enemyRusherSpeed);
        else if (key == "enemy.ranged_speed") tunables.enemyRangedSpeed = parseFloat(value, tunables.enemyRangedSpeed);
        else if (key == "enemy.touch_distance" || key == "enemy.melee_range") tunables.enemyMeleeRange = parseFloat(value, tunables.enemyMeleeRange);
        else if (key == "enemy.touch_damage" || key == "enemy.melee_damage") tunables.enemyMeleeDamage = parseInt(value, tunables.enemyMeleeDamage);
        else if (key == "enemy.melee_telegraph") tunables.enemyMeleeTelegraph = parseFloat(value, tunables.enemyMeleeTelegraph);
        else if (key == "enemy.melee_recover") tunables.enemyMeleeRecover = parseFloat(value, tunables.enemyMeleeRecover);
        else if (key == "enemy.melee_lunge") tunables.enemyMeleeLunge = parseFloat(value, tunables.enemyMeleeLunge);
        else if (key == "enemy.ranged_damage") tunables.enemyRangedDamage = parseInt(value, tunables.enemyRangedDamage);
        else if (key == "enemy.ranged_cooldown") tunables.enemyRangedCooldown = parseFloat(value, tunables.enemyRangedCooldown);
        else if (key == "enemy.ranged_telegraph") tunables.enemyRangedTelegraph = parseFloat(value, tunables.enemyRangedTelegraph);
        else if (key == "enemy.projectile_speed") tunables.enemyProjectileSpeed = parseFloat(value, tunables.enemyProjectileSpeed);
        else if (key == "enemy.projectile_radius") tunables.enemyProjectileRadius = parseFloat(value, tunables.enemyProjectileRadius);
        else if (key == "enemy.projectile_life") tunables.enemyProjectileLife = parseFloat(value, tunables.enemyProjectileLife);
        else if (key == "enemy.shoot_range") tunables.enemyShootRange = parseFloat(value, tunables.enemyShootRange);
        else if (key == "enemy.separation_radius") tunables.enemySeparationRadius = parseFloat(value, tunables.enemySeparationRadius);
        else if (key == "enemy.separation_strength") tunables.enemySeparationStrength = parseFloat(value, tunables.enemySeparationStrength);
        else if (key == "enemy.tank_health_mult") tunables.enemyTankHealthMult = parseFloat(value, tunables.enemyTankHealthMult);
        else if (key == "enemy.tank_speed") tunables.enemyTankSpeed = parseFloat(value, tunables.enemyTankSpeed);
        else if (key == "enemy.tank_touch_damage" || key == "enemy.tank_melee_damage") tunables.enemyTankMeleeDamage = parseInt(value, tunables.enemyTankMeleeDamage);
        else if (key == "enemy.tank_scale") tunables.enemyTankScale = parseFloat(value, tunables.enemyTankScale);
        else if (key == "enemy.rusher_lunge_range") tunables.enemyRusherLungeRange = parseFloat(value, tunables.enemyRusherLungeRange);
        else if (key == "enemy.rusher_lunge_speed") tunables.enemyRusherLungeSpeed = parseFloat(value, tunables.enemyRusherLungeSpeed);
        else if (key == "enemy.stalker_speed") tunables.enemyStalkerSpeed = parseFloat(value, tunables.enemyStalkerSpeed);
        else if (key == "enemy.stalker_pounce_range") tunables.enemyStalkerPounceRange = parseFloat(value, tunables.enemyStalkerPounceRange);
        else if (key == "enemy.stalker_pounce_speed") tunables.enemyStalkerPounceSpeed = parseFloat(value, tunables.enemyStalkerPounceSpeed);
        else if (key == "spawning.interval") tunables.spawnInterval = parseFloat(value, tunables.spawnInterval);
        else if (key == "spawning.max_concurrent") tunables.spawnMaxConcurrent = parseInt(value, tunables.spawnMaxConcurrent);
        else if (key == "spawning.ring_radius") tunables.spawnRingRadius = parseFloat(value, tunables.spawnRingRadius);
        else if (key == "spawning.ranged_chance") tunables.spawnRangedChance = parseFloat(value, tunables.spawnRangedChance);
        else if (key == "spawning.tank_chance") tunables.spawnTankChance = parseFloat(value, tunables.spawnTankChance);
        else if (key == "spawning.stalker_chance") tunables.spawnStalkerChance = parseFloat(value, tunables.spawnStalkerChance);
        else if (key == "hud.damage_indicator_seconds") tunables.damageIndicatorSeconds = parseFloat(value, tunables.damageIndicatorSeconds);
        else if (key == "techno.enabled") tunables.technoEnabled = parseBool(value);
        else if (key == "techno.bpm") tunables.technoBpm = parseFloat(value, tunables.technoBpm);
        else if (key == "techno.base_volume") tunables.technoBaseVolume = parseFloat(value, tunables.technoBaseVolume);
        else if (key == "music.v4") tunables.musicV4 = parseBool(value);   // v4 reactive-tension music (A/B by feel)
    }

    std::ostringstream message;
    message << "Loaded " << assignments << " tuning values from " << usedPath;
    return {true, usedPath, message.str()};
}

ConfigLoadResult loadStyleFromDisk(StyleConfig& style) {
    std::ifstream file;
    std::string usedPath;
    for (const std::string& path : styleCandidatePaths()) {
        file.open(path);
        if (file.good()) {
            usedPath = path;
            break;
        }
        file.close();
    }

    if (!file.good()) {
        // Missing file is non-fatal: the struct defaults already mirror pulse.style.
        return {false, "", "Could not find config/pulse.style (using built-in defaults)"};
    }

    int assignments = 0;
    std::string line;
    while (std::getline(file, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }

        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) {
            continue;
        }

        ++assignments;
        if      (key == "palette.shadow_indigo")      style.shadowIndigo    = parseColor(value, style.shadowIndigo);
        else if (key == "palette.shadow_indigo_hi")   style.shadowIndigoHi  = parseColor(value, style.shadowIndigoHi);
        else if (key == "palette.env_base")           style.envBase         = parseColor(value, style.envBase);
        else if (key == "palette.env_base_hi")        style.envBaseHi       = parseColor(value, style.envBaseHi);
        else if (key == "palette.light_concrete")     style.lightConcrete   = parseColor(value, style.lightConcrete);
        else if (key == "palette.light_concrete_hi")  style.lightConcreteHi = parseColor(value, style.lightConcreteHi);
        else if (key == "palette.enemy_magenta")      style.enemyMagenta    = parseColor(value, style.enemyMagenta);
        else if (key == "palette.enemy_magenta_hot")  style.enemyMagentaHot = parseColor(value, style.enemyMagentaHot);
        else if (key == "palette.friendly_cyan")      style.friendlyCyan    = parseColor(value, style.friendlyCyan);
        else if (key == "palette.friendly_cyan_hot")  style.friendlyCyanHot = parseColor(value, style.friendlyCyanHot);
        else if (key == "palette.nav_amber")          style.navAmber        = parseColor(value, style.navAmber);
        else if (key == "palette.nav_amber_hot")      style.navAmberHot     = parseColor(value, style.navAmberHot);
        else if (key == "palette.ui_text")            style.uiText          = parseColor(value, style.uiText);
        else if (key == "palette.ink_outline")        style.inkOutline      = parseColor(value, style.inkOutline);
        else if (key == "sky.zenith")                 style.skyZenith       = parseColor(value, style.skyZenith);
        else if (key == "sky.horizon")                style.skyHorizon      = parseColor(value, style.skyHorizon);
        else if (key == "sky.fog_tint")               style.fogTint         = parseColor(value, style.fogTint);
        else if (key == "shading.band_shadow")        style.bandShadow      = parseFloat(value, style.bandShadow);
        else if (key == "shading.band_lit")           style.bandLit         = parseFloat(value, style.bandLit);
        else if (key == "shading.band_softness")      style.bandSoftness    = parseFloat(value, style.bandSoftness);
        else if (key == "outline.env_px")             style.outlineEnvPx        = parseFloat(value, style.outlineEnvPx);
        else if (key == "outline.prop_px")            style.outlinePropPx       = parseFloat(value, style.outlinePropPx);
        else if (key == "outline.enemy_px")           style.outlineEnemyPx      = parseFloat(value, style.outlineEnemyPx);
        else if (key == "outline.internal_scale")     style.outlineInternalScale= parseFloat(value, style.outlineInternalScale);
        else if (key == "outline.hero_scale")          style.outlineHeroScale    = parseFloat(value, style.outlineHeroScale);
        else if (key == "hatch.strength")             style.hatchStrength   = parseFloat(value, style.hatchStrength);
        else if (key == "hatch.scale")                style.hatchScale      = parseFloat(value, style.hatchScale);
        else if (key == "hatch.width")                style.hatchWidth      = parseFloat(value, style.hatchWidth);
        else if (key == "hatch.fade")                 style.hatchFade       = parseFloat(value, style.hatchFade);
        else if (key == "mat.matte.rough_min")        style.matte.roughMin  = parseFloat(value, style.matte.roughMin);
        else if (key == "mat.matte.rough_max")        style.matte.roughMax  = parseFloat(value, style.matte.roughMax);
        else if (key == "mat.matte.metallic")         style.matte.metallic  = parseFloat(value, style.matte.metallic);
        else if (key == "mat.matte.emissive")         style.matte.emissive  = parseFloat(value, style.matte.emissive);
        else if (key == "mat.matte.reflect")          style.matte.reflect   = parseFloat(value, style.matte.reflect);
        else if (key == "mat.metal.rough_min")        style.metal.roughMin  = parseFloat(value, style.metal.roughMin);
        else if (key == "mat.metal.rough_max")        style.metal.roughMax  = parseFloat(value, style.metal.roughMax);
        else if (key == "mat.metal.metallic")         style.metal.metallic  = parseFloat(value, style.metal.metallic);
        else if (key == "mat.metal.emissive")         style.metal.emissive  = parseFloat(value, style.metal.emissive);
        else if (key == "mat.metal.reflect")          style.metal.reflect   = parseFloat(value, style.metal.reflect);
        else if (key == "mat.obsidian.rough_min")     style.obsidian.roughMin = parseFloat(value, style.obsidian.roughMin);
        else if (key == "mat.obsidian.rough_max")     style.obsidian.roughMax = parseFloat(value, style.obsidian.roughMax);
        else if (key == "mat.obsidian.metallic")      style.obsidian.metallic = parseFloat(value, style.obsidian.metallic);
        else if (key == "mat.obsidian.emissive")      style.obsidian.emissive = parseFloat(value, style.obsidian.emissive);
        else if (key == "mat.obsidian.reflect")       style.obsidian.reflect  = parseFloat(value, style.obsidian.reflect);
        else if (key == "mat.emissive_cyan.rough_min")    style.emissiveCyan.roughMin = parseFloat(value, style.emissiveCyan.roughMin);
        else if (key == "mat.emissive_cyan.rough_max")    style.emissiveCyan.roughMax = parseFloat(value, style.emissiveCyan.roughMax);
        else if (key == "mat.emissive_cyan.metallic")     style.emissiveCyan.metallic = parseFloat(value, style.emissiveCyan.metallic);
        else if (key == "mat.emissive_cyan.emissive")     style.emissiveCyan.emissive = parseFloat(value, style.emissiveCyan.emissive);
        else if (key == "mat.emissive_cyan.reflect")      style.emissiveCyan.reflect  = parseFloat(value, style.emissiveCyan.reflect);
        else if (key == "mat.emissive_magenta.rough_min") style.emissiveMagenta.roughMin = parseFloat(value, style.emissiveMagenta.roughMin);
        else if (key == "mat.emissive_magenta.rough_max") style.emissiveMagenta.roughMax = parseFloat(value, style.emissiveMagenta.roughMax);
        else if (key == "mat.emissive_magenta.metallic")  style.emissiveMagenta.metallic = parseFloat(value, style.emissiveMagenta.metallic);
        else if (key == "mat.emissive_magenta.emissive")  style.emissiveMagenta.emissive = parseFloat(value, style.emissiveMagenta.emissive);
        else if (key == "mat.emissive_magenta.reflect")   style.emissiveMagenta.reflect  = parseFloat(value, style.emissiveMagenta.reflect);
        // Interface system (Neon Ink Brutalism UI). Colours mirror pal::; metrics
        // are consumed live by the HUD/menus (hot-reloadable on F5).
        else if (key == "ui.cyan")              style.ui.cyan          = parseColor(value, style.ui.cyan);
        else if (key == "ui.cyan_hot")          style.ui.cyanHot       = parseColor(value, style.ui.cyanHot);
        else if (key == "ui.magenta")           style.ui.magenta       = parseColor(value, style.ui.magenta);
        else if (key == "ui.magenta_soft")      style.ui.magentaSoft   = parseColor(value, style.ui.magentaSoft);
        else if (key == "ui.orange")            style.ui.orange        = parseColor(value, style.ui.orange);
        else if (key == "ui.orange_hot")        style.ui.orangeHot     = parseColor(value, style.ui.orangeHot);
        else if (key == "ui.slate")             style.ui.slate         = parseColor(value, style.ui.slate);
        else if (key == "ui.slate_dim")         style.ui.slateDim      = parseColor(value, style.ui.slateDim);
        else if (key == "ui.faint")             style.ui.faint         = parseColor(value, style.ui.faint);
        else if (key == "ui.text_hi")           style.ui.textHi        = parseColor(value, style.ui.textHi);
        else if (key == "ui.text_hero")         style.ui.textHero      = parseColor(value, style.ui.textHero);
        else if (key == "ui.text_mid")          style.ui.textMid       = parseColor(value, style.ui.textMid);
        else if (key == "ui.navy")              style.ui.navy          = parseColor(value, style.ui.navy);
        else if (key == "ui.deep")              style.ui.deep          = parseColor(value, style.ui.deep);
        else if (key == "ui.border")            style.ui.border        = parseColor(value, style.ui.border);
        else if (key == "ui.ink_stroke")        style.ui.inkStroke     = parseColor(value, style.ui.inkStroke);
        else if (key == "ui.tier_common")       style.ui.tierCommon    = parseColor(value, style.ui.tierCommon);
        else if (key == "ui.tier_uncommon")     style.ui.tierUncommon  = parseColor(value, style.ui.tierUncommon);
        else if (key == "ui.tier_rare")         style.ui.tierRare      = parseColor(value, style.ui.tierRare);
        else if (key == "ui.room_combat")       style.ui.roomCombat    = parseColor(value, style.ui.roomCombat);
        else if (key == "ui.room_elite")        style.ui.roomElite     = parseColor(value, style.ui.roomElite);
        else if (key == "ui.room_cache")        style.ui.roomCache     = parseColor(value, style.ui.roomCache);
        else if (key == "ui.room_boss")         style.ui.roomBoss      = parseColor(value, style.ui.roomBoss);
        else if (key == "ui.margin_px")         style.ui.marginPx       = parseFloat(value, style.ui.marginPx);
        else if (key == "ui.grid_px")           style.ui.gridPx         = parseFloat(value, style.ui.gridPx);
        else if (key == "ui.corner_px")         style.ui.cornerPx       = parseFloat(value, style.ui.cornerPx);
        else if (key == "ui.corner_small_px")   style.ui.cornerSmallPx  = parseFloat(value, style.ui.cornerSmallPx);
        else if (key == "ui.corner_large_px")   style.ui.cornerLargePx  = parseFloat(value, style.ui.cornerLargePx);
        else if (key == "ui.stroke_px")         style.ui.strokePx       = parseFloat(value, style.ui.strokePx);
        else if (key == "ui.stroke_focus_px")   style.ui.strokeFocusPx  = parseFloat(value, style.ui.strokeFocusPx);
        else if (key == "ui.spine_px")          style.ui.spinePx        = parseFloat(value, style.ui.spinePx);
        else if (key == "ui.bar_px")            style.ui.barPx          = parseFloat(value, style.ui.barPx);
        else if (key == "ui.glow_radius_px")    style.ui.glowRadiusPx   = parseFloat(value, style.ui.glowRadiusPx);
        else if (key == "ui.waveform_stroke_px") style.ui.waveformStrokePx = parseFloat(value, style.ui.waveformStrokePx);
    }

    style.loaded = true;
    std::ostringstream message;
    message << "Loaded " << assignments << " style values from " << usedPath;
    return {true, usedPath, message.str()};
}

MaterialDesc styledMaterial(const StyleConfig& style, StyleCategory category, Vec4f baseColorFactor) {
    const StyleMaterial* m = &style.matte;
    switch (category) {
        case StyleCategory::MatteArch:        m = &style.matte; break;
        case StyleCategory::PaintedMetal:     m = &style.metal; break;
        case StyleCategory::PolishedObsidian: m = &style.obsidian; break;
        case StyleCategory::EmissiveCyan:     m = &style.emissiveCyan; break;
        case StyleCategory::EmissiveMagenta:  m = &style.emissiveMagenta; break;
    }
    MaterialDesc d;
    d.baseColorFactor = baseColorFactor;
    d.metallic = m->metallic;
    // No source roughness here, so sit at the middle of the locked range.
    d.roughness = std::clamp(0.5f * (m->roughMin + m->roughMax), m->roughMin, m->roughMax);
    d.emissive = m->emissive;
    // Low 3 bits = category; bit 8 = reflection-importance hint (consumed by W6).
    d.styleFlags = static_cast<uint32_t>(category) | (m->reflect >= 0.5f ? 0x100u : 0u);
    return d;
}

} // namespace pulse

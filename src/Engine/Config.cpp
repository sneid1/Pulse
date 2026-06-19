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
        else if (key == "render.fog_density") tunables.renderFogDensity = parseFloat(value, tunables.renderFogDensity);
        else if (key == "feel.hitstop_kill") tunables.feelHitstopKill = parseFloat(value, tunables.feelHitstopKill);
        else if (key == "feel.hitstop_scale") tunables.feelHitstopScale = parseFloat(value, tunables.feelHitstopScale);
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
        else if (key == "dash.cooldown") tunables.dashCooldown = parseFloat(value, tunables.dashCooldown);
        else if (key == "dash.fov_punch") tunables.dashFovPunch = parseFloat(value, tunables.dashFovPunch);
        else if (key == "movement.eye_height") tunables.eyeHeight = parseFloat(value, tunables.eyeHeight);
        else if (key == "movement.jump_velocity") tunables.jumpVelocity = parseFloat(value, tunables.jumpVelocity);
        else if (key == "movement.gravity") tunables.gravity = parseFloat(value, tunables.gravity);
        else if (key == "movement.jump_apex_cap") tunables.jumpApexCap = parseFloat(value, tunables.jumpApexCap);
        else if (key == "weapon.auto_fire") tunables.weaponAutoFire = parseBool(value);
        else if (key == "weapon.fire_rate") tunables.weaponFireRate = parseFloat(value, tunables.weaponFireRate);
        else if (key == "weapon.damage") tunables.weaponDamage = parseFloat(value, tunables.weaponDamage);
        else if (key == "weapon.spread_degrees") tunables.weaponSpreadDegrees = parseFloat(value, tunables.weaponSpreadDegrees);
        else if (key == "weapon.recoil_pitch_degrees") tunables.weaponRecoilPitchDegrees = parseFloat(value, tunables.weaponRecoilPitchDegrees);
        else if (key == "weapon.recoil_yaw_jitter_degrees") tunables.weaponRecoilYawJitterDegrees = parseFloat(value, tunables.weaponRecoilYawJitterDegrees);
        else if (key == "weapon.recoil_recovery_rate") tunables.weaponRecoilRecoveryRate = parseFloat(value, tunables.weaponRecoilRecoveryRate);
        else if (key == "weapon.recoil_residual_fraction") tunables.weaponRecoilResidualFraction = parseFloat(value, tunables.weaponRecoilResidualFraction);
        else if (key == "weapon.magazine_capacity") tunables.weaponMagazineCapacity = parseInt(value, tunables.weaponMagazineCapacity);
        else if (key == "weapon.reserve_ammo") tunables.weaponReserveAmmo = parseInt(value, tunables.weaponReserveAmmo);
        else if (key == "weapon.reload_duration") tunables.weaponReloadDuration = parseFloat(value, tunables.weaponReloadDuration);
        else if (key == "weapon.fire_fov_punch") tunables.weaponFireFovPunch = parseFloat(value, tunables.weaponFireFovPunch);
        else if (key == "weapon.fire_camera_kick") tunables.weaponFireCameraKick = parseFloat(value, tunables.weaponFireCameraKick);
        else if (key == "weapon.viewmodel_sway") tunables.weaponViewmodelSway = parseFloat(value, tunables.weaponViewmodelSway);
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
        else if (key == "enemy.separation_radius") tunables.enemySeparationRadius = parseFloat(value, tunables.enemySeparationRadius);
        else if (key == "enemy.separation_strength") tunables.enemySeparationStrength = parseFloat(value, tunables.enemySeparationStrength);
        else if (key == "enemy.tank_health_mult") tunables.enemyTankHealthMult = parseFloat(value, tunables.enemyTankHealthMult);
        else if (key == "enemy.tank_speed") tunables.enemyTankSpeed = parseFloat(value, tunables.enemyTankSpeed);
        else if (key == "enemy.tank_touch_damage" || key == "enemy.tank_melee_damage") tunables.enemyTankMeleeDamage = parseInt(value, tunables.enemyTankMeleeDamage);
        else if (key == "enemy.tank_scale") tunables.enemyTankScale = parseFloat(value, tunables.enemyTankScale);
        else if (key == "enemy.rusher_lunge_range") tunables.enemyRusherLungeRange = parseFloat(value, tunables.enemyRusherLungeRange);
        else if (key == "enemy.rusher_lunge_speed") tunables.enemyRusherLungeSpeed = parseFloat(value, tunables.enemyRusherLungeSpeed);
        else if (key == "spawning.interval") tunables.spawnInterval = parseFloat(value, tunables.spawnInterval);
        else if (key == "spawning.max_concurrent") tunables.spawnMaxConcurrent = parseInt(value, tunables.spawnMaxConcurrent);
        else if (key == "spawning.ring_radius") tunables.spawnRingRadius = parseFloat(value, tunables.spawnRingRadius);
        else if (key == "spawning.ranged_chance") tunables.spawnRangedChance = parseFloat(value, tunables.spawnRangedChance);
        else if (key == "spawning.tank_chance") tunables.spawnTankChance = parseFloat(value, tunables.spawnTankChance);
        else if (key == "hud.damage_indicator_seconds") tunables.damageIndicatorSeconds = parseFloat(value, tunables.damageIndicatorSeconds);
        else if (key == "techno.enabled") tunables.technoEnabled = parseBool(value);
        else if (key == "techno.bpm") tunables.technoBpm = parseFloat(value, tunables.technoBpm);
        else if (key == "techno.base_volume") tunables.technoBaseVolume = parseFloat(value, tunables.technoBaseVolume);
    }

    std::ostringstream message;
    message << "Loaded " << assignments << " tuning values from " << usedPath;
    return {true, usedPath, message.str()};
}

} // namespace pulse

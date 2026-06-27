#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Game/Build.hpp"

namespace pulse {

enum class WeaponReloadMode {
    Magazine,
    PerShell
};

enum class WeaponEvent {
    Fire,
    DryFire,
    ReloadStart,
    ReloadEnd,
    MagOut,
    MagIn,
    Bolt,
    Shell,
    Equip
};

struct RecoilPoint {
    float pitchDeg = 0.0f;
    float yawDeg = 0.0f;
};

struct WeaponViewmodelProfile {
    std::string assetPath;
    std::string idleClip = "allanims";
    std::string fireClip = "allanims";
    std::string reloadClip = "allanims";
    float idleStart = 0.0f;
    float idleEnd = 0.2f;
    float fireStart = 0.0f;
    float fireEnd = 0.12f;
    float reloadStart = 0.0f;
    float reloadEnd = 1.0f;
    float fovDegrees = 74.0f;
    float scale = 0.015f;
    float x = 0.2f;
    float y = -0.45f;
    float z = 0.38f;
    float sourceYaw = 0.0f;
    float sourcePitch = 0.0f;
    float sourceRoll = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    float idleDampScale = 1.0f;
    std::string muzzleA = "base";
    std::string muzzleB;
    std::string muzzleLocalEnd;
    std::string shellSocket = "shell";
    std::string magSocket = "mag";
    std::string boltSocket = "bolt";
    bool authored = false;
};

struct WeaponProfile {
    std::string id;
    std::string role;
    WeaponArchetype archetype = WeaponArchetype::HitscanAuto;
    bool automatic = true;
    bool rewardEligible = true;
    WeaponReloadMode reloadMode = WeaponReloadMode::Magazine;

    float damage = 30.0f;
    float fireRate = 10.0f;
    float burstInterval = 0.075f;
    int burstCount = 1;
    int pellets = 1;
    float spreadDeg = 0.0f;
    float firstShotInaccuracyDeg = 0.05f;
    float sprayInaccuracyDeg = 0.1f;
    float moveInaccuracyDeg = 0.75f;
    float airborneInaccuracyDeg = 0.95f;
    int magazine = 30;
    int reserve = 90;
    bool infiniteReserve = false;   // sidearm: reserve never depletes (still reloads when the mag empties)
    float reloadSeconds = 1.35f;
    float perShellSeconds = 0.55f;
    float projectileSpeed = 0.0f;
    float splashRadius = 0.0f;
    float chargeSeconds = 0.0f;

    float recoilPitchScale = 1.0f;
    float recoilYawScale = 1.0f;
    float recoilRecoveryRate = 10.0f;
    float recoilResetSeconds = 0.34f;
    float recoilResidualFraction = 0.16f;
    float cameraKick = 4.5f;
    float cameraKickRecovery = 18.0f;
    float fovPunch = 4.0f;
    float viewmodelKick = 1.0f;
    float viewmodelKickRecovery = 18.0f;
    float viewmodelSideScale = 0.12f;

    float fireVolume = 1.0f;
    float muzzleFlashSeconds = 0.08f;
    float muzzleFlashScale = 1.0f;
    float tracerSeconds = 0.085f;
    float impactScale = 1.0f;
    float muzzleFlashR = 255.0f;
    float muzzleFlashG = 205.0f;
    float muzzleFlashB = 118.0f;
    float tracerR = 255.0f;
    float tracerG = 210.0f;
    float tracerB = 122.0f;
    float tracerWidthScale = 1.0f;
    float tracerAlphaScale = 1.0f;
    float casingScale = 1.0f;
    std::vector<RecoilPoint> recoilPattern;
    WeaponViewmodelProfile viewmodel;
};

class WeaponProfileRegistry {
public:
    WeaponProfileRegistry();

    void resetDefaults();
    bool loadFromDisk();

    const WeaponProfile* find(const std::string& id) const;
    WeaponProfile* findMutable(const std::string& id);
    const std::vector<WeaponProfile>& profiles() const { return profiles_; }

    bool rewardEligible(const std::string& id) const;
    std::vector<std::string> invalidRewardIds() const;
    std::string dump() const;

private:
    std::vector<WeaponProfile> profiles_;
};

const char* weaponArchetypeName(WeaponArchetype archetype);
WeaponArchetype parseWeaponArchetypeName(const std::string& text);
const char* weaponEventName(WeaponEvent event);

} // namespace pulse

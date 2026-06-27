#include "Game/WeaponProfile.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace pulse {
namespace {

std::string trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

float toFloat(const std::string& value, float fallback) {
    try { return std::stof(value); } catch (...) { return fallback; }
}

int toInt(const std::string& value, int fallback) {
    try { return std::stoi(value); } catch (...) { return fallback; }
}

bool toBool(const std::string& value, bool fallback) {
    const std::string v = lower(trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

// CS2-style learnable spray patterns. Each point is the CUMULATIVE view offset (in
// degrees) for that shot index: pitchDeg climbs the view UP, yawDeg sweeps it
// sideways. The shape is 100% deterministic so it can be memorised and counter-
// pulled; the runtime scales it by recoilPitchScale/recoilYawScale per weapon.
//
// AK: the signature. A steep vertical climb over the first ~7 rounds, then a hard
// right sweep, a hard left sweep, and a settle back right. Demanding but fully
// learnable: pull down through the climb, then trace the horizontal back.
std::vector<RecoilPoint> akPattern() {
    return {
        {0.00f, 0.00f}, {1.15f, 0.05f}, {2.40f, -0.05f}, {3.65f, -0.18f}, {4.80f, -0.30f},
        {5.85f, -0.34f}, {6.70f, -0.16f}, {7.35f, 0.30f}, {7.90f, 0.95f}, {8.35f, 1.75f},
        {8.70f, 2.60f}, {8.98f, 3.35f}, {9.18f, 3.78f}, {9.32f, 3.55f}, {9.43f, 2.70f},
        {9.52f, 1.50f}, {9.60f, 0.15f}, {9.66f, -1.25f}, {9.71f, -2.55f}, {9.75f, -3.50f},
        {9.79f, -4.00f}, {9.82f, -3.80f}, {9.85f, -2.95f}, {9.88f, -1.75f}, {9.91f, -0.45f},
        {9.94f, 0.85f}, {9.97f, 1.95f}, {10.00f, 2.70f}, {10.03f, 3.00f}, {10.06f, 2.85f}
    };
}

// Pistol: a small, mostly-vertical tap pattern. Rewards controlled tapping (the
// index resets between deliberate shots); fanning the trigger walks it up.
std::vector<RecoilPoint> pistolPattern() {
    return {
        {0.00f, 0.00f}, {0.55f, -0.06f}, {1.00f, 0.09f}, {1.32f, -0.07f},
        {1.52f, 0.12f}, {1.64f, 0.03f}, {1.70f, -0.11f}, {1.74f, 0.06f}
    };
}

// SMG: a faster, sprayier climb than the AK with a wandering horizontal drift -
// lower vertical per shot but it never fully settles, so sustained fire wanders.
std::vector<RecoilPoint> smgPattern() {
    return {
        {0.00f, 0.00f}, {0.65f, 0.10f}, {1.30f, 0.28f}, {1.92f, 0.48f}, {2.50f, 0.55f},
        {3.02f, 0.38f}, {3.48f, 0.05f}, {3.90f, -0.34f}, {4.28f, -0.64f}, {4.60f, -0.74f},
        {4.88f, -0.58f}, {5.12f, -0.20f}, {5.32f, 0.28f}, {5.50f, 0.72f}, {5.66f, 1.02f},
        {5.80f, 1.08f}, {5.92f, 0.80f}, {6.04f, 0.32f}, {6.14f, -0.24f}, {6.24f, -0.74f},
        {6.33f, -0.92f}, {6.42f, -0.70f}
    };
}

// Burst (carbine / railbolt): a tight vertical climb across the 3-round burst that
// resets between bursts, so each pull starts from the same place.
std::vector<RecoilPoint> burstPattern() {
    return {
        {0.00f, 0.00f}, {0.95f, 0.12f}, {1.95f, -0.18f}, {2.65f, 0.08f}
    };
}

// Precision single-shot (marksman): a strong, consistent vertical kick per shot that
// does not run away if tapped quickly - the view re-centres on release.
std::vector<RecoilPoint> precisionPattern() {
    return {
        {2.70f, 0.00f}, {2.95f, 0.16f}, {2.75f, -0.16f}, {2.95f, 0.10f}, {2.75f, -0.10f}
    };
}

// Heavy single-shot (scattergun): a big, satisfying vertical punch per shell, flat
// across the tube so every shell kicks the same.
std::vector<RecoilPoint> heavyPattern() {
    return {
        {3.80f, 0.00f}, {4.05f, 0.28f}, {3.85f, -0.28f}, {4.05f, 0.20f},
        {3.85f, -0.20f}, {4.05f, 0.14f}, {3.85f, -0.14f}, {4.05f, 0.10f}
    };
}

WeaponViewmodelProfile pistolVm() {
    WeaponViewmodelProfile vm;
    vm.assetPath = "assets/bumstrum/fps_pistol_animated/scene.gltf";
    vm.idleStart = 7.4667f; vm.idleEnd = 7.80f;
    vm.fireStart = 7.4667f; vm.fireEnd = 7.80f;
    vm.reloadStart = 2.10f; vm.reloadEnd = 3.25f;
    vm.fovDegrees = 66.0f;
    vm.scale = 0.0138f;
    vm.x = 0.120f; vm.y = -0.35f; vm.z = 0.36f;
    vm.yaw = -0.108f; vm.pitch = -0.092f; vm.roll = -0.016f;
    vm.idleDampScale = 0.35f;
    vm.muzzleA = "muzzlebreak"; vm.muzzleB = "base";
    vm.authored = true;
    return vm;
}

WeaponViewmodelProfile akVm() {
    WeaponViewmodelProfile vm;
    vm.assetPath = "assets/bumstrum/fps_ak_animated/scene.gltf";
    vm.idleStart = 0.00f; vm.idleEnd = 0.233f;
    vm.fireStart = 0.00f; vm.fireEnd = 0.125f;
    vm.reloadStart = 2.10f; vm.reloadEnd = 3.35f;
    vm.fovDegrees = 80.0f;
    vm.scale = 0.0156f;
    vm.x = 0.23f; vm.y = -0.50f; vm.z = 0.39f;
    vm.yaw = -0.060f; vm.pitch = -0.038f; vm.roll = -0.018f;
    vm.muzzleA = "base"; vm.muzzleB = "ak74";
    vm.authored = true;
    return vm;
}

WeaponViewmodelProfile carbineVm() {
    WeaponViewmodelProfile vm;
    vm.assetPath = "assets/bumstrum/fps_animated_carbine/scene.gltf";
    // Idle uses the authored ready frame at the start of the fire bank; the later forward hold
    // drops the support arm out of frame, so it only popped in while shooting.
    vm.idleStart = 0.00f; vm.idleEnd = 0.18f;
    vm.fireStart = 0.00f; vm.fireEnd = 0.18f;
    vm.reloadStart = 2.10f; vm.reloadEnd = 3.55f;
    vm.fovDegrees = 80.0f;
    // The carbine glTF bakes a ~0.0208 scale into its root node (the AK's root is identity), so its
    // world geometry is ~48x smaller; vm.scale must be ~48x the AK's 0.0156 to match. Both rigs
    // share the same root orientation, so the AK's position/rotation carry over directly.
    vm.scale = 0.75f;
    vm.x = 0.23f; vm.y = -0.50f; vm.z = 0.39f;
    vm.sourcePitch = 0.0f;
    vm.yaw = -0.060f; vm.pitch = -0.038f; vm.roll = -0.018f;
    vm.idleDampScale = 0.70f;
    vm.muzzleA = "base"; vm.muzzleB = "carbine";
    vm.authored = true;
    return vm;
}

WeaponViewmodelProfile sniperVm() {
    WeaponViewmodelProfile vm;
    vm.assetPath = "assets/bumstrum/sniper_animated/scene.gltf";
    vm.idleStart = 0.00f; vm.idleEnd = 0.20f;
    vm.fireStart = 0.00f; vm.fireEnd = 0.22f;
    vm.reloadStart = 2.10f; vm.reloadEnd = 3.60f;
    vm.fovDegrees = 80.0f;
    // sniper_animated's root node is identity-scaled (like the AK, unlike the carbine's 0.0208), so
    // it uses the AK-tier vm.scale; it is a LONGER rifle (bbox Z ~161 vs the AK's ~110) so it sits a
    // touch smaller + further forward. Same root orientation as the AK -> AK position/rotation carry.
    vm.scale = 0.0150f;
    vm.x = 0.23f; vm.y = -0.50f; vm.z = 0.42f;
    vm.yaw = -0.060f; vm.pitch = -0.038f; vm.roll = -0.018f;
    vm.muzzleA = "base"; vm.muzzleB = "sniper";
    vm.authored = true;
    return vm;
}

WeaponViewmodelProfile shotgunVm() {
    WeaponViewmodelProfile vm;
    vm.assetPath = "assets/bumstrum/shotgun_animated/scene.gltf";
    vm.idleStart = 0.00f; vm.idleEnd = 0.20f;
    vm.fireStart = 0.00f; vm.fireEnd = 0.38f;   // shot + pump-action chamber
    vm.reloadStart = 2.10f; vm.reloadEnd = 3.80f;
    vm.fovDegrees = 80.0f;
    // shotgun_animated's root node is identity-scaled (like the AK) and its bbox is nearly the AK's,
    // so the AK-tier vm.scale + position/rotation carry over directly.
    vm.scale = 0.0156f;
    vm.x = 0.23f; vm.y = -0.50f; vm.z = 0.39f;
    vm.yaw = -0.060f; vm.pitch = -0.038f; vm.roll = -0.018f;
    vm.muzzleA = "base"; vm.muzzleB = "shotgun";
    vm.authored = true;
    return vm;
}

WeaponViewmodelProfile smgVm() {
    WeaponViewmodelProfile vm;
    vm.assetPath = "assets/bumstrum/fps_smg9_animated/scene.gltf";
    vm.idleStart = 6.82f; vm.idleEnd = 7.18f;
    vm.fireStart = 0.00f; vm.fireEnd = 0.20f;
    vm.reloadStart = 2.10f; vm.reloadEnd = 3.80f;
    vm.fovDegrees = 76.0f;
    vm.scale = 0.0151f;
    vm.x = 0.25f; vm.y = -0.50f; vm.z = 0.385f;
    vm.yaw = -0.078f; vm.pitch = -0.044f; vm.roll = -0.016f;
    vm.idleDampScale = 0.85f;
    vm.muzzleA = "base"; vm.muzzleB = "smg9";
    vm.authored = true;
    return vm;
}

WeaponProfile baseProfile(const std::string& id, WeaponArchetype archetype, const WeaponViewmodelProfile& vm) {
    WeaponProfile p;
    p.id = id;
    p.archetype = archetype;
    p.viewmodel = vm;
    p.recoilPattern = akPattern();
    return p;
}

bool assetExists(const std::string& relPath) {
    if (relPath.empty()) return false;
    for (const char* prefix : { "", "../", "../../" }) {
        if (std::filesystem::exists(std::string(prefix) + relPath)) return true;
    }
    return false;
}

void applyKey(WeaponProfile& p, const std::string& rawKey, const std::string& value) {
    const std::string key = lower(trim(rawKey));
    if (key == "role") p.role = value;
    else if (key == "archetype") p.archetype = parseWeaponArchetypeName(value);
    else if (key == "automatic") p.automatic = toBool(value, p.automatic);
    else if (key == "rewardeligible") p.rewardEligible = toBool(value, p.rewardEligible);
    else if (key == "reloadmode") p.reloadMode = lower(value) == "pershell" ? WeaponReloadMode::PerShell : WeaponReloadMode::Magazine;
    else if (key == "damage") p.damage = toFloat(value, p.damage);
    else if (key == "firerate") p.fireRate = toFloat(value, p.fireRate);
    else if (key == "burstinterval") p.burstInterval = toFloat(value, p.burstInterval);
    else if (key == "burstcount") p.burstCount = toInt(value, p.burstCount);
    else if (key == "pellets") p.pellets = toInt(value, p.pellets);
    else if (key == "spreaddeg") p.spreadDeg = toFloat(value, p.spreadDeg);
    else if (key == "firstshotinaccuracydeg") p.firstShotInaccuracyDeg = toFloat(value, p.firstShotInaccuracyDeg);
    else if (key == "sprayinaccuracydeg") p.sprayInaccuracyDeg = toFloat(value, p.sprayInaccuracyDeg);
    else if (key == "moveinaccuracydeg") p.moveInaccuracyDeg = toFloat(value, p.moveInaccuracyDeg);
    else if (key == "airborneinaccuracydeg") p.airborneInaccuracyDeg = toFloat(value, p.airborneInaccuracyDeg);
    else if (key == "magazine") p.magazine = toInt(value, p.magazine);
    else if (key == "reserve") p.reserve = toInt(value, p.reserve);
    else if (key == "infinitereserve") p.infiniteReserve = toBool(value, p.infiniteReserve);
    else if (key == "reloadseconds") p.reloadSeconds = toFloat(value, p.reloadSeconds);
    else if (key == "pershellseconds") p.perShellSeconds = toFloat(value, p.perShellSeconds);
    else if (key == "projectilespeed") p.projectileSpeed = toFloat(value, p.projectileSpeed);
    else if (key == "splashradius") p.splashRadius = toFloat(value, p.splashRadius);
    else if (key == "chargeseconds") p.chargeSeconds = toFloat(value, p.chargeSeconds);
    else if (key == "recoilpitchscale") p.recoilPitchScale = toFloat(value, p.recoilPitchScale);
    else if (key == "recoilyawscale") p.recoilYawScale = toFloat(value, p.recoilYawScale);
    else if (key == "recoilrecoveryrate") p.recoilRecoveryRate = toFloat(value, p.recoilRecoveryRate);
    else if (key == "recoilresetseconds") p.recoilResetSeconds = toFloat(value, p.recoilResetSeconds);
    else if (key == "recoilresidualfraction") p.recoilResidualFraction = toFloat(value, p.recoilResidualFraction);
    else if (key == "camerakick") p.cameraKick = toFloat(value, p.cameraKick);
    else if (key == "camerakickrecovery") p.cameraKickRecovery = toFloat(value, p.cameraKickRecovery);
    else if (key == "fovpunch") p.fovPunch = toFloat(value, p.fovPunch);
    else if (key == "viewmodelkick") p.viewmodelKick = toFloat(value, p.viewmodelKick);
    else if (key == "viewmodelkickrecovery") p.viewmodelKickRecovery = toFloat(value, p.viewmodelKickRecovery);
    else if (key == "viewmodelsidescale") p.viewmodelSideScale = toFloat(value, p.viewmodelSideScale);
    else if (key == "firevolume") p.fireVolume = toFloat(value, p.fireVolume);
    else if (key == "muzzleflashseconds") p.muzzleFlashSeconds = toFloat(value, p.muzzleFlashSeconds);
    else if (key == "muzzleflashscale") p.muzzleFlashScale = toFloat(value, p.muzzleFlashScale);
    else if (key == "tracerseconds") p.tracerSeconds = toFloat(value, p.tracerSeconds);
    else if (key == "impactscale") p.impactScale = toFloat(value, p.impactScale);
    else if (key == "muzzleflashr") p.muzzleFlashR = toFloat(value, p.muzzleFlashR);
    else if (key == "muzzleflashg") p.muzzleFlashG = toFloat(value, p.muzzleFlashG);
    else if (key == "muzzleflashb") p.muzzleFlashB = toFloat(value, p.muzzleFlashB);
    else if (key == "tracerr") p.tracerR = toFloat(value, p.tracerR);
    else if (key == "tracerg") p.tracerG = toFloat(value, p.tracerG);
    else if (key == "tracerb") p.tracerB = toFloat(value, p.tracerB);
    else if (key == "tracerwidthscale") p.tracerWidthScale = toFloat(value, p.tracerWidthScale);
    else if (key == "traceralphascale") p.tracerAlphaScale = toFloat(value, p.tracerAlphaScale);
    else if (key == "casingscale") p.casingScale = toFloat(value, p.casingScale);
    else if (key == "viewmodel.asset") p.viewmodel.assetPath = value;
    else if (key == "viewmodel.idleclip") p.viewmodel.idleClip = value;
    else if (key == "viewmodel.fireclip") p.viewmodel.fireClip = value;
    else if (key == "viewmodel.reloadclip") p.viewmodel.reloadClip = value;
    else if (key == "viewmodel.idlestart") p.viewmodel.idleStart = toFloat(value, p.viewmodel.idleStart);
    else if (key == "viewmodel.idleend") p.viewmodel.idleEnd = toFloat(value, p.viewmodel.idleEnd);
    else if (key == "viewmodel.firestart") p.viewmodel.fireStart = toFloat(value, p.viewmodel.fireStart);
    else if (key == "viewmodel.fireend") p.viewmodel.fireEnd = toFloat(value, p.viewmodel.fireEnd);
    else if (key == "viewmodel.reloadstart") p.viewmodel.reloadStart = toFloat(value, p.viewmodel.reloadStart);
    else if (key == "viewmodel.reloadend") p.viewmodel.reloadEnd = toFloat(value, p.viewmodel.reloadEnd);
    else if (key == "viewmodel.fovdegrees") p.viewmodel.fovDegrees = toFloat(value, p.viewmodel.fovDegrees);
    else if (key == "viewmodel.scale") p.viewmodel.scale = toFloat(value, p.viewmodel.scale);
    else if (key == "viewmodel.x") p.viewmodel.x = toFloat(value, p.viewmodel.x);
    else if (key == "viewmodel.y") p.viewmodel.y = toFloat(value, p.viewmodel.y);
    else if (key == "viewmodel.z") p.viewmodel.z = toFloat(value, p.viewmodel.z);
    else if (key == "viewmodel.sourceyaw") p.viewmodel.sourceYaw = toFloat(value, p.viewmodel.sourceYaw);
    else if (key == "viewmodel.sourcepitch") p.viewmodel.sourcePitch = toFloat(value, p.viewmodel.sourcePitch);
    else if (key == "viewmodel.sourceroll") p.viewmodel.sourceRoll = toFloat(value, p.viewmodel.sourceRoll);
    else if (key == "viewmodel.yaw") p.viewmodel.yaw = toFloat(value, p.viewmodel.yaw);
    else if (key == "viewmodel.pitch") p.viewmodel.pitch = toFloat(value, p.viewmodel.pitch);
    else if (key == "viewmodel.roll") p.viewmodel.roll = toFloat(value, p.viewmodel.roll);
    else if (key == "viewmodel.idledampscale") p.viewmodel.idleDampScale = toFloat(value, p.viewmodel.idleDampScale);
    else if (key == "viewmodel.muzzlea") p.viewmodel.muzzleA = value;
    else if (key == "viewmodel.muzzleb") p.viewmodel.muzzleB = value;
    else if (key == "viewmodel.muzzlelocalend") p.viewmodel.muzzleLocalEnd = value;
    else if (key == "viewmodel.authored") p.viewmodel.authored = toBool(value, p.viewmodel.authored);
}

std::vector<std::string> candidatePaths() {
    return { "config/pulse.weapons", "../config/pulse.weapons", "../../config/pulse.weapons" };
}

} // namespace

WeaponProfileRegistry::WeaponProfileRegistry() {
    resetDefaults();
}

void WeaponProfileRegistry::resetDefaults() {
    profiles_.clear();

    WeaponProfile pistol = baseProfile("pistol", WeaponArchetype::HitscanAuto, pistolVm());
    pistol.role = "semi-auto sidearm";
    pistol.automatic = false;
    pistol.damage = 22.0f;
    pistol.fireRate = 7.0f;
    pistol.magazine = 12;
    pistol.reserve = 36;
    pistol.infiniteReserve = true;   // the always-available sidearm never runs dry (still reloads)
    pistol.reloadSeconds = 1.0f;
    pistol.firstShotInaccuracyDeg = 0.035f;
    pistol.sprayInaccuracyDeg = 0.06f;
    pistol.moveInaccuracyDeg = 0.55f;
    pistol.recoilPitchScale = 0.55f;
    pistol.recoilYawScale = 0.40f;
    pistol.recoilRecoveryRate = 16.0f;
    pistol.recoilResetSeconds = 0.22f;
    pistol.cameraKickRecovery = 24.0f;
    pistol.cameraKick = 2.2f;
    pistol.fovPunch = 1.4f;
    pistol.viewmodelKick = 0.70f;
    pistol.viewmodelKickRecovery = 22.0f;
    pistol.viewmodelSideScale = 0.10f;
    pistol.fireVolume = 2.7f;
    pistol.muzzleFlashSeconds = 0.050f;
    pistol.muzzleFlashScale = 0.78f;
    pistol.tracerSeconds = 0.050f;
    pistol.impactScale = 0.78f;
    pistol.tracerWidthScale = 0.58f;
    pistol.tracerAlphaScale = 0.82f;
    pistol.casingScale = 0.72f;
    pistol.recoilPattern = pistolPattern();
    profiles_.push_back(pistol);

    WeaponProfile ak = baseProfile("ak47", WeaponArchetype::HitscanAuto, akVm());
    ak.role = "AK-47 full-auto rifle";
    ak.automatic = true;
    ak.damage = 30.0f;
    ak.fireRate = 10.0f;
    ak.magazine = 30;
    ak.reserve = 90;
    ak.reloadSeconds = 1.35f;
    ak.firstShotInaccuracyDeg = 0.08f;
    ak.sprayInaccuracyDeg = 0.20f;
    ak.moveInaccuracyDeg = 1.10f;
    ak.recoilPitchScale = 0.92f;
    ak.recoilYawScale = 0.46f;
    ak.recoilRecoveryRate = 10.5f;
    ak.recoilResetSeconds = 0.34f;
    ak.cameraKickRecovery = 15.0f;
    ak.cameraKick = 4.9f;
    ak.fovPunch = 3.4f;
    ak.viewmodelKick = 1.0f;
    ak.viewmodelKickRecovery = 14.0f;
    ak.viewmodelSideScale = 0.20f;
    ak.fireVolume = 1.0f;
    ak.muzzleFlashSeconds = 0.060f;
    ak.muzzleFlashScale = 1.18f;
    ak.tracerSeconds = 0.046f;
    ak.impactScale = 1.20f;
    ak.tracerWidthScale = 0.92f;
    ak.tracerAlphaScale = 1.0f;
    ak.casingScale = 1.0f;
    profiles_.push_back(ak);

    WeaponProfile carbine = baseProfile("carbine", WeaponArchetype::Burst, carbineVm());
    carbine.role = "tactical burst carbine";
    carbine.automatic = false;
    carbine.damage = 28.0f;
    carbine.fireRate = 4.3f;
    carbine.burstCount = 3;
    carbine.burstInterval = 0.067f;
    carbine.magazine = 24;
    carbine.reserve = 96;
    carbine.reloadSeconds = 1.35f;
    carbine.spreadDeg = 0.22f;
    carbine.firstShotInaccuracyDeg = 0.045f;
    carbine.sprayInaccuracyDeg = 0.10f;
    carbine.moveInaccuracyDeg = 0.70f;
    carbine.airborneInaccuracyDeg = 0.95f;
    carbine.recoilPitchScale = 0.48f;
    carbine.recoilYawScale = 0.24f;
    carbine.recoilRecoveryRate = 15.0f;
    carbine.recoilResetSeconds = 0.24f;
    carbine.recoilResidualFraction = 0.10f;
    carbine.cameraKickRecovery = 20.0f;
    carbine.cameraKick = 2.9f;
    carbine.fovPunch = 2.0f;
    carbine.viewmodelKick = 0.72f;
    carbine.viewmodelKickRecovery = 20.0f;
    carbine.viewmodelSideScale = 0.12f;
    carbine.fireVolume = 2.35f;
    carbine.muzzleFlashSeconds = 0.048f;
    carbine.muzzleFlashScale = 0.78f;
    carbine.tracerSeconds = 0.040f;
    carbine.impactScale = 0.95f;
    carbine.tracerWidthScale = 0.68f;
    carbine.tracerAlphaScale = 0.82f;
    carbine.casingScale = 0.82f;
    carbine.recoilPattern = burstPattern();
    profiles_.push_back(carbine);

    WeaponProfile smg = baseProfile("pulse_smg", WeaponArchetype::Beam, smgVm());
    smg.role = "mobility spray SMG";
    smg.automatic = true;
    smg.damage = 14.0f;
    smg.fireRate = 15.5f;
    smg.magazine = 45;
    smg.reserve = 135;
    smg.reloadSeconds = 1.45f;
    smg.spreadDeg = 0.55f;
    smg.firstShotInaccuracyDeg = 0.07f;
    smg.sprayInaccuracyDeg = 0.16f;
    smg.moveInaccuracyDeg = 0.45f;
    smg.recoilPitchScale = 0.34f;
    smg.recoilYawScale = 0.32f;
    smg.recoilRecoveryRate = 14.0f;
    smg.recoilResetSeconds = 0.18f;
    smg.cameraKickRecovery = 22.0f;
    smg.cameraKick = 2.1f;
    smg.fovPunch = 1.4f;
    smg.viewmodelKick = 0.55f;
    smg.viewmodelKickRecovery = 24.0f;
    smg.viewmodelSideScale = 0.11f;
    smg.fireVolume = 3.4f;
    smg.muzzleFlashSeconds = 0.034f;
    smg.muzzleFlashScale = 0.58f;
    smg.tracerSeconds = 0.030f;
    smg.impactScale = 0.62f;
    smg.muzzleFlashR = 150.0f;
    smg.muzzleFlashG = 230.0f;
    smg.muzzleFlashB = 255.0f;
    smg.tracerR = 120.0f;
    smg.tracerG = 220.0f;
    smg.tracerB = 255.0f;
    smg.fireVolume = 3.05f;
    smg.muzzleFlashScale = 0.48f;
    smg.tracerWidthScale = 0.46f;
    smg.tracerAlphaScale = 0.68f;
    smg.casingScale = 0.62f;
    smg.recoilPattern = smgPattern();
    profiles_.push_back(smg);

    WeaponProfile machine = smg;
    machine.id = "machine_pistol";
    machine.role = "panic full-auto sidearm";
    machine.rewardEligible = false;
    machine.viewmodel.authored = false;
    machine.damage = 15.0f;
    machine.fireRate = 11.0f;
    machine.magazine = 24;
    machine.reserve = 90;
    machine.reloadSeconds = 1.10f;
    machine.sprayInaccuracyDeg = 0.28f;
    machine.recoilPitchScale = 0.52f;
    machine.recoilYawScale = 0.58f;
    machine.cameraKick = 2.6f;
    machine.viewmodelKick = 0.68f;
    machine.fireVolume = 3.9f;
    profiles_.push_back(machine);

    WeaponProfile scatter = baseProfile("scattergun", WeaponArchetype::Spread, shotgunVm());
    scatter.role = "close pellet control";
    scatter.rewardEligible = true;
    scatter.automatic = false;
    scatter.reloadMode = WeaponReloadMode::PerShell;
    scatter.damage = 11.0f;
    scatter.fireRate = 2.4f;
    scatter.pellets = 7;
    scatter.spreadDeg = 5.0f;
    scatter.magazine = 8;
    scatter.reserve = 32;
    scatter.reloadSeconds = 1.60f;
    scatter.perShellSeconds = 0.45f;
    scatter.cameraKick = 7.0f;
    scatter.fovPunch = 5.0f;
    scatter.viewmodelKick = 1.35f;
    scatter.fireVolume = 4.8f;
    scatter.recoilPattern = heavyPattern();
    profiles_.push_back(scatter);

    // Marksman: a single-shot, bolt-action PRECISION SNIPER (its own sniper_animated rig), not a
    // burst weapon - high damage, slow cadence, tiny spread, heavy per-shot kick.
    WeaponProfile marksman = baseProfile("marksman", WeaponArchetype::HitscanAuto, sniperVm());
    marksman.role = "bolt-action precision rifle";
    marksman.rewardEligible = true;
    marksman.automatic = false;
    marksman.damage = 100.0f;
    marksman.fireRate = 1.4f;
    marksman.burstCount = 1;
    marksman.spreadDeg = 0.04f;
    marksman.firstShotInaccuracyDeg = 0.0f;
    marksman.sprayInaccuracyDeg = 0.0f;
    marksman.moveInaccuracyDeg = 1.1f;
    marksman.magazine = 5;
    marksman.reserve = 30;
    marksman.reloadSeconds = 2.40f;
    marksman.recoilPitchScale = 0.95f;
    marksman.recoilYawScale = 0.30f;
    marksman.recoilRecoveryRate = 9.0f;
    marksman.recoilResetSeconds = 0.5f;
    marksman.cameraKick = 4.6f;
    marksman.fovPunch = 3.2f;
    marksman.fireVolume = 5.2f;
    marksman.recoilPattern = precisionPattern();
    profiles_.push_back(marksman);

    WeaponProfile rail = baseProfile("railbolt", WeaponArchetype::Projectile, smgVm());
    rail.role = "slow charged splash projectile";
    rail.rewardEligible = false;
    rail.viewmodel.authored = false;
    rail.automatic = false;
    rail.damage = 58.0f;
    rail.fireRate = 1.6f;
    rail.magazine = 6;
    rail.reserve = 24;
    rail.reloadSeconds = 1.80f;
    rail.projectileSpeed = 22.0f;
    rail.splashRadius = 1.8f;
    rail.chargeSeconds = 0.10f;
    rail.cameraKick = 5.5f;
    rail.fovPunch = 4.2f;
    rail.viewmodelKick = 1.25f;
    rail.fireVolume = 4.6f;
    rail.recoilPattern = burstPattern();
    profiles_.push_back(rail);
}

bool WeaponProfileRegistry::loadFromDisk() {
    resetDefaults();
    std::ifstream file;
    for (const std::string& path : candidatePaths()) {
        file.open(path);
        if (file.good()) break;
        file.close();
    }
    if (!file.good()) return false;

    WeaponProfile* current = nullptr;
    std::string line;
    while (std::getline(file, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            current = findMutable(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        if (!current) continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        applyKey(*current, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return true;
}

const WeaponProfile* WeaponProfileRegistry::find(const std::string& id) const {
    for (const WeaponProfile& p : profiles_) if (p.id == id) return &p;
    return nullptr;
}

WeaponProfile* WeaponProfileRegistry::findMutable(const std::string& id) {
    for (WeaponProfile& p : profiles_) if (p.id == id) return &p;
    return nullptr;
}

bool WeaponProfileRegistry::rewardEligible(const std::string& id) const {
    const WeaponProfile* p = find(id);
    return p && p->rewardEligible && p->viewmodel.authored && assetExists(p->viewmodel.assetPath);
}

std::vector<std::string> WeaponProfileRegistry::invalidRewardIds() const {
    std::vector<std::string> out;
    for (const WeaponProfile& p : profiles_) {
        if (p.id != "pistol" && !rewardEligible(p.id)) out.push_back("w:" + p.id);
    }
    return out;
}

std::string WeaponProfileRegistry::dump() const {
    std::ostringstream out;
    out << "Weapon profiles: " << profiles_.size() << "\n";
    for (const WeaponProfile& p : profiles_) {
        out << "  " << std::left << std::setw(15) << p.id
            << " [" << weaponArchetypeName(p.archetype) << "]"
            << " dmg=" << p.damage
            << " rate=" << p.fireRate
            << " mag=" << p.magazine << "/" << p.reserve
            << " auto=" << (p.automatic ? "yes" : "no")
            << " fireVol=" << p.fireVolume
            << " camKick=" << p.cameraKick
            << "/" << p.cameraKickRecovery
            << " flash=" << p.muzzleFlashSeconds
            << "x" << p.muzzleFlashScale
            << " tracer=" << p.tracerSeconds
            << "x" << p.tracerWidthScale
            << " reward=" << (rewardEligible(p.id) || p.id == "pistol" ? "live" : "locked")
            << " vm=" << (p.viewmodel.authored ? p.viewmodel.assetPath : std::string("MISSING-AUTHORED-ASSET"))
            << "\n";
    }
    return out.str();
}

const char* weaponArchetypeName(WeaponArchetype archetype) {
    switch (archetype) {
        case WeaponArchetype::Burst: return "Burst";
        case WeaponArchetype::Spread: return "Spread";
        case WeaponArchetype::Projectile: return "Projectile";
        case WeaponArchetype::Beam: return "Beam";
        case WeaponArchetype::HitscanAuto:
        default: return "HitscanAuto";
    }
}

WeaponArchetype parseWeaponArchetypeName(const std::string& text) {
    const std::string v = lower(trim(text));
    if (v == "burst") return WeaponArchetype::Burst;
    if (v == "spread") return WeaponArchetype::Spread;
    if (v == "projectile") return WeaponArchetype::Projectile;
    if (v == "beam") return WeaponArchetype::Beam;
    return WeaponArchetype::HitscanAuto;
}

const char* weaponEventName(WeaponEvent event) {
    switch (event) {
        case WeaponEvent::Fire: return "fire";
        case WeaponEvent::DryFire: return "dry";
        case WeaponEvent::ReloadStart: return "reload_start";
        case WeaponEvent::ReloadEnd: return "reload_end";
        case WeaponEvent::MagOut: return "mag_out";
        case WeaponEvent::MagIn: return "mag_in";
        case WeaponEvent::Bolt: return "bolt";
        case WeaponEvent::Shell: return "shell";
        case WeaponEvent::Equip: return "equip";
        default: return "unknown";
    }
}

} // namespace pulse

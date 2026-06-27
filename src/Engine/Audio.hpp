#pragma once

// Clean audio backend: WASAPI shared-mode, event-driven, low latency (the "feel"
// pillar). SFX one-shots and short beat-synced music stems are fully decoded from
// assets/audio/ for RT-safe playback. Music is state/intensity driven: the game
// crossfades authored loop layers rather than picking a random long track. Required
// samples still fail loud when missing (PROJECT_RULES). If no audio device is
// available (headless/CI) the system runs silent and never blocks.

#include <atomic>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;

namespace pulse {

enum class SoundEventType {
    Config, Dash, Fire, Hit, Kill, Hurt, DryFire, ReloadStart, ReloadEnd, Pickup
};
constexpr int kSoundEventCount = 10;

enum class WeaponEventType {
    Fire, DryFire, ReloadStart, ReloadEnd, MagOut, MagIn, Bolt, Shell, Equip
};

enum class EnemyEventType {
    Telegraph, Shot, Impact, Beam, Lunge, MeleeHit, Hurt, Death, BossBurst
};

// Player feedback bus: short tactile cues the player reads to confirm their own
// actions and state. Authored as round-robin banks (sfx_fb_<event>[_N].wav) by
// tools/audio/pulse_player_sfx_producer.py and kept distinct from weapon/enemy/world
// banks without using arcade coin/chime language. If a bank is missing they degrade
// gracefully to a sensible one-shot SFX rather than ever going silent.
enum class FeedbackEventType {
    Hitmarker, HitCrit, Kill, KillElite,
    Dash, Jump,
    AbilityTactical, AbilityUltimate, ChargeReady, Explosion,
    ShieldAbsorb, ShieldBreak, LowHealth,
    PickupHealth, PickupShield, PickupAmmo, PickupScrap, PickupPowerup,
    ElementBurn, ElementShock, ElementCryo, ElementCorrode, ElementCombo, ElementLeech,
    UiMove, UiConfirm, UiCancel, UiReward,
    RunWin, RunLose
};
constexpr int kFeedbackEventCount = 30;

enum class MusicState {
    Silent, Hub, Combat, Reward, Boss, RunOver
};

enum class MusicBiome {
    Foundry, Furnace, Reliquary
};
constexpr int kMusicBiomeCount = 3;

enum class MusicStingerType {
    RoomClear, Reward, BossIntro, Overpulse, RunWin, RunLose,
    SectorFoundry, SectorFurnace, SectorReliquary,
    // v4 appended (indices stable, files name-indexed): boss escalation crossings and
    // the DoorsOpen anticipation riser (C2). Priorities sit between BossIntro and Overpulse.
    BossPhase, BossEnrage, Anticipation
};
constexpr int kMusicStingerCount = 12;

// v4 reactive-tension context (primary, non-breaking). The four legacy setters below are
// thin wrappers that fill duress=0 and bossEscalation=(state==Boss?1:0), reproducing v3
// byte-for-byte; only callers that build a MusicContext opt into the v4 treatment
// (low-health submerge + heartbeat, beat/bar-quantized transitions, boss escalation).
struct MusicContext {
    bool       enabled        = false;
    float      bpm            = 140.0f;
    float      baseVolume     = 0.0f;
    float      intensity      = 0.0f;   // existing run/pulse RTPC
    MusicState state          = MusicState::Silent;
    MusicBiome biome          = MusicBiome::Foundry;
    bool       overpulseActive= false;
    float      duress         = 0.0f;   // 0 healthy .. 1 near-death (mixer-side submerge)
    float      bossEscalation = 0.0f;   // 0 phase-1 .. 1 enrage (Boss state only)
};

// Shared SFX-bus reverb (Freeverb-style comb/allpass network). Defined in the .cpp;
// the mixer feeds the summed one-shot SFX bus through it so every cue shares one
// room instead of sounding dry and detached. Music has its own baked-in delay and
// does not pass through here.
struct ReverbState;

// A decoded audio asset: interleaved float PCM at its native rate. SFX are mono,
// the music bed is stereo; the mixer resamples per voice to the device rate.
struct Sample {
    std::vector<float> data;       // interleaved, channels_ per frame
    uint32_t channels = 1;
    uint32_t rate = 48000;
    uint32_t frames() const { return channels ? static_cast<uint32_t>(data.size()) / channels : 0; }
};

class AudioSystem {
public:
    explicit AudioSystem(bool enableDevice = true);
    ~AudioSystem();

    void play(SoundEventType type, float volume);
    void playWeaponEvent(const std::string& weaponId, WeaponEventType event, float volume, int sequenceIndex);
    void playEnemyEvent(const std::string& enemyId, EnemyEventType event, float volume, int sequenceIndex);
    // Spatial overload: the cue is placed in the arena at the ground-plane emitter
    // (emitterX, emitterY) relative to the listener pose set by setListener().
    void playEnemyEvent(const std::string& enemyId, EnemyEventType event, float volume, int sequenceIndex,
                        float emitterX, float emitterY);
    // Set the listener (player) pose for spatialized SFX. fwd/right are ground-plane
    // unit vectors (use the game's fromAngle/rightFromForward so panning matches aim).
    void setListener(float posX, float posY, float fwdX, float fwdY, float rightX, float rightY);
    // Play a player-feedback cue from its round-robin bank (sfx_fb_<event>). Falls back
    // to a sensible one-shot SFX if the authored bank is absent (never silent).
    void playFeedback(FeedbackEventType event, float volume, int sequenceIndex);
    // Play a gun shot from the per-weapon fire bank `bankId` (e.g. "pistol").
    // Named weapons do not fall back to the generic bank; missing banks are caught
    // by weapon validation and play silent rather than shipping the wrong weapon.
    void playFire(const std::string& bankId, float volume, int burstIndex);
    void setMusic(bool enabled, float bpm, float baseVolume, float intensity);
    void setMusicState(bool enabled, float bpm, float baseVolume, float intensity, MusicState state);
    void setMusicContext(bool enabled, float bpm, float baseVolume, float intensity,
                         MusicState state, MusicBiome biome);
    void setMusicContext(bool enabled, float bpm, float baseVolume, float intensity,
                         MusicState state, MusicBiome biome, bool overpulseActive);
    // v4 primary setter: opts into reactive tension (duress submerge + heartbeat), beat/bar
    // quantized structural transitions, and boss escalation. The legacy setters above keep
    // the v3 behavior (immediate transitions, full-band duck, no duress).
    void setMusicContext(const MusicContext& ctx);
    // v4 player mix / accessibility options (read lock-free in mix() via atomics, like sfxGain_).
    // musicDuckDepth 0..1 scales the high-band SFX duck (0 = no music duck). readabilityBoost is a
    // multiplier the game applies to telegraph/confirm cues (stored here for completeness).
    // reducedIntensity caps the duress submerge + enrage loudness. monoDownmix sums L/R at output.
    void setMixOptions(float musicDuckDepth, float readabilityBoost, bool reducedIntensity, bool monoDownmix);
    void playMusicStinger(MusicStingerType type, float volume, bool quantizeToBar);
    // Global gain on the SFX (one-shot voice) bus, 0..1+. Music has its own gain via the
    // setMusicState baseVolume. Default 1.0 leaves the mix identical to before this existed.
    void setSfxGain(float gain) { sfxGain_.store(gain < 0.0f ? 0.0f : gain, std::memory_order_relaxed); }
    // Per-biome ambient bed (spec biome.audio): a steady looping atmosphere pad. biome selects the
    // loaded bed (0 Foundry, 1 Furnace, 2 Reliquary); gain 0 fades it out. The mixer crossfades on
    // a biome change. Soft asset: if the bed wav is absent the bed simply stays silent.
    void setAmbientBed(int biome, float gain) {
        ambientBiome_.store(biome, std::memory_order_relaxed);
        ambientTargetGain_.store(gain < 0.0f ? 0.0f : gain, std::memory_order_relaxed);
    }

    // Bake the synth to the required sample assets (one-shot SFX + adaptive music
    // stems) as 16-bit WAVs under `dir`. Run by `--bake-audio`.
    bool bakeSamples(const std::string& dir);

    // Offline render of the current adaptive music method to a 16-bit stereo WAV.
    bool renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume, float intensity);
    bool renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                          float intensity, MusicState state, MusicBiome biome);
    bool renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                          float intensity, MusicState state, MusicBiome biome, bool overpulseActive);
    // v4 render overload: drives the duress + boss-escalation treatment (used by --music-duress /
    // --music-boss-escalation and the report sweeps). duress/bossEscalation default to v3 (0).
    bool renderMusicToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                          float intensity, MusicState state, MusicBiome biome, bool overpulseActive,
                          float duress, float bossEscalation);
    bool renderMusicStingerToWav(const std::string& path, float seconds, float bpm, float baseVolume,
                                 MusicStingerType type, MusicBiome biome, bool quantizeToBar);
    bool renderShotsToWav(const std::string& path, float seconds, SoundEventType type, float fireRate, float volume,
                          const std::string& fireBankId = std::string());
    bool renderWeaponEventToWav(const std::string& path, float seconds, const std::string& weaponId,
                                WeaponEventType event, float eventRate, float volume);
    bool renderEnemyEventToWav(const std::string& path, float seconds, const std::string& enemyId,
                               EnemyEventType event, float eventRate, float volume,
                               bool spatial = false, float emitterX = 0.0f, float emitterY = 0.0f);
    bool renderFeedbackEventToWav(const std::string& path, float seconds, FeedbackEventType event,
                                  float eventRate, float volume);

    // Deterministic offline capture for headless video runs. Gameplay queues the
    // same events via play()/playWeaponEvent()/playEnemyEvent(); the caller advances the mixer by the
    // simulated frame dt and writes the accumulated stereo mix at the end.
    void beginOfflineCapture(uint32_t sampleRate = 48000);
    void advanceOfflineCapture(float seconds);
    bool writeOfflineCaptureWav(const std::string& path);

private:
    struct Voice {
        const Sample* sample = nullptr;
        double pos = 0.0;       // fractional read position (frames)
        double step = 1.0;      // sample.rate / device rate
        float volume = 1.0f;
        bool active = false;
        // 3D placement, baked once at trigger time (one-shots do not move during their
        // ~0.2 s life). Non-spatial voices stay centred and unfiltered like before.
        bool  spatial = false;
        float gainL = 1.0f, gainR = 1.0f;   // equal-power pan * distance attenuation
        float lpCoef = 1.0f;                // one-pole brightness (1 = transparent, lower = darker/farther)
        float lpZ = 0.0f;                   // one-pole filter state
    };

    struct MusicStingerVoice {
        const Sample* sample = nullptr;
        double pos = 0.0;        // fractional source frame
        double startFrame = 0.0; // output-rate music timeline frame
        float volume = 1.0f;
        int priority = 0;
        MusicStingerType type = MusicStingerType::RoomClear;
        bool active = false;
    };

    // Load the sample bank from assets/audio/ (fails loud if a required asset is
    // missing). Called once when a device is available or an offline capture starts.
    void loadBank();
    // Load a round-robin sample bank: stem.wav (variation 0) + stem_1.wav, _2.wav, ... in order.
    std::vector<Sample> loadVariantBank(const std::string& stem);
    void playSampleBank(const std::vector<Sample>& bank, float volume, int sequenceIndex);

    // Queue one one-shot voice with per-trigger humanization so repeats do not read
    // as a machine: pitchSemis is the +/- random detune in semitones, gainDb the +/-
    // random level in dB. Takes mutex_ internally.
    void enqueueVoice(const Sample& s, float volume, float pitchSemis, float gainDb);
    // As enqueueVoice, but bakes spatial pan/attenuation/distance-filter from the
    // listener pose to the ground-plane emitter (ex, ey) when spatial is true.
    void enqueueVoiceEx(const Sample& s, float volume, float pitchSemis, float gainDb,
                        bool spatial, float ex, float ey);
    void playSampleBankSpatial(const std::vector<Sample>& bank, float volume, int sequenceIndex,
                               float ex, float ey);
    // Shared body for both playEnemyEvent overloads (spatial selects placed vs centred).
    void playEnemyEventImpl(const std::string& enemyId, EnemyEventType event, float volume,
                            int sequenceIndex, bool spatial, float ex, float ey);
    // Advance the humanization RNG and return [-1, 1] (caller holds mutex_).
    float nextJitter();
    // (Re)build the reverb buffers for the current sampleRate_ (caller holds mutex_).
    void ensureReverb();
    // Reset reverb tail + duck/envelope + RNG so offline captures are deterministic
    // and never carry a stale tail between renders (caller holds mutex_).
    void clearMixState();
    bool scheduleMusicStingerLocked(MusicStingerType type, float volume, bool quantizeToBar);
    // Shared body for every music-context setter (caller holds mutex_). v4 selects the
    // quantized-transition + duress + escalation path; v3 callers pass v4=false and
    // duress=0, bossEscalation=(state==Boss?1:0) for byte-identical legacy behavior.
    void updateMusicContextLocked(bool enabled, float bpm, float baseVolume, float intensity,
                                  MusicState state, MusicBiome biome, bool overpulseActive,
                                  float duress, float bossEscalation, bool v4);

    bool initDevice();
    void shutdownDevice();
    void renderThread();
    void mix(float* stereo, uint32_t frames);     // fills interleaved L/R

    // v4 (C1): 7th per-biome stem "duress" opens with the near-death input for composer
    // control, layered over the mixer-side submerge. At duress=0 the layer is silent, so
    // legacy/v3 renders stay byte-identical.
    static constexpr int kMusicLayerCount = 7;
    static constexpr int kMaxMusicStingers = 8;

    // Device.
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice*           device_ = nullptr;
    IAudioClient*        client_ = nullptr;
    IAudioRenderClient*  renderClient_ = nullptr;
    void*                bufferEvent_ = nullptr;   // HANDLE
    uint32_t             sampleRate_ = 48000;
    uint16_t             channels_ = 2;            // device mix channel count
    uint32_t             bufferFrames_ = 0;
    std::thread          thread_;
    std::atomic<bool>    running_{ false };

    // Sample bank (loaded once; immutable after load, so read lock-free in mix()).
    Sample               sfx_[kSoundEventCount];
    std::vector<Sample>  fireVariations_;       // default fire series (sfx_fire[_N].wav)
    std::map<std::string, std::vector<Sample>> namedFire_;  // per-weapon banks (sfx_fire_<id>[_N].wav)
    std::map<std::string, std::vector<Sample>> namedWeaponEvents_; // <weapon>:<event> reload/action banks
    std::map<std::string, std::vector<Sample>> namedEnemyEvents_;  // <enemy>:<event> combat/attack banks
    std::map<std::string, std::vector<Sample>> namedFeedback_;     // sfx_fb_<event> player-feedback banks
    std::array<std::array<Sample, kMusicLayerCount>, kMusicBiomeCount> musicLayers_; // biome-synced stems
    Sample               hubMusicBed_;    // non-combat machine-temple bed
    std::array<Sample, kMusicStingerCount> musicStingers_;
    Sample               ambientBeds_[3];   // per-biome ambient atmosphere loops (0 Foundry,1 Furnace,2 Reliquary)
    std::atomic<int>     ambientBiome_{ -1 };          // requested bed (-1 = none)
    std::atomic<float>   ambientTargetGain_{ 0.0f };   // requested bed gain
    int                  ambientCurBiome_ = -1;        // mixer-thread: bed currently sounding
    double               ambientPos_ = 0.0;            // mixer-thread: bed loop position (output frames)
    float                ambientGain_ = 0.0f;          // mixer-thread: smoothed bed gain
    bool                 enableDevice_ = false;

    // Mixer state (guarded by mutex_).
    std::mutex           mutex_;
    std::vector<Voice>   voices_;
    std::atomic<float>   sfxGain_{ 1.0f };   // global one-shot SFX bus gain (settings volume)
    uint32_t             rngState_ = 0x2545F491u;   // per-trigger humanization RNG (under mutex_)
    std::unique_ptr<ReverbState> reverb_;           // shared SFX-bus reverb (lazy-built per rate)
    float                sfxEnv_ = 0.0f;            // SFX bus envelope follower (drives ducking)
    float                duck_ = 0.0f;             // smoothed music duck amount, 0..1
    // Listener (player) pose for spatialized SFX, ground-plane (guarded by mutex_).
    float                listenerX_ = 0.0f, listenerY_ = 0.0f;
    float                listenerFwdX_ = 0.0f, listenerFwdY_ = 1.0f;
    float                listenerRightX_ = 1.0f, listenerRightY_ = 0.0f;
    bool                 musicEnabled_ = false;
    float                musicBpm_ = 146.0f;
    float                musicVolume_ = 0.0f;
    float                musicIntensity_ = 0.0f;
    MusicState           musicState_ = MusicState::Silent;       // APPLIED state (drives layer targets)
    bool                 musicOverpulseActive_ = false;
    // v4 reactive-tension inputs (guarded by mutex_, slewed in mix() - never quantized).
    float                musicDuress_ = 0.0f;          // 0 healthy .. 1 near-death
    float                musicBossEscalation_ = 0.0f;  // 0 phase-1 .. 1 enrage (Boss only)
    bool                 musicV4_ = false;             // true once a MusicContext setter is used
    // v4 quantized state machine: caller sets the REQUESTED state; mix() swaps the applied
    // musicState_ on the next musical boundary (beat for most, bar for ->Boss).
    MusicState           musicRequestedState_ = MusicState::Silent;
    double               musicPendingApplyFrame_ = -1.0;  // <0 = nothing pending
    // v4 player mix / accessibility options (atomics, read lock-free in mix()).
    std::atomic<float>   musicDuckDepth_{ 1.0f };      // scales the high-band SFX duck (0 = off)
    std::atomic<float>   readabilityBoost_{ 1.0f };    // telegraph/confirm cue multiplier (applied game-side)
    std::atomic<int>     reducedIntensity_{ 0 };       // caps duress submerge + enrage loudness
    std::atomic<int>     monoDownmix_{ 0 };            // sum L/R at the final stage
    // v4 mixer-thread DSP state (deterministic, reset in clearMixState).
    float                musicDuressSm_ = 0.0f;        // smoothed duress for submerge/heartbeat
    float                musDuressLpL_ = 0.0f, musDuressLpR_ = 0.0f;  // one-pole submerge low-pass
    float                duckSplitLpL_ = 0.0f, duckSplitLpR_ = 0.0f;  // ~120 Hz low/high split for the tilted duck
    MusicBiome           musicBiome_ = MusicBiome::Foundry;
    int                  musicCurBiome_ = 0;
    int                  musicPrevBiome_ = -1;
    float                musicBiomeFade_ = 1.0f;   // 0..1 crossfade into musicCurBiome_
    double               musicPos_ = 0.0;        // output-rate timeline position (frames)
    double               musicStateAgeFrames_ = 0.0;
    std::array<float, kMusicLayerCount> musicLayerGains_{};
    std::array<MusicStingerVoice, kMaxMusicStingers> musicStingerVoices_{};
    std::array<double, kMusicStingerCount> musicStingerLastStart_{};

    bool                 offlineCapture_ = false;
    double               offlineFrameCarry_ = 0.0;
    std::vector<float>   offlineStereo_;
};

} // namespace pulse

# PULSE - Roguelike Structure Plan (path, economy, deals, heat)

Implementation plan for four roguelike-structure additions on top of the existing run
shell (see `docs/Plan_PULSE_roguelite.md`, Phases A-D). Today a run is a LINEAR sequence
of 9 combat rooms (3 sectors x [2 combat + 1 boss]) with a 1-of-3 reward card after each
and a between-runs hub. The four additions turn that into a decision-driven roguelike:

1. Branching room paths (choose your next room from typed options)
2. In-run economy + shops (scrap from kills, spent mid-run)
3. Risk/reward deals (optional curses-for-power)
4. Heat / ascension modifiers (stackable, unlock-gated difficulty for replay depth)

## Design principles (carry over - do not break)

- Difficulty escalates by COUNT / AGGRESSION / COMPOSITION / enemy DAMAGE, never by
  per-enemy HP sponge. Heat and deals obey this: they scale spawn count, cadence, affix
  rate, enemy damage-to-player, and economy - not enemy HP.
- Meta currency stays UNLOCKS-ONLY (more options, never permanent stat power). The new
  in-run "scrap" is a separate, per-run currency that buys run-scoped power; it never
  persists.
- Determinism: every new roll uses the run-RNG (`run_.rng()`), never the combat-jitter
  `rng_`. The run-RNG reproduces what is OFFERED (path options, shop stock, deal offers,
  heat table). The path actually TAKEN also depends on the player/bot choices (which are
  HP/scrap driven), so a byte-identical replay needs the whole seeded machine (combat
  `rng_` included) - which `--balance-sim` already runs end to end.
- Everything stays headless-verifiable: `--bot-test` and `--balance-sim` (pure CPU, no
  GPU) must exercise the new phases; the bot gets a deterministic policy for each choice.
- Content stays data-driven where it is cheap (room-type weights, shop prices, deal
  catalog, heat table can live in `config/pulse.content` or a sibling once stable).
- Fail loud on missing required assets; new UI degrades gracefully (auto-pick fallback).

## Shared foundation: RunModifiers (build FIRST - features 3 + 4 both need it)

A single aggregate of run-scoped multipliers, mirroring `BuildStats`, recomputed when the
active modifier set changes. Deals and heat are just two sources that push modifiers in.

New file `src/Game/RunMods.{hpp,cpp}` (or fold into `Run.*`):

    enum class ModKind {
        EnemyDamagePct,    // +% damage enemies deal to the player
        EnemyCountPct,     // +% wave count
        EnemyCadencePct,   // -% spawn interval (faster waves)
        EliteChancePct,    // +absolute elite-affix chance
        HealReceivedPct,   // -% healing the player gets (curses/heat)
        ScrapPct,          // +% scrap drops
        RewardTierBias,    // shifts reward rolls toward higher tiers
        MetaPayoutPct,     // +% end-of-run meta currency
        Count
    };
    struct RunModifier { ModKind kind; float value; std::string sourceId; };
    struct RunMods {     // aggregated, combat-ready
        float enemyDamageMult = 1.0f, enemyCountMult = 1.0f, enemyCadenceMult = 1.0f;
        float eliteChanceAdd  = 0.0f, healMult = 1.0f, scrapMult = 1.0f;
        float rewardTierBias  = 0.0f, metaPayoutMult = 1.0f;
    };

- Own a `std::vector<RunModifier> activeMods_` + `RunMods mods_` on `PulseGame` (or `Run`).
  `recomputeMods()` folds the vector into `mods_`. Cleared in `resetRun()`; deals add to it
  mid-run; heat seeds it at `run_.begin`.
- FOLD CONVENTION (define BEFORE M0 - this is the balance-critical detail): every `...Pct`
  mod accumulates ADDITIVELY into its aggregate, never multiplicatively. `recomputeMods()`
  sums each kind's `value` across `activeMods_`, then:
    enemyDamageMult  = 1 + sum(EnemyDamagePct)      // +25% +50% +15% -> 1.90x, NOT 2.16x
    enemyCountMult   = 1 + sum(EnemyCountPct)
    enemyCadenceMult = 1 + sum(EnemyCadencePct)     // interval /= this (bigger -> faster)
    healMult         = clamp(1 + sum(HealReceivedPct), 0, 1)   // curse only ever reduces
    scrapMult        = max(0, 1 + sum(ScrapPct))
    metaPayoutMult   = max(0, 1 + sum(MetaPayoutPct))
    eliteChanceAdd   = sum(EliteChancePct)          // absolute, applied + clamped at the roll
    rewardTierBias   = sum(RewardTierBias)          // absolute bias into the tier weighting
  Additive (not multiplicative) because four systems stack into the same fields and
  compounding explodes; additive keeps the sim's per-mod assert linear and legible.
- READ SITES (surgical except `rollRewards`, which needs an API change - see below):
  - `startWave` (~1938, where `waveSpawnsLeft_` is set from `activeWave_.count`):
    `waveSpawnsLeft_ = round(activeWave_.count * mods_.enemyCountMult)` and scale
    `activeWave_.maxConcurrent` by the same factor (raising count WITHOUT concurrency just
    lengthens the fight - it must add on-screen pressure, not a longer slog).
  - `updateSpawning` / wave streaming (~3234): `interval /= mods_.enemyCadenceMult` only
    (count is already consumed by `startWave` above - do NOT also scale it here, or the
    count mult is either a no-op or double-applied).
  - `spawnEnemy` elite roll (~3379): `eliteChance += mods_.eliteChanceAdd`, then CLAMP the
    final chance (e.g. `min(eliteChance, 0.85f)`) so heat + Elite rooms + the per-sector base
    cannot saturate to all-elite.
  - `damagePlayer` (~3203): incoming `*= mods_.enemyDamageMult`.
  - heal sites - ALL of them, scaling the AMOUNT healed (never a price): `onEnemyKilled`
    HealOnKill/shield (~2755), `updatePickups` health/shield (~3278), `grantReward` HP/shield
    (~1978), and the Shop heal service: `healed = round(base * mods_.healMult)`.
  - `onEnemyKilled` (~2740): scrap award `*= mods_.scrapMult`.
  - `enterRunOver` payout (~1995): `lastPayout_ = round(lastPayout_ * mods_.metaPayoutMult)`.
  - `Build::rollRewards` - NOT a surgical edit. Tier weights are a hardcoded lambda with no
    parameter today; threading `rewardTierBias` in is a public-API change (new signature +
    every caller) plus a concrete transform, e.g. Rare/Uncommon weight `*= (1 + rewardTierBias)`.
    Budget it as API work in M0, not a read-site.
- Verify in isolation: `--balance-sim` with a forced modifier set; assert win-rate moves
  the expected direction (e.g. EnemyDamagePct UP -> win-rate DOWN).

## Feature 1 - Branching room paths + room types

### Data model (`Run.hpp/cpp`)

    enum class RoomType { Combat, Elite, Cache, Shop, Event, Boss };
    // RoomSpec gains:  RoomType type = RoomType::Combat;

Replace the flat `rooms_` with a sequence of CHOICE STEPS (a "choose 1 of N" map - the
core decision without a full 2D node graph, which is far more UI + a later iteration):

    struct RoomStep { std::vector<RoomSpec> options; bool boss = false; };
    std::vector<RoomStep> steps_; int stepIndex_ = 0; int chosen_ = -1;

`Run::begin` builds, per sector: the first room is a fixed Combat (no choice), then
`kChoiceSteps` steps each offering 3 typed options rolled on the run-RNG from a weighted
table (Combat common; Elite/Cache/Shop/Event rarer; never two Shops back to back; at least
one "safe-ish" option per step), then a forced Boss step. Use 3 options, not 2: the
"safe-ish" guarantee on a 2-option step collapses the choice to safe-vs-not (a non-decision);
reserve 2-option steps for deliberate pinch points. Keep the existing escalation curve for
the Combat/Elite waves.

New interface: `currentOptions()`, `chooseOption(int)` -> sets `chosen_`, `currentRoom()`
returns the chosen option, `advanceStep()`. DEFINE the derived accessors explicitly (they are
load-bearing): `roomIndex()` = count of rooms ENTERED along the chosen path (incremented in
`advanceStep`), `sector()` = the sector from the fixed sector layout. This keeps the meta
payout (`roomIndex()*5`, ~1995) and the elite-chance escalation (`0.05*sector`, ~3379)
meaning the same thing regardless of how the branch is shaped.

### Room-type semantics

- Combat: existing waves.
- Elite: fewer enemies, forced EliteAffix on most (reuse the existing `EliteAffix` system),
  higher enemy damage; reward upgraded (guaranteed Uncommon+ or 2 cards) + bonus scrap.
  Bias the forced affixes toward Fast/Volatile/Regen (they change the PUZZLE, not the TTK)
  and cap Shielded density (e.g. at most ~1-in-4 elites Shielded): Shielded halves damage
  taken, so an all-Shielded wave IS an HP sponge by another name - exactly the wall the sim
  caught once, and a violation of the no-sponge principle if left uncapped.
- Cache: no wave (or one light wave); a guaranteed free reward OR a scrap cache. Low risk.
- Shop: no wave; opens the shop (Feature 2).
- Event: no wave; opens a deal (Feature 3).
- Boss: existing boss room.

### State machine (`PulseGame.cpp`)

- New phase `RunPhase::ChoosePath`. After `grantReward` / after a non-combat room resolves,
  instead of jumping straight to `beginRoom`, enter `ChoosePath` and present
  `run_.currentOptions()`. Input keys 1/2/3 -> `run_.chooseOption(pick)` -> `beginRoom()`.
- `beginRoom()` branches on `currentRoom().type`: Combat/Elite -> spawn waves (Elite forces
  affixes via the spawn roll + RunMods); Cache -> grant reward + a short loot dwell; Shop ->
  `enterShop()`; Event -> `enterEvent()`; Boss -> existing.
- The reward step (`enterRoomCleared`) stays, but Elite/Cache tune what `rollRewards` returns
  (count / tier bias).

### UI (`UiDrawList`)

- `ChoosePath` draw: 2-3 room cards (type icon glyph, name, one-line risk/reward), keys 1/2/3
  - mirror the reward-card draw (~4889). Generous auto-pick fallback after a timeout (bot).
- HUD: a compact "run rail" showing the current step + the next TWO steps' option types
  (2-step lookahead). This is what makes a choice a ROUTE, not just "pick the next room's
  flavor" - without lookahead the branching is close to cosmetic. A full node-graph map
  stays later polish, but ship at least 2-step visibility in M1.

### Verify

- `buildBotInput`: in `ChoosePath`, deterministic policy (e.g. prefer Elite when healthy,
  Shop when scrap-rich, Cache when hurt). 
- `--balance-sim`: traverse choices via the bot; print room-type distribution + per-type
  clear rates. Extend the `--bot-test` line with the chosen room-type path.

## Feature 2 - In-run economy + shops

### Scrap currency

- `int scrap_` on `PulseGame`, reset in `resetRun()` (per-run, never persisted).
- `onEnemyKilled` (2740): award scrap by kind/affix/boss (e.g. Rusher 1, Ranged 2, Tank 3,
  elite x2, boss big), `* mods_.scrapMult`, on the run-RNG for small variance. Auto-collect
  (increment + a HUD "+N" flash) first; a physical scrap pickup entity is a later polish.
- HUD: scrap counter near the ammo/score block.

### Shop (`RunPhase::Shop`, entered from a Shop room)

- Stock: 3-4 items rolled via `Build::rollRewards` (mixed passive/weapon), each priced by
  tier (Common ~12, Uncommon ~28, Rare ~60 scrap - tune in sim). Prices scale UP with
  `mods_.scrapMult` so high-heat scrap abundance does not trivialize the shop: purchasing
  power stays roughly constant and buying stays a real decision when scrap is flush. Plus
  service rows:
  - Heal: restore HP for scrap. `mods_.healMult` scales the HP RESTORED, never the price (a
    heal-reduction curse must make healing weaker, not cheaper); the per-HP price also rises
    with `scrapMult`.
  - Reroll: re-roll the stock for a small flat scrap cost (rising each use).
  - (Optional) Remove: delete a passive stack to refine a build. NOTE: needs a new
    `Build::remove(id)` (decrement a stack + `recompute()`) - no such API exists today.
- Buying: `scrap_ >= price` -> grant (`build_.add` / loadout add for weapons) and deduct.
  "Leave" key resolves the room -> `ChoosePath`.
- UI: item rows with name/tier/price + service rows; number keys to buy, a key to leave;
  greys out unaffordable rows (reuse the hub affordable/unaffordable pattern ~4944).

### Verify

- Bot: buy affordable items (prefer higher tier), heal if hurt, then leave.
- `--balance-sim`: track scrap earned/spent, items bought per run, gold-starved vs flush.

## Feature 3 - Risk/reward deals

### Data model

    struct Deal {
        std::string id, name, blurb;
        std::vector<EffectMod>   boon;    // immediate build mods / item grant
        std::vector<RunModifier> curse;   // RunMods pushed for the sector or run
        int boonScrap = 0; bool grantsRare = false; int curseScope = 0; // 0 run, 1 sector
    };

Small built-in deal catalog (data-ify later via a `deal|...` line in `config/pulse.content`).
Examples: "+25% enemy damage this sector -> a guaranteed Rare"; "lose 20 max HP -> +50%
scrap for the run"; "skip this reward -> bank 25 scrap".

### Flow (`RunPhase::Event`, entered from an Event room)

- `enterEvent()` rolls 1-2 deals on the run-RNG. UI shows the deal(s): accept (apply boon
  + push curse RunModifiers via `recomputeMods()`) or decline. Resolve -> `ChoosePath`.
- Curses use the shared RunMods read-sites (Feature foundation), so no new combat hooks.
- Also optional: a "cursed reward card" variant on the normal reward screen (a Rare with a
  small curse) - same Deal data, surfaced in `enterRoomCleared`.

### Verify

- Bot: a policy (accept when the boon is a Rare and HP is high; else decline).
- `--balance-sim`: deal uptake rate vs win-rate, to tune curse/boon strength.

## Feature 4 - Heat / ascension modifiers

### Data model + meta

- A single `int heat_` (0..N) selected before a run; each level adds fixed RunModifiers from
  a HEAT TABLE (L1 +15% enemy damage; L2 +1 enemy/wave i.e. EnemyCountPct; L3 -25% healing;
  L4 +elite chance; L5 faster cadence; ...). Heat is a difficulty/greed TRADE: higher heat
  raises `metaPayoutMult` (the persistent climb incentive) and modestly `scrapMult` /
  `rewardTierBias`.
- MONOTONICITY CONSTRAINT (the unlock gate depends on it): "clear H unlocks H+1" only makes
  sense if heat is NET harder, so win-rate must DECREASE monotonically with heat. The
  difficulty knobs must out-weigh the power knobs - keep `scrapMult`/`rewardTierBias` small
  relative to the damage/count/cadence/elite knobs, and lean on `metaPayoutMult` (which does
  not make the current run easier) for the incentive. `--balance-sim` per heat must confirm
  the monotonic decline; if a mid-heat valley or inversion appears, cut the power knobs at
  that level - do not let better loot make a higher heat easier.
- `Run::begin` seeds the heat modifiers into `activeMods_`.
- Meta persistence (`Meta.*`): add `heat_` (selected) + `maxHeatUnlocked_`. Clearing a run at
  heat H unlocks H+1 (`maxHeatUnlocked_ = max(., H+1)`). Save format: keep the versioned
  profile, ADD keys; the loader already degrades unknown/missing keys to defaults, so old
  saves load with heat 0 (bump "pulse_save_v1" only if a real incompatibility appears).

### Hub UI

- Heat selector: keys to raise/lower heat (clamped to `maxHeatUnlocked_`), a panel listing
  the active heat modifiers + the reward multiplier, before START. `setHeat()` on Meta.

### Verify

- `--balance-sim` PER HEAT level -> win-rate curve (the key balance signal; the sim already
  surfaced a sponge wall once and will tune this). Telemetry: heat + win-rate + payout.
- Two sim-harness fixes this needs (today's harness would give a false curve):
  - A heat-force override that BYPASSES the `maxHeatUnlocked_` clamp - `resetForSim` resets
    meta to fresh (`maxHeatUnlocked_ = 0`), so the hub selector alone can never reach heat>0
    in the sim. Add a `--sim-heat N` (or `forceHeat_`) path used only headless.
  - Scale the per-run timeout with heat (higher heat -> more/queued enemies -> longer runs)
    and record TIMEOUT separately from DEATH. The current 150s cap calls `abandonRun()` and
    would otherwise miscount slow high-heat wins as losses, contaminating the win-rate curve.

## Cross-cutting

- SAVE: extend the Meta profile with heat keys (graceful fallback). No index-keyed data.
- UI: each new phase (`ChoosePath`, `Shop`, `Event`) gets a draw fn mirroring reward/hub;
  HUD adds scrap + heat + run-rail. Keep it text + number keys (the established pattern).
- TELEMETRY: extend the `--bot-test` summary (scrap earned, items bought, deals taken,
  room-type path, heat) and `--balance-sim` (per-heat win rates, room-type mix, economy
  flow). `resetForSim` / `abandonRun` already support headless runs.
- BOT POLICIES: `buildBotInput` gains deterministic handling for ChoosePath / Shop / Event so
  a seed reproduces a full automated run for the sim.
- DATA: room-type weights, shop prices, deal catalog, heat table start in code; promote to
  `config/pulse.content` once the numbers settle. NOTE: that file is a PIPE-DELIMITED record
  parser in `Build.cpp` (`passive|...`, `weapon|...`), NOT the flat key=value of `Config.cpp`;
  each of these is a NEW record type needing a parser extension, so spec the row format when
  promoting (e.g. `heat | L1 | EnemyDamagePct:0.15 EnemyCountPct:... | payout:0.10`). Budget
  it as parser work, not a free config drop.

## Sequencing (each milestone independently headless-verifiable)

- M0  RunMods foundation: struct + the fold convention + read-sites + the `rollRewards`
      tier-bias API change + a forced-mod sim assert.                                    [M]
- M1  Feature 1: RoomType + choice steps + ChoosePath phase + Combat/Elite/Cache.        [L]
      (Shop/Event rooms stubbed -> skip to ChoosePath for now.)
- M2  Feature 2: scrap currency + Shop room/phase + shop UI + bot buy policy.            [M]
- M3  Feature 3: Event room + Deal catalog + accept/decline UI (uses M0 RunMods).        [M]
- M4  Feature 4: heat table + hub heat selector + meta persist + reward scaling.         [M]
- M5  Polish + balance: telemetry, bot policies, `--balance-sim` per heat, tune, data-ify. [M]

Order rationale: M0 unblocks M3/M4; M1 creates the room-type slots M2/M3 fill; M5 tunes the
whole loop with the sim. Each milestone leaves the game playable (`--bot-test` passes) and
keeps `pulse` building.

## Risks / watch-outs

- `PulseGame.cpp` is large and under active edit (viewmodel work). Keep new logic in
  `Run` / `Build` / `Meta` / `RunMods` modules; make the PulseGame edits small and localized
  at the hooks listed above; rebuild + `--bot-test` after each milestone.
- Balance interactions (path x economy x deals x heat) are the real risk. Lean on
  `--balance-sim` after every milestone; never reach for enemy-HP scaling to fix difficulty.
- UI scope creep: ship text-card UIs first (number keys), no new art; a node-graph map and
  scrap-pickup entities are explicit later polish, not M1-M5.
- Determinism: route every new roll through `run_.rng()`; keep combat `rng_` untouched so
  `--balance-sim` stays reproducible.
- Non-monotonic heat: heat hands the player MORE power (loot/scrap) while it adds difficulty,
  so the win-rate-vs-heat curve can be non-monotonic and break the unlock gate. Keep the
  power knobs small, prefer `metaPayoutMult` for the incentive, and verify monotonic decline
  in sim (see Feature 4).
- Economy inflation: count-up + scrapMult-up + Elite scrap bonuses all rise together, so high
  heat is scrap-flush and the shop becomes a non-decision exactly when it should bite. Scale
  shop prices with `scrapMult` to hold purchasing power roughly constant.
- Sim signal integrity: the per-run timeout + `abandonRun` can miscount slow high-heat wins as
  losses; split timeout from death and scale the cap with heat before trusting per-heat curves.

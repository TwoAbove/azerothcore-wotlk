/*
 * mod-tertiary-stats: fabled tier.
 * Fabled effects are rule-changing bonuses rolled as a regular tertiary outcome.
 * A fabled item carries exactly one purple effect line and no stat points; its
 * bonus id lives in the reserved namespace (power_mask = 0): id = effect << 7.
 *
 * Every effect has one home anchor (AnchorFor). Utility anchors host a single
 * effect; Finger and Trinket host pools of combat effects, so the two ring and
 * two trinket slots cap how many combat effects can compound. Anchors with an
 * empty pool roll ordinary stat powers only.
 *
 * Each effect lives in its own fabled_<name>.cpp implementing one Fabled::Script
 * subclass and its own test-harness suite ("fabled-<name>"). The dispatcher in
 * fabled.cpp owns settings, per-player runtime, and hook fan-out; the main module
 * (tertiary_stats.cpp) forwards core script hooks into the Handle* entry points.
 */
#ifndef MOD_TERTIARY_STATS_FABLED_H
#define MOD_TERTIARY_STATS_FABLED_H

#include "DataMap.h"
#include "Define.h"
#include "SharedDefines.h"
#include "ObjectGuid.h"

#include <array>
#include <span>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

class Creature;
class Aura;
class GameObject;
class Item;
class ItemTemplate;
class Player;
class Spell;
class SpellInfo;
class Unit;

enum EnviromentalDamage : int;

namespace Fabled
{
enum class Effect : uint8
{
    None = 0,
    Impact,          // 1  - fall damage becomes an AoE nuke around your landing
    Premonition,     // 2  - XP-eligible killing blows open a free-cost window
    Momentum,        // 3  - kills add move/attack/cast speed stacks on a shared timer
    Ghostwalk,       // 4  - out of combat you fade into a fast spirit
    Cavalier,        // 5  - attack, cast, and interact from a ground mount outdoors
    Keeper,          // 6  - configured consumable categories persist until death
    VengefulGhost,   // 7  - the killing blow leaves you a vengeful spirit instead
    Crossfire,       // 8  - direct damage also strikes your previous target
    AlchemistsGut,   // 9  - potions have no cooldown; each drink stacks Toxicity
    LeviathansGift,  // 10 - waterbreathing, fast swim, abilities work submerged
    Overkill,        // 11 - excess killing-blow damage banks into your next hit
    BloodMagic,      // 12 - missing mana becomes a refreshed self-damage pool
    Warcaster,        // 13 - casts and channels survive movement
    Indomitable,      // 14 - the first hostile hard control each combat fails
    Max
};

constexpr uint8 EFFECT_COUNT = uint8(Effect::Max) - 1;

constexpr uint16 BonusId(Effect effect)
{
    return uint16(uint16(effect) << 7);
}

// Returns Effect::None unless bonusId is a well-formed fabled id.
Effect EffectFromBonusId(uint16 bonusId);
char const* Name(Effect effect);

constexpr uint16 EffectBit(Effect effect)
{
    return uint16(1u << (uint8(effect) - 1));
}
enum class Anchor : uint8
{
    None,
    Head,
    Shoulders,
    Neck,
    Chest,
    Wrists,
    Hands,
    Waist,
    Legs,
    Feet,
    Back,
    Finger,
    Trinket,
    Weapon
};

// The effect's home anchor: the only equipment family it can roll on or be
// reforged onto.
Anchor AnchorFor(Effect effect);
Anchor AnchorFor(ItemTemplate const* itemTemplate);
// Effects that can roll on this anchor. Empty = ordinary powers only.
std::span<Effect const> EffectPoolFor(Anchor anchor);
std::span<Effect const> EffectPoolFor(ItemTemplate const* itemTemplate);
char const* AnchorName(Anchor anchor);
bool CanReforgeTo(Effect effect, ItemTemplate const* itemTemplate);


// Custom spells shipped via the client MPQ patch + server DBC (see
// tools/build-wow-client-patch.py). Validated at startup.
constexpr uint32 SPELL_PREMONITION_AURA = 82011;
constexpr uint32 SPELL_MOMENTUM_AURA = 82012;
constexpr uint32 SPELL_GHOSTWALK_AURA = 82013;
constexpr uint32 SPELL_VENGEFUL_PHASE = 82014;
constexpr uint32 SPELL_TOXICITY = 82015;
constexpr uint32 SPELL_LEVIATHAN_PASSIVE = 82016;
constexpr uint32 SPELL_BLOOD_MAGIC_DEBT = 82017;
constexpr uint32 SPELL_IMPACT_DAMAGE = 82018;
constexpr uint32 SPELL_CROSSFIRE_DAMAGE = 82019;
constexpr uint32 SPELL_VENGEFUL_COOLDOWN = 82020;
constexpr uint32 SPELL_OVERKILL_DAMAGE = 82022;
constexpr uint32 SPELL_VISUAL_KIT_INDOMITABLE = 270;
constexpr uint32 SPELL_FIRST = SPELL_PREMONITION_AURA;
constexpr uint32 SPELL_LAST = SPELL_VENGEFUL_COOLDOWN;

struct Settings
{
    float chance = 2.0f;                     // slice of successful first rolls that become fabled
    std::array<float, 3> chanceMultipliers = { 1.0f, 2.0f, 4.0f };
    bool reforgeEnabled = true;
    float reforgeVendorMultiplier = 5.0f;
    uint32 reforgeMinCost = 10000;
    uint32 reforgeMaxCost = 1000000;

    float impactRadiusYd = 20.0f;

    uint32 premonitionWindowMs = 4000;

    float momentumPctPerStack = 1.0f;
    uint32 momentumMaxStacks = 40;
    uint32 momentumWindowMs = 15000;

    uint32 ghostwalkDelayMs = 6000;
    float ghostwalkSpeedPct = 40.0f;

    uint32 keeperScanIntervalMs = 2000;
    std::string keeperExtraSpells;           // csv of additional spell ids to keep

    uint32 vengefulPhaseMs = 10000;
    uint32 vengefulLockoutMs = 600000;
    float vengefulResHealthPct = 30.0f;

    float crossfirePct = 30.0f;
    float crossfireRangeYd = 40.0f;

    float toxicityPctPerTick = 5.0f;         // % max HP per 3s tick, per stack
    uint32 toxicityDurationMs = 30000;

    float leviathanSwimPct = 60.0f;

    uint32 overkillWindowMs = 15000;

    float bloodMagicHealthPerMana = 4.0f;
    uint32 bloodMagicDebtDurationMs = 10000;
};

Settings const& GetSettings();

// Test support: suites may tweak knobs, restoring them on finish.
Settings& MutableSettings();

struct KeeperAuraIdentity
{
    uint32 spellId = 0;
    ObjectGuid casterGuid;
    ObjectGuid castItemGuid;
    uint64 instanceId = 0;
    uint8 effectMask = 0;
    int32 maxDuration = 0;
    int32 duration = 0;
};

// Per-player fabled state. All *Ms fields are absolute GameTime milliseconds.
struct Runtime : public DataMap::Base
{
    uint16 mask = 0;                         // EffectBit() of equipped effects
    bool wasInCombat = false;
    bool indomitableReady = false;

    uint64 ghostwalkFadeAtMs = 0;            // 0 = not armed


    uint64 keeperNextScanMs = 0;
    std::vector<KeeperAuraIdentity> keeperManaged;

    uint64 vengefulPhaseEndMs = 0;
    bool vengefulPhaseActive = false;

    ObjectGuid crossfirePreviousTarget;
    ObjectGuid crossfireCurrentTarget;

    uint64 overkillBank = 0;
    uint64 overkillExpiresMs = 0;
    SpellSchoolMask overkillSchoolMask = SPELL_SCHOOL_MASK_NONE;
    ObjectGuid overkillPendingTarget;
    uint64 overkillPendingDamage = 0;
    SpellSchoolMask overkillPendingSchoolMask = SPELL_SCHOOL_MASK_NONE;
    uint32 bloodDebtTickAccumulator = 0;
    uint32 bloodDebtTicksRemaining = 0;

    // Test support: pull every pending deadline closer by ms. A deadline that
    // would land in the past becomes 1 (elapsed); zero stays zero (inactive).
    void Advance(uint64 ms);
};

bool Has(Runtime const& runtime, Effect effect);

enum class DamageKind : uint8
{
    Melee,
    Spell,
    Periodic
};

class Script
{
public:
    explicit Script(Effect effect) : _effect(effect) { }
    virtual ~Script() = default;
    Effect GetEffect() const { return _effect; }

    // Equipment/settings refresh. active = the effect is currently equipped.
    // Must be idempotent; apply or remove persistent auras here.
    virtual void OnRefresh(Player* /*player*/, Runtime& /*runtime*/, bool /*active*/) { }
    // Called before an aura is initially applied. Scripts may remove effects.
    virtual void OnModifyAuraEffectMask(Player* /*player*/, Runtime& /*runtime*/,
        Aura const* /*aura*/, uint8& /*effectMask*/) { }


    // Called every player update tick while the effect is equipped.
    virtual void OnUpdate(Player* /*player*/, Runtime& /*runtime*/, uint32 /*diffMs*/, uint64 /*nowMs*/) { }

    // Killing blows attributed to the player (direct, pet, or PvP).
    virtual void OnKill(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/, bool /*xpEligible*/) { }

    // Pre-application damage the player deals; modifiable.
    virtual void OnModifyDealtDamage(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/,
        uint32& /*damage*/, SpellInfo const* /*spellInfo*/, DamageKind /*kind*/) { }

    // Resolved damage the player deals, after all modifiers and mitigation.
    virtual void OnDealtDamageFinal(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/,
        uint32 /*damage*/, SpellInfo const* /*spellInfo*/, DamageKind /*kind*/) { }

    // Damage the player is about to take; modifiable (cheat-death lives here).
    virtual void OnIncomingDamage(Player* /*player*/, Runtime& /*runtime*/, Unit* /*attacker*/, uint32& /*damage*/) { }

    // Environmental damage (fall etc.); runs after Avoidance mitigation.
    virtual void OnEnvironmentalDamage(Player* /*player*/, Runtime& /*runtime*/, EnviromentalDamage /*type*/, uint32& /*damage*/) { }

    // Non-triggered spell lifecycle for the player's own casts.
    virtual void OnSpellCast(Player* /*player*/, Runtime& /*runtime*/, Spell* /*spell*/) { }
    virtual void OnSpellCastCancel(Player* /*player*/, Runtime& /*runtime*/, Spell* /*spell*/) { }

    // Core restriction exemptions (see AllSpellScript::CanCastWhileMoving/Mounted).
    virtual bool CanCastWhileMoving(Player* /*player*/, Runtime& /*runtime*/, Spell const* /*spell*/) { return false; }
    virtual bool CanCastWhileMounted(Player* /*player*/, Runtime& /*runtime*/, Spell const* /*spell*/) { return false; }
    virtual bool CanAttackWhileMounted(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/, bool /*meleeAttack*/) { return false; }
    virtual bool CanUseGameObjectWhileMounted(Player* /*player*/, Runtime& /*runtime*/, GameObject* /*gameObject*/) { return false; }

    // Combat state transitions, derived from the update tick.
    virtual void OnCombatEnter(Player* /*player*/, Runtime& /*runtime*/) { }
    virtual void OnCombatExit(Player* /*player*/, Runtime& /*runtime*/) { }

    // The player died for real (Vengeful Ghost could not or did not intervene).
    virtual void OnDeath(Player* /*player*/, Runtime& /*runtime*/) { }

private:
    Effect _effect;
};

// One factory per fabled_<name>.cpp. Each factory also registers its
// test-harness suite ("fabled-<name>") before returning.
std::unique_ptr<Script> MakeImpact();
std::unique_ptr<Script> MakePremonition();
std::unique_ptr<Script> MakeMomentum();
std::unique_ptr<Script> MakeGhostwalk();
std::unique_ptr<Script> MakeCavalier();
std::unique_ptr<Script> MakeKeeper();
std::unique_ptr<Script> MakeVengefulGhost();
std::unique_ptr<Script> MakeCrossfire();
std::unique_ptr<Script> MakeAlchemistsGut();
std::unique_ptr<Script> MakeLeviathansGift();
std::unique_ptr<Script> MakeOverkill();
std::unique_ptr<Script> MakeBloodMagic();
std::unique_ptr<Script> MakeWarcaster();
std::unique_ptr<Script> MakeIndomitable();

// Dispatcher API used by tertiary_stats.cpp.
void LoadSettings();
bool ValidateSpells();                       // startup DBC check; latches readiness
Runtime* FindRuntime(Player* player);
bool IsReady();
uint64 Now();                                // GameTime in ms
void DealEffectDamage(Player* attacker, Unit* victim, uint64 amount,
    SpellSchoolMask schoolMask, uint32 outputSpellId);
Runtime& GetRuntime(Player* player);
void SetMask(Player* player, uint16 mask);   // from equipment refresh
void HandleUpdate(Player* player, uint32 diffMs);
void HandleKill(Player* killer, Unit* victim);
void HandleModifyDealtDamage(Player* attacker, Unit* victim, uint32& damage,
    SpellInfo const* spellInfo, DamageKind kind);
void HandleDealtDamageFinal(Player* attacker, Unit* victim, uint32 damage,
    SpellInfo const* spellInfo, DamageKind kind);
void HandleIncomingDamage(Player* victim, Unit* attacker, uint32& damage);
void HandleModifyAuraEffectMask(Player* player, Aura const* aura, uint8& effectMask);
void HandleEnvironmentalDamage(Player* player, EnviromentalDamage type, uint32& damage);
void HandleSpellCast(Player* caster, Spell* spell);
void HandleSpellCastCancel(Player* caster, Spell* spell);
bool HandleCanCastWhileMoving(Spell const* spell);
bool HandleCanCastWhileMounted(Spell const* spell);
bool HandleCanAttackWhileMounted(Player* player, Unit* victim, bool meleeAttack);
bool HandleCanUseGameObjectWhileMounted(Player* player, GameObject* gameObject);
void HandleDeath(Player* player);

// Test support.
void AdvanceClock(Player* player, uint64 ms);
}

#endif

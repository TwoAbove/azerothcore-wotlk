/*
 * mod-tertiary-stats: fabled tier.
 * Fabled provenance is a permanent item marker rolled in addition to ordinary
 * tertiary powers. Equipping the item teaches its effect to the character and
 * initially attunes that effect to the item's exact equipment slot. Active
 * attunements suppress the ordinary package in their occupied slots.
 *
 * Every effect has one home anchor (AnchorFor). Utility anchors host a single
 * effect; Finger and Trinket host pools of combat effects, so the two ring and
 * two trinket slots cap how many combat effects can compound. Anchors with an
 * empty pool never roll Fabled provenance.
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
#include <unordered_set>
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

Effect EffectFromItemBonusSeed(uint32 seed);
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
    Weapon,
    Max
};

// Effects and equipment slots retain their natural anchors after the memory is
// learned. An attunement is active only while compatible gear occupies its slot.
Anchor AnchorFor(Effect effect);
Anchor AnchorFor(ItemTemplate const* itemTemplate);
Anchor AnchorForEquipmentSlot(uint8 slot);
// Effects that can roll on this anchor. Empty = ordinary powers only.
std::span<Effect const> EffectPoolFor(Anchor anchor);
std::span<Effect const> EffectPoolFor(ItemTemplate const* itemTemplate);
char const* AnchorName(Anchor anchor);
bool CanAttuneTo(Effect effect, ItemTemplate const* itemTemplate);


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
constexpr uint32 SPELL_VENGEFUL_GUARD = 82023;
constexpr uint32 SPELL_VISUAL_KIT_INDOMITABLE = 270;
constexpr uint32 SPELL_FIRST = SPELL_PREMONITION_AURA;
constexpr uint32 SPELL_LAST = SPELL_VENGEFUL_GUARD;

struct Settings
{
    float chance = 2.0f;                     // chance after the first successful ordinary roll
    std::array<float, 3> chanceMultipliers = { 1.0f, 2.0f, 4.0f };

    float impactRadiusYd = 20.0f;

    uint32 premonitionWindowMs = 4000;

    float momentumPctPerStack = 1.0f;
    uint32 momentumMaxStacks = 40;
    uint32 momentumWindowMs = 15000;

    uint32 ghostwalkDelayMs = 6000;
    float ghostwalkSpeedPct = 40.0f;

    void SetKeeperExtraSpells(std::string csv);
    bool IsKeeperExtraSpell(uint32 spellId) const;

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

private:
    std::unordered_set<uint32> _keeperExtraSpells;
};

Settings const& GetSettings();
Settings const& GetSettings(Player const* player);
Settings& TestSettings(Player* player);
void ClearTestSettings(Player* player);


struct KeeperAuraState
{
    Aura* aura = nullptr;
};

// Per-player fabled state. All *Ms fields are absolute GameTime milliseconds.
struct Runtime : public DataMap::Base
{
    uint16 mask = 0;                         // EffectBit() of active attunements
    std::unique_ptr<Settings> testSettings;  // TestHarness actor-only override.

    struct IndomitableState
    {
        bool ready = false;
    } indomitable;

    struct GhostwalkState
    {
        uint64 fadeAtMs = 0;
    } ghostwalk;

    struct KeeperState
    {
        std::vector<KeeperAuraState> managed;
    } keeper;

    struct VengefulGhostState
    {
        uint64 phaseEndMs = 0;
        bool phaseActive = false;
    } vengefulGhost;

    struct CrossfireState
    {
        ObjectGuid previousTarget;
        ObjectGuid currentTarget;
    } crossfire;

    struct ImpactState
    {
        uint32 pendingFallDamage = 0;
    } impact;

    struct OverkillState
    {
        uint64 bank = 0;
        uint64 expiresMs = 0;
        SpellSchoolMask schoolMask = SPELL_SCHOOL_MASK_NONE;
        struct Discharge
        {
            ObjectGuid target;
            uint64 damage;
            SpellSchoolMask schoolMask;
        };
        std::vector<Discharge> pending;
    } overkill;

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
    // Application lifetime hooks; remove is delivered before the Aura is destroyed.
    virtual void OnAuraApply(Player* /*player*/, Runtime& /*runtime*/, Aura* /*aura*/) { }
    virtual void OnAuraRemove(Player* /*player*/, Runtime& /*runtime*/, Aura* /*aura*/) { }

    // Called every player update tick while the effect is equipped.
    virtual void OnUpdate(Player* /*player*/, Runtime& /*runtime*/, uint32 /*diffMs*/, uint64 /*nowMs*/) { }

    // Killing blows attributed to the player (direct, pet, or PvP).
    virtual void OnKill(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/, bool /*xpEligible*/) { }

    // Resolved damage about to be applied to the victim.
    virtual void OnBeforeDealtDamage(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/,
        uint32 /*damage*/, SpellInfo const* /*spellInfo*/, DamageKind /*kind*/) { }

    // Resolved damage the player deals, after all modifiers and mitigation.
    virtual void OnDealtDamageFinal(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/,
        uint32 /*damage*/, SpellInfo const* /*spellInfo*/, DamageKind /*kind*/) { }

    virtual void ModifySpellEffectImmunityMask(Player* /*player*/, Runtime& /*runtime*/,
        Unit* /*caster*/, SpellInfo const* /*spellInfo*/, uint8 /*candidateEffectMask*/,
        uint8& /*immuneEffectMask*/) { }

    // Environmental damage (fall etc.); runs after Avoidance mitigation.
    virtual void OnEnvironmentalDamage(Player* /*player*/, Runtime& /*runtime*/, EnviromentalDamage /*type*/, uint32& /*damage*/) { }
    virtual void OnAfterMove(Player* /*player*/, Runtime& /*runtime*/, uint32 /*opcode*/) { }

    // Non-triggered spell lifecycle for the player's own casts.
    virtual void OnSpellPrepare(Player* /*player*/, Runtime& /*runtime*/, Spell* /*spell*/) { }
    virtual void OnSpellCastComplete(Player* /*player*/, Runtime& /*runtime*/, Spell* /*spell*/) { }

    // Core restriction exemptions (see AllSpellScript::CanCastWhileMoving/Mounted).
    virtual bool CanCastWhileMoving(Player* /*player*/, Runtime& /*runtime*/, Spell const* /*spell*/) { return false; }
    virtual bool CanCastWhileMounted(Player* /*player*/, Runtime& /*runtime*/, Spell const* /*spell*/) { return false; }
    virtual bool CanAttackWhileMounted(Player* /*player*/, Runtime& /*runtime*/, Unit* /*victim*/, bool /*meleeAttack*/) { return false; }
    virtual bool CanUseGameObjectWhileMounted(Player* /*player*/, Runtime& /*runtime*/, GameObject* /*gameObject*/) { return false; }

    // Native player combat state transitions.
    virtual void OnCombatEnter(Player* /*player*/, Runtime& /*runtime*/) { }
    virtual void OnCombatExit(Player* /*player*/, Runtime& /*runtime*/) { }

    // The player died for real (Vengeful Ghost could not or did not intervene).
    virtual void OnDeath(Player* /*player*/, Runtime& /*runtime*/) { }
    virtual void OnResurrect(Player* /*player*/, Runtime& /*runtime*/) { }

private:
    Effect _effect;
};

// One production factory and one explicit test registration entry point per effect.
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
void RegisterVengefulGhostSpellScripts();
void RegisterBloodMagicSpellScripts();
void RegisterImpactTests();
void RegisterPremonitionTests();
void RegisterMomentumTests();
void RegisterGhostwalkTests();
void RegisterCavalierTests();
void RegisterKeeperTests();
void RegisterVengefulGhostTests();
void RegisterCrossfireTests();
void RegisterAlchemistsGutTests();
void RegisterLeviathansGiftTests();
void RegisterOverkillTests();
void RegisterBloodMagicTests();
void RegisterWarcasterTests();
void RegisterIndomitableTests();

// Per-character Fabled memories. Item markers are permanent provenance; the
// learned effect and active equipment slot live in the characters database.
void LoadAttunements(Player* player);
uint16 UnlockedEffects(Player* player);
bool IsUnlocked(Player* player, Effect effect);
Effect AttunedEffect(Player* player, uint8 slot);
bool SetAttunement(Player* player, Effect effect, uint8 slot, std::string& message);
bool ClearAttunement(Player* player, uint8 slot, std::string& message);
bool LearnFromEquippedItem(Player* player, Item* item, uint8 slot, std::string& message);
void RegisterAttunementSnapshotParticipant();

// Dispatcher API used by tertiary_stats.cpp.
void RegisterScripts();
void RegisterTests();
void LoadSettings();
bool ValidateSpells();                       // startup DBC check; latches readiness
Runtime* FindRuntime(Player* player);
bool IsReady();
uint64 Now();                                // GameTime in ms
void CastEffectDamage(Player* attacker, Unit* victim, uint64 amount,
    SpellSchoolMask schoolMask, uint32 outputSpellId);
Runtime& GetRuntime(Player* player);
void SetMask(Player* player, uint16 mask);   // from equipment refresh
void HandleUpdate(Player* player, uint32 diffMs);
void HandleCombatEnter(Player* player);
void HandleCombatExit(Player* player);
void HandleKill(Player* killer, Unit* victim);
void HandleBeforeDealtDamage(Player* attacker, Unit* victim, uint32 damage,
    SpellInfo const* spellInfo, DamageKind kind);
void HandleDealtDamageFinal(Player* attacker, Unit* victim, uint32 damage,
    SpellInfo const* spellInfo, DamageKind kind);
void HandleModifySpellEffectImmunityMask(Player* target, Unit* caster,
    SpellInfo const* spellInfo, uint8 candidateEffectMask, uint8& immuneEffectMask);
void HandleAuraApply(Player* player, Aura* aura);
void HandleAuraRemove(Player* player, Aura* aura);
void HandleEnvironmentalDamage(Player* player, EnviromentalDamage type, uint32& damage);
void HandleSpellPrepare(Player* caster, Spell* spell);
void HandleSpellCastComplete(Player* caster, Spell* spell);
void HandleCombatEnter(Player* player);
void HandleCombatExit(Player* player);
bool HandleCanCastWhileMoving(Spell const* spell);
bool HandleCanCastWhileMounted(Spell const* spell);
bool HandleCanAttackWhileMounted(Player* player, Unit* victim, bool meleeAttack);
bool HandleCanUseGameObjectWhileMounted(Player* player, GameObject* gameObject);
void HandleDeath(Player* player);
void HandleResurrect(Player* player);

// Test support.
void AdvanceClock(Player* player, uint64 ms);
}

#endif

/*
 * mod-tertiary-stats: fabled tier dispatcher.
 */
#include "fabled.h"
#include "item_bonus_seed.h"

#include "Config.h"
#include "GameTime.h"
#include "Log.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace Fabled
{
namespace
{
constexpr char RUNTIME_KEY[] = "mod-tertiary-fabled";

using Factory = std::unique_ptr<Script> (*)();
using TestRegistrar = void (*)();

struct EffectDescriptor
{
    Effect effect;
    char const* name;
    Anchor anchor;
    Factory factory;
    TestRegistrar registerTests;
};

constexpr std::array<EffectDescriptor, EFFECT_COUNT> EFFECTS = {{
    { Effect::Impact, "Impact", Anchor::Feet, MakeImpact, RegisterImpactTests },
    { Effect::Premonition, "Premonition", Anchor::Trinket, MakePremonition, RegisterPremonitionTests },
    { Effect::Momentum, "Momentum", Anchor::Finger, MakeMomentum, RegisterMomentumTests },
    { Effect::Ghostwalk, "Ghostwalk", Anchor::Back, MakeGhostwalk, RegisterGhostwalkTests },
    { Effect::Cavalier, "Cavalier", Anchor::Legs, MakeCavalier, RegisterCavalierTests },
    { Effect::Keeper, "Keeper", Anchor::Chest, MakeKeeper, RegisterKeeperTests },
    { Effect::VengefulGhost, "Vengeful Ghost", Anchor::Trinket, MakeVengefulGhost, RegisterVengefulGhostTests },
    { Effect::Crossfire, "Crossfire", Anchor::Finger, MakeCrossfire, RegisterCrossfireTests },
    { Effect::AlchemistsGut, "Alchemist's Gut", Anchor::Trinket, MakeAlchemistsGut, RegisterAlchemistsGutTests },
    { Effect::LeviathansGift, "Leviathan's Gift", Anchor::Head, MakeLeviathansGift, RegisterLeviathansGiftTests },
    { Effect::Overkill, "Overkill", Anchor::Weapon, MakeOverkill, RegisterOverkillTests },
    { Effect::BloodMagic, "Blood Magic", Anchor::Finger, MakeBloodMagic, RegisterBloodMagicTests },
    { Effect::Warcaster, "Warcaster", Anchor::Finger, MakeWarcaster, RegisterWarcasterTests },
    { Effect::Indomitable, "Indomitable", Anchor::Trinket, MakeIndomitable, RegisterIndomitableTests }
}};

Settings _settings;
bool _ready = false;
bool _spellsValidated = false;

std::array<std::unique_ptr<Script>, EFFECT_COUNT>& Scripts()
{
    static std::array<std::unique_ptr<Script>, EFFECT_COUNT> scripts;
    return scripts;
}

EffectDescriptor const* DescriptorFor(Effect effect)
{
    if (effect == Effect::None || effect >= Effect::Max)
        return nullptr;
    return &EFFECTS[uint8(effect) - 1];
}

template <typename Fn>
void ForEachActive(Runtime& runtime, Fn&& fn)
{
    if (!runtime.mask)
        return;

    for (uint8 index = 0; index < EFFECT_COUNT; ++index)
        if (runtime.mask & (1u << index))
            fn(*Scripts()[index]);
}

class FabledMovementScript final : public PlayerScript
{
public:
    FabledMovementScript() : PlayerScript("tertiary_fabled_movement", { PLAYERHOOK_ON_AFTER_MOVE }) { }

    void OnPlayerAfterMove(Player* player, uint32 opcode) override
    {
        Runtime* runtime = _ready ? FindRuntime(player) : nullptr;
        if (runtime && runtime->mask)
            ForEachActive(*runtime, [&](Script& script) { script.OnAfterMove(player, *runtime, opcode); });
    }
};

void AdvanceDeadline(uint64& deadline, uint64 ms)
{
    if (!deadline)
        return;
    deadline = deadline > ms ? deadline - ms : 1;
}
} // namespace

Effect EffectFromItemBonusSeed(uint32 seed)
{
    if ((seed >> ITEM_BONUS_SEED_VERSION_SHIFT) != ITEM_BONUS_SEED_VERSION)
        return Effect::None;

    uint8 index = ItemBonusFabledEffect(seed);
    return index >= 1 && index <= EFFECT_COUNT ? Effect(index) : Effect::None;
}

char const* Name(Effect effect)
{
    EffectDescriptor const* descriptor = DescriptorFor(effect);
    return descriptor ? descriptor->name : "Unknown";
}

Anchor AnchorFor(Effect effect)
{
    EffectDescriptor const* descriptor = DescriptorFor(effect);
    return descriptor ? descriptor->anchor : Anchor::None;
}

Anchor AnchorFor(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return Anchor::None;

    switch (itemTemplate->InventoryType)
    {
        case INVTYPE_HEAD:
            return Anchor::Head;
        case INVTYPE_SHOULDERS:
            return Anchor::Shoulders;
        case INVTYPE_NECK:
            return Anchor::Neck;
        case INVTYPE_CHEST:
        case INVTYPE_ROBE:
            return Anchor::Chest;
        case INVTYPE_WRISTS:
            return Anchor::Wrists;
        case INVTYPE_HANDS:
            return Anchor::Hands;
        case INVTYPE_WAIST:
            return Anchor::Waist;
        case INVTYPE_LEGS:
            return Anchor::Legs;
        case INVTYPE_FEET:
            return Anchor::Feet;
        case INVTYPE_CLOAK:
            return Anchor::Back;
        case INVTYPE_FINGER:
            return Anchor::Finger;
        case INVTYPE_TRINKET:
            return Anchor::Trinket;
        case INVTYPE_WEAPON:
        case INVTYPE_2HWEAPON:
        case INVTYPE_WEAPONMAINHAND:
        case INVTYPE_WEAPONOFFHAND:
        case INVTYPE_RANGED:
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
            return Anchor::Weapon;
        default:
            return Anchor::None;
    }
}

Anchor AnchorForEquipmentSlot(uint8 slot)
{
    switch (slot)
    {
        case EQUIPMENT_SLOT_HEAD:
            return Anchor::Head;
        case EQUIPMENT_SLOT_NECK:
            return Anchor::Neck;
        case EQUIPMENT_SLOT_SHOULDERS:
            return Anchor::Shoulders;
        case EQUIPMENT_SLOT_CHEST:
            return Anchor::Chest;
        case EQUIPMENT_SLOT_WRISTS:
            return Anchor::Wrists;
        case EQUIPMENT_SLOT_HANDS:
            return Anchor::Hands;
        case EQUIPMENT_SLOT_WAIST:
            return Anchor::Waist;
        case EQUIPMENT_SLOT_LEGS:
            return Anchor::Legs;
        case EQUIPMENT_SLOT_FEET:
            return Anchor::Feet;
        case EQUIPMENT_SLOT_BACK:
            return Anchor::Back;
        case EQUIPMENT_SLOT_FINGER1:
        case EQUIPMENT_SLOT_FINGER2:
            return Anchor::Finger;
        case EQUIPMENT_SLOT_TRINKET1:
        case EQUIPMENT_SLOT_TRINKET2:
            return Anchor::Trinket;
        case EQUIPMENT_SLOT_MAINHAND:
        case EQUIPMENT_SLOT_OFFHAND:
        case EQUIPMENT_SLOT_RANGED:
            return Anchor::Weapon;
        default:
            return Anchor::None;
    }
}

std::span<Effect const> EffectPoolFor(Anchor anchor)
{
    using Pools = std::array<std::vector<Effect>, uint8(Anchor::Max)>;
    static Pools const pools = []
    {
        Pools result;
        for (EffectDescriptor const& descriptor : EFFECTS)
            result[uint8(descriptor.anchor)].push_back(descriptor.effect);
        // Pool order is part of deterministic bonus generation.
        result[uint8(Anchor::Finger)] = {
            Effect::Crossfire, Effect::Momentum, Effect::Warcaster, Effect::BloodMagic
        };
        return result;
    }();

    if (anchor >= Anchor::Max)
        return {};
    return pools[uint8(anchor)];
}

std::span<Effect const> EffectPoolFor(ItemTemplate const* itemTemplate)
{
    return EffectPoolFor(AnchorFor(itemTemplate));
}

char const* AnchorName(Anchor anchor)
{
    switch (anchor)
    {
        case Anchor::Head:
            return "Head";
        case Anchor::Shoulders:
            return "Shoulders";
        case Anchor::Neck:
            return "Neck";
        case Anchor::Chest:
            return "Chest";
        case Anchor::Wrists:
            return "Wrists";
        case Anchor::Hands:
            return "Hands";
        case Anchor::Waist:
            return "Waist";
        case Anchor::Legs:
            return "Legs";
        case Anchor::Feet:
            return "Feet";
        case Anchor::Back:
            return "Back";
        case Anchor::Finger:
            return "Finger";
        case Anchor::Trinket:
            return "Trinket";
        case Anchor::Weapon:
            return "Weapon";
        default:
            return "None";
    }
}

bool CanAttuneTo(Effect effect, ItemTemplate const* itemTemplate)
{
    return effect != Effect::None && AnchorFor(effect) == AnchorFor(itemTemplate);
}


Settings const& GetSettings()
{
    return _settings;
}

Settings const& GetSettings(Player const* player)
{
    Runtime const* runtime = player
        ? player->CustomData.Get<Runtime>(RUNTIME_KEY) : nullptr;
    return runtime && runtime->testSettings ? *runtime->testSettings : _settings;
}

Settings& TestSettings(Player* player)
{
    Runtime& runtime = GetRuntime(player);
    if (!runtime.testSettings)
        runtime.testSettings = std::make_unique<Settings>(_settings);
    return *runtime.testSettings;
}

void ClearTestSettings(Player* player)
{
    if (Runtime* runtime = FindRuntime(player))
        runtime->testSettings.reset();
}

void Runtime::Advance(uint64 ms)
{
    AdvanceDeadline(ghostwalk.fadeAtMs, ms);
    AdvanceDeadline(vengefulGhost.phaseEndMs, ms);
    AdvanceDeadline(overkill.expiresMs, ms);
}

bool Has(Runtime const& runtime, Effect effect)
{
    return effect != Effect::None && effect < Effect::Max
        && (runtime.mask & EffectBit(effect));
}

void RegisterScripts()
{
    auto& scripts = Scripts();
    for (uint8 index = 0; index < EFFECT_COUNT; ++index)
    {
        EffectDescriptor const& descriptor = EFFECTS[index];
        if (uint8(descriptor.effect) != index + 1)
        {
            LOG_FATAL("module", "TertiaryStats: invalid Fabled descriptor order at index {}", index);
            std::abort();
        }
        scripts[index] = descriptor.factory();
        if (!scripts[index] || scripts[index]->GetEffect() != descriptor.effect)
        {
            LOG_FATAL("module", "TertiaryStats: factory mismatch for {}", descriptor.name);
            std::abort();
        }
    }
    new FabledMovementScript();
}

void RegisterTests()
{
    for (EffectDescriptor const& descriptor : EFFECTS)
        descriptor.registerTests();
}

void LoadSettings()
{
    _settings.chance = std::clamp(sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Chance", 2.0f), 0.0f, 100.0f);
    _settings.chanceMultipliers = {
        std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.ChanceMultiplier.Uncommon", 1.0f)),
        std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.ChanceMultiplier.Rare", 2.0f)),
        std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.ChanceMultiplier.Epic", 4.0f))
    };

    _settings.impactRadiusYd = std::max(1.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Impact.RadiusYd", 20.0f));

    _settings.premonitionWindowMs = std::min<uint32>(
        sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Premonition.WindowMs", 4000),
        uint32(std::numeric_limits<int32>::max()));

    _settings.momentumPctPerStack = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Momentum.PctPerStack", 1.0f));
    _settings.momentumMaxStacks = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Momentum.MaxStacks", 40), 1, 255);
    _settings.momentumWindowMs = std::min<uint32>(
        sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Momentum.WindowMs", 15000),
        uint32(std::numeric_limits<int32>::max()));

    _settings.ghostwalkDelayMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Ghostwalk.DelayMs", 6000);
    _settings.ghostwalkSpeedPct = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Ghostwalk.SpeedPct", 40.0f));

    _settings.SetKeeperExtraSpells(
        sConfigMgr->GetOption<std::string>("TertiaryStats.Fabled.Keeper.ExtraSpells", ""));

    _settings.vengefulPhaseMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.VengefulGhost.PhaseMs", 10000);
    _settings.vengefulLockoutMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.VengefulGhost.LockoutMs", 600000);
    _settings.vengefulResHealthPct = std::clamp(sConfigMgr->GetOption<float>("TertiaryStats.Fabled.VengefulGhost.ResHealthPct", 30.0f), 1.0f, 100.0f);

    _settings.crossfirePct = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Crossfire.Pct", 30.0f));
    _settings.crossfireRangeYd = std::max(1.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Crossfire.RangeYd", 40.0f));

    _settings.toxicityPctPerTick = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.AlchemistsGut.PctPerTick", 5.0f));
    _settings.toxicityDurationMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.AlchemistsGut.DurationMs", 30000);

    _settings.leviathanSwimPct = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.LeviathansGift.SwimSpeedPct", 60.0f));

    _settings.overkillWindowMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Overkill.WindowMs", 15000);

    _settings.bloodMagicHealthPerMana = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.BloodMagic.HealthPerMana", 4.0f));
    _settings.bloodMagicDebtDurationMs = std::clamp<uint32>(
        sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.BloodMagic.DebtDurationMs", 10000),
        1000, uint32(std::numeric_limits<int32>::max()));
}

bool ValidateSpells()
{
    if (_spellsValidated)
        return _ready;
    _spellsValidated = true;

    _ready = true;
    for (uint32 spellId = SPELL_FIRST; spellId <= SPELL_LAST; ++spellId)
        if (!sSpellMgr->GetSpellInfo(spellId))
            _ready = false;
    if (!_ready)
        LOG_ERROR("module", "TertiaryStats: fabled spell DBC rows (82011-82023) are missing; fabled effects are disabled");

    return _ready;
}

bool IsReady()
{
    return _ready;
}

uint64 Now()
{
    return uint64(GameTime::GetGameTimeMS().count());
}
void CastEffectDamage(Player* attacker, Unit* victim, uint64 amount,
    SpellSchoolMask schoolMask, uint32 outputSpellId)
{
    if (!attacker || !victim || !victim->IsAlive() || !amount
        || schoolMask == SPELL_SCHOOL_MASK_NONE)
        return;

    CustomSpellValues values;
    values.AddSpellMod(SPELLVALUE_BASE_POINT0, int32(std::min<uint64>(amount,
        uint64(std::numeric_limits<int32>::max()))));
    values.AddSpellMod(SPELLVALUE_SCHOOL_MASK, int32(schoolMask));
    attacker->CastCustomSpell(outputSpellId, values, victim,
        TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS));
}


Runtime* FindRuntime(Player* player)
{
    return player ? player->CustomData.Get<Runtime>(RUNTIME_KEY) : nullptr;
}

Runtime& GetRuntime(Player* player)
{
    return *player->CustomData.GetDefault<Runtime>(RUNTIME_KEY);
}

void SetMask(Player* player, uint16 mask)
{
    if (!_ready)
        mask = 0;

    Runtime* runtime = FindRuntime(player);
    if (!mask && !runtime)
        return;
    if (!runtime)
        runtime = &GetRuntime(player);
    runtime->mask = mask;
    for (uint8 index = 0; index < EFFECT_COUNT; ++index)
        Scripts()[index]->OnRefresh(player, *runtime, (mask & (1u << index)) != 0);
}

void HandleUpdate(Player* player, uint32 diffMs)
{
    if (!_ready)
        return;

    Runtime* runtime = FindRuntime(player);
    if (!runtime || !runtime->mask)
        return;

    uint64 nowMs = Now();
    ForEachActive(*runtime, [&](Script& script) { script.OnUpdate(player, *runtime, diffMs, nowMs); });
}

void HandleKill(Player* killer, Unit* victim)
{
    if (!_ready || !killer || !victim)
        return;

    Runtime* runtime = FindRuntime(killer);
    if (!runtime || !runtime->mask)
        return;

    bool xpEligible = killer->isHonorOrXPTarget(victim);
    ForEachActive(*runtime, [&](Script& script) { script.OnKill(killer, *runtime, victim, xpEligible); });
}

void HandleBeforeDealtDamage(Player* attacker, Unit* victim, uint32 damage,
    SpellInfo const* spellInfo, DamageKind kind)
{
    if (!_ready || !attacker || !victim)
        return;

    Runtime* runtime = FindRuntime(attacker);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnBeforeDealtDamage(attacker, *runtime, victim, damage, spellInfo, kind); });
}

void HandleDealtDamageFinal(Player* attacker, Unit* victim, uint32 damage,
    SpellInfo const* spellInfo, DamageKind kind)
{
    if (!_ready || !attacker || !victim)
        return;

    Runtime* runtime = FindRuntime(attacker);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script)
        {
            script.OnDealtDamageFinal(attacker, *runtime, victim, damage, spellInfo, kind);
        });
}

void HandleModifySpellEffectImmunityMask(Player* target, Unit* caster,
    SpellInfo const* spellInfo, uint8 candidateEffectMask, uint8& immuneEffectMask)
{
    if (!_ready || !target || !spellInfo || !candidateEffectMask)
        return;

    Runtime* runtime = FindRuntime(target);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script)
        {
            script.ModifySpellEffectImmunityMask(target, *runtime, caster, spellInfo,
                candidateEffectMask, immuneEffectMask);
        });
}

void HandleAuraApply(Player* player, Aura* aura)
{
    if (!_ready || !player || !aura)
        return;

    Runtime* runtime = FindRuntime(player);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnAuraApply(player, *runtime, aura); });
}

void HandleAuraRemove(Player* player, Aura* aura)
{
    if (!_ready || !player || !aura)
        return;

    Runtime* runtime = FindRuntime(player);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnAuraRemove(player, *runtime, aura); });
}

void HandleEnvironmentalDamage(Player* player, EnviromentalDamage type, uint32& damage)
{
    if (!_ready || !player)
        return;

    Runtime* runtime = FindRuntime(player);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnEnvironmentalDamage(player, *runtime, type, damage); });
}

void HandleSpellPrepare(Player* caster, Spell* spell)
{
    if (!_ready || !caster || !spell || spell->IsTriggered())
        return;

    Runtime* runtime = FindRuntime(caster);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnSpellPrepare(caster, *runtime, spell); });
}

void HandleSpellCastComplete(Player* caster, Spell* spell)
{
    if (!_ready || !caster || !spell || spell->IsTriggered())
        return;

    Runtime* runtime = FindRuntime(caster);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnSpellCastComplete(caster, *runtime, spell); });
}

void HandleCombatEnter(Player* player)
{
    Runtime* runtime = _ready ? FindRuntime(player) : nullptr;
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnCombatEnter(player, *runtime); });
}

void HandleCombatExit(Player* player)
{
    Runtime* runtime = _ready ? FindRuntime(player) : nullptr;
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnCombatExit(player, *runtime); });
}

bool HandleCanCastWhileMoving(Spell const* spell)
{
    if (!_ready || !spell)
        return false;

    Player* caster = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
    if (!caster)
        return false;

    Runtime* runtime = FindRuntime(caster);
    if (!runtime)
        return false;
    bool allowed = false;
    ForEachActive(*runtime, [&](Script& script) { allowed = allowed || script.CanCastWhileMoving(caster, *runtime, spell); });
    return allowed;
}

bool HandleCanCastWhileMounted(Spell const* spell)
{
    if (!_ready || !spell)
        return false;

    Player* caster = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
    if (!caster)
        return false;

    Runtime* runtime = FindRuntime(caster);
    if (!runtime)
        return false;
    bool allowed = false;
    ForEachActive(*runtime, [&](Script& script) { allowed = allowed || script.CanCastWhileMounted(caster, *runtime, spell); });
    return allowed;
}

bool HandleCanAttackWhileMounted(Player* player, Unit* victim, bool meleeAttack)
{
    if (!_ready || !player || !victim)
        return false;

    Runtime* runtime = FindRuntime(player);
    if (!runtime)
        return false;
    bool allowed = false;
    ForEachActive(*runtime, [&](Script& script)
    {
        allowed = allowed || script.CanAttackWhileMounted(player, *runtime, victim, meleeAttack);
    });
    return allowed;
}

bool HandleCanUseGameObjectWhileMounted(Player* player, GameObject* gameObject)
{
    if (!_ready || !player || !gameObject)
        return false;

    Runtime* runtime = FindRuntime(player);
    if (!runtime)
        return false;
    bool allowed = false;
    ForEachActive(*runtime, [&](Script& script)
    {
        allowed = allowed || script.CanUseGameObjectWhileMounted(player, *runtime, gameObject);
    });
    return allowed;
}

void HandleDeath(Player* player)
{
    if (!_ready || !player)
        return;

    Runtime* runtime = FindRuntime(player);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnDeath(player, *runtime); });
}

void HandleResurrect(Player* player)
{
    if (!_ready || !player)
        return;

    Runtime* runtime = FindRuntime(player);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnResurrect(player, *runtime); });
}

void AdvanceClock(Player* player, uint64 ms)
{
    if (player)
        GetRuntime(player).Advance(ms);
}
} // namespace Fabled

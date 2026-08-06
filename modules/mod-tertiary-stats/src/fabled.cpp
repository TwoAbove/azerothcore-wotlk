/*
 * mod-tertiary-stats: fabled tier dispatcher.
 */
#include "fabled.h"

#include "Config.h"
#include "GameTime.h"
#include "Log.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <limits>

namespace Fabled
{
void LoadKeeperExtraSpells(std::string const& csv);

namespace
{
constexpr char RUNTIME_KEY[] = "mod-tertiary-fabled";

constexpr std::array<char const*, EFFECT_COUNT> EFFECT_NAMES = {
    "Impact", "Premonition", "Momentum", "Ghostwalk", "Cavalier", "Keeper",
    "Vengeful Ghost", "Crossfire", "Alchemist's Gut", "Leviathan's Gift",
    "Overkill", "Blood Magic", "Warcaster", "Indomitable"
};

Settings _settings;
bool _ready = false;
constexpr std::array<Effect, 1> HEAD_EFFECTS = { Effect::LeviathansGift };
constexpr std::array<Effect, 1> CHEST_EFFECTS = { Effect::Keeper };
constexpr std::array<Effect, 1> LEGS_EFFECTS = { Effect::Cavalier };
constexpr std::array<Effect, 1> FEET_EFFECTS = { Effect::Impact };
constexpr std::array<Effect, 1> BACK_EFFECTS = { Effect::Ghostwalk };
constexpr std::array<Effect, 4> FINGER_EFFECTS = {
    Effect::Crossfire, Effect::Momentum, Effect::Warcaster, Effect::BloodMagic
};
constexpr std::array<Effect, 4> TRINKET_EFFECTS = {
    Effect::Premonition, Effect::VengefulGhost, Effect::AlchemistsGut, Effect::Indomitable
};
constexpr std::array<Effect, 1> WEAPON_EFFECTS = { Effect::Overkill };


std::array<std::unique_ptr<Script>, EFFECT_COUNT>& Scripts()
{
    static std::array<std::unique_ptr<Script>, EFFECT_COUNT> scripts = [] {
        std::array<std::unique_ptr<Script>, EFFECT_COUNT> made;
        made[uint8(Effect::Impact) - 1] = MakeImpact();
        made[uint8(Effect::Premonition) - 1] = MakePremonition();
        made[uint8(Effect::Momentum) - 1] = MakeMomentum();
        made[uint8(Effect::Ghostwalk) - 1] = MakeGhostwalk();
        made[uint8(Effect::Cavalier) - 1] = MakeCavalier();
        made[uint8(Effect::Keeper) - 1] = MakeKeeper();
        made[uint8(Effect::VengefulGhost) - 1] = MakeVengefulGhost();
        made[uint8(Effect::Crossfire) - 1] = MakeCrossfire();
        made[uint8(Effect::AlchemistsGut) - 1] = MakeAlchemistsGut();
        made[uint8(Effect::LeviathansGift) - 1] = MakeLeviathansGift();
        made[uint8(Effect::Overkill) - 1] = MakeOverkill();
        made[uint8(Effect::BloodMagic) - 1] = MakeBloodMagic();
        made[uint8(Effect::Warcaster) - 1] = MakeWarcaster();
        made[uint8(Effect::Indomitable) - 1] = MakeIndomitable();
        return made;
    }();
    return scripts;
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

void AdvanceDeadline(uint64& deadline, uint64 ms)
{
    if (!deadline)
        return;
    deadline = deadline > ms ? deadline - ms : 1;
}
} // namespace

Effect EffectFromBonusId(uint16 bonusId)
{
    if (!bonusId || (bonusId & ((1u << 7) - 1)))
        return Effect::None;

    uint16 index = bonusId >> 7;
    if (index < 1 || index > EFFECT_COUNT)
        return Effect::None;

    return Effect(index);
}

char const* Name(Effect effect)
{
    if (effect == Effect::None || effect >= Effect::Max)
        return "Unknown";
    return EFFECT_NAMES[uint8(effect) - 1];
}
Anchor AnchorFor(Effect effect)
{
    switch (effect)
    {
        case Effect::Impact:
            return Anchor::Feet;
        case Effect::Premonition:
            return Anchor::Trinket;
        case Effect::Momentum:
            return Anchor::Finger;
        case Effect::Ghostwalk:
            return Anchor::Back;
        case Effect::Cavalier:
            return Anchor::Legs;
        case Effect::Keeper:
            return Anchor::Chest;
        case Effect::VengefulGhost:
            return Anchor::Trinket;
        case Effect::Crossfire:
            return Anchor::Finger;
        case Effect::AlchemistsGut:
            return Anchor::Trinket;
        case Effect::LeviathansGift:
            return Anchor::Head;
        case Effect::Overkill:
            return Anchor::Weapon;
        case Effect::BloodMagic:
        case Effect::Warcaster:
            return Anchor::Finger;
        case Effect::Indomitable:
            return Anchor::Trinket;
        default:
            return Anchor::None;
    }
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

std::span<Effect const> EffectPoolFor(Anchor anchor)
{
    switch (anchor)
    {
        case Anchor::Head:
            return HEAD_EFFECTS;
        case Anchor::Chest:
            return CHEST_EFFECTS;
        case Anchor::Legs:
            return LEGS_EFFECTS;
        case Anchor::Feet:
            return FEET_EFFECTS;
        case Anchor::Back:
            return BACK_EFFECTS;
        case Anchor::Finger:
            return FINGER_EFFECTS;
        case Anchor::Trinket:
            return TRINKET_EFFECTS;
        case Anchor::Weapon:
            return WEAPON_EFFECTS;
        default:
            return {};
    }
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

bool CanReforgeTo(Effect effect, ItemTemplate const* itemTemplate)
{
    return effect != Effect::None && AnchorFor(effect) == AnchorFor(itemTemplate);
}


Settings const& GetSettings()
{
    return _settings;
}

Settings& MutableSettings()
{
    return _settings;
}

void Runtime::Advance(uint64 ms)
{
    AdvanceDeadline(ghostwalkFadeAtMs, ms);
    AdvanceDeadline(keeperNextScanMs, ms);
    AdvanceDeadline(vengefulPhaseEndMs, ms);
    AdvanceDeadline(overkillExpiresMs, ms);
}

bool Has(Runtime const& runtime, Effect effect)
{
    return effect != Effect::None && effect < Effect::Max
        && (runtime.mask & EffectBit(effect));
}

void LoadSettings()
{
    _settings.chance = std::clamp(sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Chance", 2.0f), 0.0f, 100.0f);
    _settings.chanceMultipliers = {
        std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.ChanceMultiplier.Uncommon", 1.0f)),
        std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.ChanceMultiplier.Rare", 2.0f)),
        std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.ChanceMultiplier.Epic", 4.0f))
    };
    _settings.reforgeEnabled =
        sConfigMgr->GetOption<bool>("TertiaryStats.Fabled.Reforge.Enable", true);
    _settings.reforgeVendorMultiplier = std::max(
        0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Fabled.Reforge.VendorMultiplier", 5.0f));
    _settings.reforgeMinCost =
        sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Reforge.MinCostCopper", 10000);
    _settings.reforgeMaxCost = std::max(
        _settings.reforgeMinCost,
        sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Reforge.MaxCostCopper", 1000000));

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

    _settings.keeperScanIntervalMs = std::max<uint32>(500, sConfigMgr->GetOption<uint32>("TertiaryStats.Fabled.Keeper.ScanIntervalMs", 2000));
    _settings.keeperExtraSpells = sConfigMgr->GetOption<std::string>("TertiaryStats.Fabled.Keeper.ExtraSpells", "");
    LoadKeeperExtraSpells(_settings.keeperExtraSpells);

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
    Scripts(); // construct effect scripts; factories register their test suites

    _ready = true;
    for (uint32 spellId = SPELL_FIRST; spellId <= SPELL_LAST; ++spellId)
        if (!sSpellMgr->GetSpellInfo(spellId))
            _ready = false;
    if (!sSpellMgr->GetSpellInfo(SPELL_OVERKILL_DAMAGE))
        _ready = false;

    if (!_ready)
        LOG_ERROR("module", "TertiaryStats: fabled spell DBC rows (82011-82020, 82022) are missing; fabled effects are disabled");

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
void DealEffectDamage(Player* attacker, Unit* victim, uint64 amount,
    SpellSchoolMask schoolMask, uint32 outputSpellId)
{
    if (!attacker || !victim || !victim->IsAlive() || !amount
        || schoolMask == SPELL_SCHOOL_MASK_NONE || victim->IsImmunedToDamage(schoolMask))
        return;

    SpellInfo const* outputSpell = sSpellMgr->GetSpellInfo(outputSpellId);
    if (!outputSpell)
        return;

    uint32 damage = uint32(std::min<uint64>(amount, std::numeric_limits<uint32>::max()));
    SpellNonMeleeDamage damageInfo(attacker, victim, outputSpell, schoolMask);
    attacker->CalculateSpellDamageTaken(&damageInfo, int32(std::min<uint32>(
        damage, uint32(std::numeric_limits<int32>::max()))), outputSpell);
    Unit::DealDamageMods(victim, damageInfo.damage, &damageInfo.absorb);
    attacker->SendSpellNonMeleeDamageLog(&damageInfo);
    attacker->DealSpellDamage(&damageInfo, false);
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

    bool inCombat = player->IsInCombat();
    if (inCombat != runtime->wasInCombat)
    {
        runtime->wasInCombat = inCombat;
        if (inCombat)
            ForEachActive(*runtime, [&](Script& script) { script.OnCombatEnter(player, *runtime); });
        else
            ForEachActive(*runtime, [&](Script& script) { script.OnCombatExit(player, *runtime); });
    }

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

void HandleModifyDealtDamage(Player* attacker, Unit* victim, uint32& damage,
    SpellInfo const* spellInfo, DamageKind kind)
{
    if (!_ready || !attacker || !victim)
        return;

    Runtime* runtime = FindRuntime(attacker);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnModifyDealtDamage(attacker, *runtime, victim, damage, spellInfo, kind); });
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

void HandleIncomingDamage(Player* victim, Unit* attacker, uint32& damage)
{
    if (!_ready || !victim)
        return;

    Runtime* runtime = FindRuntime(victim);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnIncomingDamage(victim, *runtime, attacker, damage); });
}

void HandleModifyAuraEffectMask(Player* player, Aura const* aura, uint8& effectMask)
{
    if (!_ready || !player || !aura || !effectMask)
        return;

    Runtime* runtime = FindRuntime(player);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script)
        {
            script.OnModifyAuraEffectMask(player, *runtime, aura, effectMask);
        });
}

void HandleEnvironmentalDamage(Player* player, EnviromentalDamage type, uint32& damage)
{
    if (!_ready || !player)
        return;

    Runtime* runtime = FindRuntime(player);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnEnvironmentalDamage(player, *runtime, type, damage); });
}


void HandleSpellCast(Player* caster, Spell* spell)
{
    if (!_ready || !caster || !spell || spell->IsTriggered())
        return;

    Runtime* runtime = FindRuntime(caster);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnSpellCast(caster, *runtime, spell); });
}

void HandleSpellCastCancel(Player* caster, Spell* spell)
{
    if (!_ready || !caster || !spell)
        return;

    Runtime* runtime = FindRuntime(caster);
    if (runtime && runtime->mask)
        ForEachActive(*runtime, [&](Script& script) { script.OnSpellCastCancel(caster, *runtime, spell); });
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

void AdvanceClock(Player* player, uint64 ms)
{
    if (player)
        GetRuntime(player).Advance(ms);
}
} // namespace Fabled

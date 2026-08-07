/*
 * mod-tertiary-stats
 *
 * Equipment acquired from loot, quests, professions, and vendors can receive
 * up to three custom tertiary powers. Each item instance stores one immutable
 * power selection independently of its native random affix. Ordinary equipment
 * stores a fixed point budget; heirlooms resolve their budget at the wearer's
 * level. Equipped point totals are converted once at the character's level. A
 * matching client addon renders tertiary tooltips and the character-sheet summary.
 */

#include "Bag.h"
#include "Cell.h"
#include "CellImpl.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DataMap.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Item.h"
#include "LootMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "fabled.h"
#include "item_bonus_seed.h"
#include "test_harness.h"
#include "Util.h"
#include "WorldPacket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
std::string const STATE_KEY = "mod-tertiary-stats";
std::string const REFORGE_STATE_KEY = "mod-tertiary-stats-reforge";
constexpr std::string_view TERTIARY_PROTOCOL_VERSION = "V1";
std::atomic<uint32> _tertiaryProtocolGeneration{ 0 };
constexpr uint8 MAX_POWERS_PER_ITEM = 3;
constexpr uint8 BONUS_MASK_BITS = 7;
constexpr uint16 MAX_BONUS_POINTS = (1u << (16 - BONUS_MASK_BITS)) - 1;
constexpr uint8 HEIRLOOM_PROFILE_SHIFT = 16;
constexpr uint8 HEIRLOOM_PROFILE_MASK = 0xFF;

enum class HeirloomProfile : uint8
{
    None,
    Shoulder,
    Trinket,
    Weapon1H,
    Primary,
    Ranged,
    Tertiary
};

constexpr uint32 SPELL_AVOIDANCE_PASSIVE = 82001;
constexpr uint32 SPELL_FLEETFOOT_GROUND_PASSIVE = 82002;
constexpr uint32 SPELL_FLEETFOOT_FLIGHT_PASSIVE = 82003;
constexpr uint32 SPELL_SIPHON_HEAL = 82004;
constexpr uint32 SPELL_TEMPO = 82005;
constexpr uint32 SPELL_ECHO_DAMAGE = 82006;
constexpr uint32 SPELL_ECHO_HEAL = 82007;
constexpr uint32 SPELL_UNBROKEN = 82008;
constexpr uint32 SPELL_OPPORTUNITY = 82009;
constexpr uint32 SPELL_ECHO_AURA = 82010;
constexpr uint32 SPELL_TEMPO_COOLDOWN = 82021;
constexpr uint32 SPELL_DAZED = 1604;
constexpr uint32 SPELL_BROWN_HORSE = 458;
constexpr uint32 TEST_SUFFIX_ITEM_ENTRY = 25100; // Liege Blade, native random suffix.

constexpr TriggerCastFlags TERTIARY_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);
bool _dbcReady = false;
bool _spellsValidated = false;
bool IsTertiaryOutput(SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->Id >= SPELL_AVOIDANCE_PASSIVE
        && spellInfo->Id <= Fabled::SPELL_OVERKILL_DAMAGE;
}

bool ValidateTertiarySpells()
{
    if (_spellsValidated)
        return _dbcReady;
    _spellsValidated = true;

    _dbcReady = true;
    for (uint32 spellId = SPELL_AVOIDANCE_PASSIVE; spellId <= SPELL_ECHO_AURA; ++spellId)
        if (!sSpellMgr->GetSpellInfo(spellId))
            _dbcReady = false;
    if (!sSpellMgr->GetSpellInfo(SPELL_TEMPO_COOLDOWN))
        _dbcReady = false;

    if (_dbcReady)
        LOG_INFO("module", "TertiaryStats: validated 11 ordinary custom spells");
    else
        LOG_ERROR("module", "TertiaryStats: ordinary custom spell DBC rows are missing; item rolls and effects are disabled");
    return _dbcReady;
}


enum class Power : uint8
{
    Avoidance,
    Fleetfoot,
    Siphon,
    Tempo,
    Echo,
    Unbroken,
    Opportunity,
    Count
};

constexpr uint8 POWER_COUNT = static_cast<uint8>(Power::Count);
constexpr uint8 FIRST_EXOTIC = static_cast<uint8>(Power::Tempo);

constexpr std::array<char const*, POWER_COUNT> POWER_NAMES = {
    "Avoidance", "Fleetfoot", "Siphon", "Tempo", "Echo", "Unbroken", "Opportunity"
};


struct Settings
{
    bool enabled = true;
    float rollChance = 70.0f;
    float ordinaryChance = 70.0f;
    uint32 minQuality = ITEM_QUALITY_UNCOMMON;
    uint32 maxQuality = ITEM_QUALITY_EPIC;
    uint32 minItemLevel = 1;
    std::array<float, POWER_COUNT> weights = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

    float avoidancePerPoint = 0.5f;
    float fleetfootBase = 15.0f;
    float fleetfootPerPoint = 1.1f;
    float siphonBase = 1.0f;
    float siphonPerPoint = 0.2f;

    float tempoSpeedBase = 5.0f;
    float tempoSpeedPerPoint = 1.2f;
    float tempoPpmBase = 3.0f;
    float tempoPpmPerPoint = 0.075f;
    float tempoCooldownBaseMs = 1000.0f;
    float tempoCooldownPerPointMs = 100.0f;
    uint32 tempoDurationMs = 8000;
    uint32 tempoMinCooldownMs = 3000;
    uint32 tempoMaxCooldownMs = 30000;

    float echoPercentBase = 4.0f;
    float echoPercentPerPoint = 0.46f;
    float echoPpmBase = 2.0f;
    float echoPpmPerPoint = 0.05f;
    uint32 echoDurationMs = 6000;
    float echoDamageRadius = 10.0f;
    float echoHealingRadius = 30.0f;

    float unbrokenShieldBase = 10.0f;
    float unbrokenShieldPerPoint = 0.25f;
    float unbrokenHealingPerPoint = 0.5f;
    float unbrokenSpeedPerPoint = 2.0f;
    float unbrokenThreshold = 35.0f;
    uint32 unbrokenDurationMs = 6000;
    uint32 unbrokenCooldownMs = 45000;

    float opportunityPpmBase = 0.5f;
    float opportunityPpmPerPoint = 0.225f;
    uint32 opportunityDurationMs = 8000;
};

struct BonusDefinition
{
    uint8 powerMask = 0;
    uint16 pointsPerPower = 0;
    uint8 fabledEffect = 0;
};

Settings _settings;
uint32 _settingsRevision = 0;
thread_local Settings const* _itemRollTestSettings = nullptr;
thread_local Fabled::Settings const* _fabledRollTestSettings = nullptr;

class ScopedItemRollSettings
{
public:
    ScopedItemRollSettings(Settings const& settings, Fabled::Settings const& fabledSettings)
        : _previous(_itemRollTestSettings), _previousFabled(_fabledRollTestSettings)
    {
        _itemRollTestSettings = &settings;
        _fabledRollTestSettings = &fabledSettings;
    }

    ~ScopedItemRollSettings()
    {
        _itemRollTestSettings = _previous;
        _fabledRollTestSettings = _previousFabled;
    }

private:
    Settings const* _previous;
    Fabled::Settings const* _previousFabled;
};

struct PeriodicAreaState
{
    ObjectGuid ownerGuid;
    ObjectGuid casterGuid;
    uint32 spellId = 0;
    bool damaging = false;
    bool opportunity = false;
};

struct TertiaryState : public DataMap::Base
{
    std::array<uint32, POWER_COUNT> rawPoints{};
    std::array<float, POWER_COUNT> points{};
    uint32 settingsRevision = 0;
    bool needsRefresh = true;
    bool disabled = false;
    bool processing = false;
    bool pendingUnbroken = false;
    std::unique_ptr<Settings> testSettings;

    uint64 lastTempoCheckMs = 0;
    uint64 lastEchoCheckMs = 0;
    uint64 lastOpportunityCheckMs = 0;

    Spell* tempoPendingCast = nullptr;
    uint64 tempoPendingChargeExpiresAtMs = 0;

    bool echoActive = false;
    uint64 echoEndsAtMs = 0;
    float echoPercent = 0.0f;
    uint64 echoDamage = 0;
    uint64 echoHealing = 0;
    ObjectGuid echoDamageAnchor;

    Spell* opportunityCast = nullptr;
    SpellInfo const* opportunityCastInfo = nullptr;
    Spell* opportunityChannel = nullptr;
    SpellInfo const* opportunityChannelInfo = nullptr;
    std::vector<PeriodicAreaState> periodicAreas;
};

struct ProcessingGuard
{
    explicit ProcessingGuard(TertiaryState& state) : _state(state), _previous(state.processing)
    {
        _state.processing = true;
    }

    ~ProcessingGuard()
    {
        _state.processing = _previous;
    }

private:
    TertiaryState& _state;
    bool _previous;
};

float ClampChance(float value)
{
    return std::clamp(value, 0.0f, 100.0f);
}


float FabledRollChance(uint32 quality, Fabled::Settings const& settings)
{
    if (quality < ITEM_QUALITY_UNCOMMON || quality > ITEM_QUALITY_EPIC)
        return 0.0f;

    return ClampChance(settings.chance
        * settings.chanceMultipliers[quality - ITEM_QUALITY_UNCOMMON]);
}

uint32 ClampAuraDuration(uint32 duration)
{
    return std::min(duration, uint32(std::numeric_limits<int32>::max()));
}

constexpr uint8 PowerIndex(Power power)
{
    return static_cast<uint8>(power);
}

constexpr uint8 PowerBit(Power power)
{
    return uint8(1u << PowerIndex(power));
}

bool IsExotic(Power power)
{
    return PowerIndex(power) >= FIRST_EXOTIC;
}

bool IsValidMask(uint8 mask)
{
    if (!mask || (mask & ~uint8((1u << POWER_COUNT) - 1)))
        return false;

    uint8 powers = 0;
    for (uint8 index = 0; index < POWER_COUNT; ++index)
        powers += (mask & (1u << index)) != 0;

    return powers <= MAX_POWERS_PER_ITEM;
}

uint16 EncodeBonusId(uint8 mask, uint32 pointsPerPower)
{
    if (!IsValidMask(mask) || !pointsPerPower || pointsPerPower > MAX_BONUS_POINTS)
        return 0;

    return uint16((pointsPerPower << BONUS_MASK_BITS) | mask);
}

bool DecodeBonusId(uint16 bonusId, BonusDefinition& definition)
{
    if (!bonusId)
        return false;

    if (Fabled::Effect effect = Fabled::EffectFromBonusId(bonusId); effect != Fabled::Effect::None)
    {
        definition = { 0, 0, uint8(effect) };
        return true;
    }

    uint8 mask = uint8(bonusId & ((1u << BONUS_MASK_BITS) - 1));
    uint16 points = bonusId >> BONUS_MASK_BITS;
    if (EncodeBonusId(mask, points) != bonusId)
        return false;

    definition = { mask, points, 0 };
    return true;
}

std::string TertiaryDefinitionWirePayload(uint16 bonusId)
{
    BonusDefinition definition;
    if (!DecodeBonusId(bonusId, definition))
        return {};

    if (definition.fabledEffect)
        return Acore::StringFormat("F:{}", uint32(definition.fabledEffect));

    std::string payload = Acore::StringFormat("O:{}", definition.pointsPerPower);
    for (uint8 powerIndex = 0; powerIndex < POWER_COUNT; ++powerIndex)
        if (definition.powerMask & (1u << powerIndex))
            payload += Acore::StringFormat(":{}", uint32(powerIndex));
    return payload;
}

uint32 NextTertiaryProtocolGeneration()
{
    uint32 generation = _tertiaryProtocolGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!generation)
        generation = _tertiaryProtocolGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    return generation;
}

class BonusRng
{
public:
    BonusRng(uint32 entropy, uint32 itemEntry)
        : _state((uint64(entropy) << 32) | itemEntry)
    {
    }

    bool RollChance(float chance)
    {
        chance = ClampChance(chance);
        if (chance <= 0.0f)
            return false;
        if (chance >= 100.0f)
            return true;
        return Float(0.0f, 100.0f) < chance;
    }

    float Float(float minimum, float maximum)
    {
        return minimum + (maximum - minimum)
            * float(Next() >> 8) / float(1u << 24);
    }

    uint32 Range(uint32 minimum, uint32 maximum)
    {
        return minimum + uint32((uint64(Next()) * (uint64(maximum) - minimum + 1)) >> 32);
    }

private:
    uint32 Next()
    {
        _state += 0x9E3779B97F4A7C15ULL;
        uint64 value = _state;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        value ^= value >> 31;
        return uint32(value);
    }

    uint64 _state;
};

uint16 TertiaryBonusOf(Item const* item);

TertiaryState* FindState(Player* player)
{
    return player ? player->CustomData.Get<TertiaryState>(STATE_KEY) : nullptr;
}

TertiaryState* GetState(Player* player)
{
    return player->CustomData.GetDefault<TertiaryState>(STATE_KEY);
}

Settings const& SettingsFor(TertiaryState const& state)
{
    return state.testSettings ? *state.testSettings : _settings;
}

Settings& TestSettings(Player* player)
{
    TertiaryState* state = GetState(player);
    if (!state->testSettings)
        state->testSettings = std::make_unique<Settings>(_settings);
    return *state->testSettings;
}

void ClearTestSettings(Player* player)
{
    if (TertiaryState* state = FindState(player))
        state->testSettings.reset();
}

uint8 ItemPointIndex(ItemTemplate const* itemTemplate)
{
    switch (itemTemplate->InventoryType)
    {
        case INVTYPE_HEAD:
        case INVTYPE_BODY:
        case INVTYPE_CHEST:
        case INVTYPE_LEGS:
        case INVTYPE_2HWEAPON:
        case INVTYPE_ROBE:
            return 0;
        case INVTYPE_SHOULDERS:
        case INVTYPE_WAIST:
        case INVTYPE_FEET:
        case INVTYPE_HANDS:
        case INVTYPE_TRINKET:
            return 1;
        case INVTYPE_NECK:
        case INVTYPE_WRISTS:
        case INVTYPE_FINGER:
        case INVTYPE_SHIELD:
        case INVTYPE_CLOAK:
        case INVTYPE_HOLDABLE:
            return 2;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
        case INVTYPE_WEAPONOFFHAND:
            return 3;
        case INVTYPE_RANGED:
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
            return 4;
        default:
            return MAX_ITEM_ENCHANTMENT_EFFECTS;
    }
}

HeirloomProfile HeirloomProfileFor(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate || itemTemplate->Quality != ITEM_QUALITY_HEIRLOOM)
        return HeirloomProfile::None;

    uint32 mask = itemTemplate->ScalingStatValue;
    if (mask & 0x00000001)
        return HeirloomProfile::Shoulder;
    if (mask & 0x00000002)
        return HeirloomProfile::Trinket;
    if (mask & 0x00000004)
        return HeirloomProfile::Weapon1H;
    if (mask & 0x00000008)
        return HeirloomProfile::Primary;
    if (mask & 0x00000010)
        return HeirloomProfile::Ranged;
    if (mask & 0x00040000)
        return HeirloomProfile::Tertiary;

    switch (itemTemplate->InventoryType)
    {
        case INVTYPE_SHOULDERS:
            return HeirloomProfile::Shoulder;
        case INVTYPE_TRINKET:
            return HeirloomProfile::Trinket;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
        case INVTYPE_WEAPONOFFHAND:
            return HeirloomProfile::Weapon1H;
        case INVTYPE_RANGED:
        case INVTYPE_THROWN:
        case INVTYPE_RANGEDRIGHT:
            return HeirloomProfile::Ranged;
        case INVTYPE_HEAD:
        case INVTYPE_BODY:
        case INVTYPE_CHEST:
        case INVTYPE_LEGS:
        case INVTYPE_2HWEAPON:
        case INVTYPE_ROBE:
            return HeirloomProfile::Primary;
        default:
            return HeirloomProfile::Tertiary;
    }
}

uint32 HeirloomScalingMask(HeirloomProfile profile)
{
    switch (profile)
    {
        case HeirloomProfile::Shoulder:
            return 0x00000001;
        case HeirloomProfile::Trinket:
            return 0x00000002;
        case HeirloomProfile::Weapon1H:
            return 0x00000004;
        case HeirloomProfile::Primary:
            return 0x00000008;
        case HeirloomProfile::Ranged:
            return 0x00000010;
        case HeirloomProfile::Tertiary:
            return 0x00040000;
        default:
            return 0;
    }
}

uint32 HeirloomPointBudget(HeirloomProfile profile, uint32 level)
{
    ScalingStatValuesEntry const* values =
        sScalingStatValuesStore.LookupEntry(std::clamp<uint32>(level, 1, GT_MAX_LEVEL));
    return values ? values->getssdMultiplier(HeirloomScalingMask(profile)) : 0;
}

uint32 AddHeirloomProfile(uint32 seed, HeirloomProfile profile)
{
    return seed | (uint32(profile) << HEIRLOOM_PROFILE_SHIFT);
}

HeirloomProfile HeirloomProfileFromSeed(uint32 seed)
{
    uint8 profile = uint8((seed >> HEIRLOOM_PROFILE_SHIFT) & HEIRLOOM_PROFILE_MASK);
    return profile <= uint8(HeirloomProfile::Tertiary)
        ? HeirloomProfile(profile) : HeirloomProfile::None;
}

uint32 ItemPointBudget(ItemTemplate const* itemTemplate)
{
    if (!itemTemplate)
        return 0;

    if (HeirloomProfile profile = HeirloomProfileFor(itemTemplate);
        profile != HeirloomProfile::None)
        return HeirloomPointBudget(profile, 1);

    uint8 index = ItemPointIndex(itemTemplate);
    if (index >= MAX_ITEM_ENCHANTMENT_EFFECTS)
        return 0;

    RandomPropertiesPointsEntry const* points = sRandomPropertiesPointsStore.LookupEntry(itemTemplate->ItemLevel);
    if (!points)
        return 0;

    switch (itemTemplate->Quality)
    {
        case ITEM_QUALITY_UNCOMMON:
            return points->UncommonPropertiesPoints[index];
        case ITEM_QUALITY_RARE:
            return points->RarePropertiesPoints[index];
        case ITEM_QUALITY_EPIC:
            return points->EpicPropertiesPoints[index];
        default:
            return 0;
    }
}

bool ItemHasPower(Item const* item, Power power)
{
    BonusDefinition definition;
    return item && DecodeBonusId(TertiaryBonusOf(item), definition)
        && (definition.powerMask & PowerBit(power));
}

bool IsEligibleEquipment(ItemTemplate const* itemTemplate)
{
    if (!_settings.enabled || !_dbcReady || !itemTemplate)
        return false;
    if (itemTemplate->Class != ITEM_CLASS_WEAPON && itemTemplate->Class != ITEM_CLASS_ARMOR)
        return false;
    bool qualityEligible = itemTemplate->Quality == ITEM_QUALITY_HEIRLOOM
        || (itemTemplate->Quality >= _settings.minQuality && itemTemplate->Quality <= _settings.maxQuality);
    if (!qualityEligible || itemTemplate->ItemLevel < _settings.minItemLevel
        || !ItemPointBudget(itemTemplate))
        return false;
    if (itemTemplate->Stackable != 1)
        return false;

    switch (itemTemplate->InventoryType)
    {
        case INVTYPE_NON_EQUIP:
        case INVTYPE_BAG:
        case INVTYPE_BODY:
        case INVTYPE_QUIVER:
        case INVTYPE_RELIC:
        case INVTYPE_TABARD:
            return false;
        default:
            return true;
    }
}

Power PickWeightedPower(BonusRng& rng, uint8 selectedMask, bool wantExotic,
    Settings const& settings)
{
    float total = 0.0f;
    for (uint8 index = 0; index < POWER_COUNT; ++index)
    {
        Power power = Power(index);
        if ((selectedMask & (1u << index)) || IsExotic(power) != wantExotic)
            continue;
        total += std::max(0.0f, settings.weights[index]);
    }

    if (total <= 0.0f)
        return Power::Count;

    float roll = rng.Float(0.0f, total);
    for (uint8 index = 0; index < POWER_COUNT; ++index)
    {
        Power power = Power(index);
        if ((selectedMask & (1u << index)) || IsExotic(power) != wantExotic)
            continue;
        roll -= std::max(0.0f, settings.weights[index]);
        if (roll <= 0.0f)
            return power;
    }

    return Power::Count;
}

Power PickPower(BonusRng& rng, uint8 selectedMask, Settings const& settings)
{
    bool wantExotic = rng.RollChance(100.0f - settings.ordinaryChance);
    Power power = PickWeightedPower(rng, selectedMask, wantExotic, settings);
    if (power == Power::Count)
        power = PickWeightedPower(rng, selectedMask, !wantExotic, settings);
    return power;
}

uint16 RollBonus(ItemTemplate const* itemTemplate, BonusRng& rng,
    Settings const& settings, Fabled::Settings const& fabledSettings)
{
    if (!IsEligibleEquipment(itemTemplate) || !rng.RollChance(settings.rollChance))
        return 0;
    std::span<Fabled::Effect const> fabledPool = Fabled::EffectPoolFor(itemTemplate);
    if (Fabled::IsReady() && !fabledPool.empty()
        && rng.RollChance(FabledRollChance(itemTemplate->Quality, fabledSettings)))
        return Fabled::BonusId(fabledPool[rng.Range(0, uint32(fabledPool.size() - 1))]);

    uint8 mask = 0;
    for (uint8 slot = 0; slot < MAX_POWERS_PER_ITEM; ++slot)
    {
        if (slot && !rng.RollChance(settings.rollChance))
            break;

        Power power = PickPower(rng, mask, settings);
        if (power == Power::Count)
            break;
        mask |= PowerBit(power);
    }

    uint32 pointsPerPower = ItemPointBudget(itemTemplate);
    return EncodeBonusId(mask, pointsPerPower);
}
uint16 TertiaryBonusFromSeed(uint32 seed)
{
    if ((seed >> ITEM_BONUS_SEED_VERSION_SHIFT) != ITEM_BONUS_SEED_VERSION)
        return 0;

    uint16 bonusId = uint16(seed & ITEM_BONUS_ID_MASK);
    BonusDefinition definition;
    return DecodeBonusId(bonusId, definition) ? bonusId : 0;
}

uint16 TertiaryBonusOf(Item const* item)
{
    return item ? TertiaryBonusFromSeed(item->GetBonusSeed()) : 0;
}
uint16 ResolvedTertiaryBonusFromSeed(uint32 seed, Player const* player,
    ItemTemplate const* itemTemplate = nullptr)
{
    uint16 bonusId = TertiaryBonusFromSeed(seed);
    BonusDefinition definition;
    if (!DecodeBonusId(bonusId, definition) || definition.fabledEffect)
        return bonusId;

    HeirloomProfile profile = HeirloomProfileFromSeed(seed);
    if (profile == HeirloomProfile::None)
        profile = HeirloomProfileFor(itemTemplate);
    if (profile == HeirloomProfile::None)
        return bonusId;

    uint32 points = HeirloomPointBudget(profile, player ? player->GetLevel() : 1);
    uint16 resolved = EncodeBonusId(definition.powerMask, points);
    return resolved ? resolved : bonusId;
}

uint16 ResolvedTertiaryBonusOf(Item const* item, Player const* player)
{
    return item
        ? ResolvedTertiaryBonusFromSeed(item->GetBonusSeed(), player, item->GetTemplate())
        : 0;
}


float RatingCost(Player const* player)
{
    uint8 level = std::clamp<uint8>(player->GetLevel(), 1, GT_MAX_LEVEL);
    GtCombatRatingsEntry const* rating =
        sGtCombatRatingsStore.LookupEntry(CR_HASTE_MELEE * GT_MAX_LEVEL + level - 1);
    return rating && rating->ratio > 0.0f ? rating->ratio : 1.0f;
}

bool IsAllStatPercentAura(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        if (effect.MiscValue == -1
            && (effect.ApplyAuraName == SPELL_AURA_MOD_PERCENT_STAT
                || effect.ApplyAuraName == SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE))
            return true;
    }

    return false;
}

float AllStatPercentMultiplier(Player const* player)
{
    return player->GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_PERCENT_STAT, -1)
        * player->GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE, -1);
}


float Points(TertiaryState const& state, Power power)
{
    return state.points[PowerIndex(power)];
}

float LinearValue(float points, float base, float perPoint)
{
    return points > 0.0f ? base + points * perPoint : 0.0f;
}

float Avoidance(TertiaryState const& state)
{
    return Points(state, Power::Avoidance) * _settings.avoidancePerPoint;
}

float Fleetfoot(TertiaryState const& state)
{
    return LinearValue(Points(state, Power::Fleetfoot), _settings.fleetfootBase, _settings.fleetfootPerPoint);
}

float Siphon(TertiaryState const& state)
{
    return LinearValue(Points(state, Power::Siphon), _settings.siphonBase, _settings.siphonPerPoint);
}

float TempoSpeed(TertiaryState const& state)
{
    return LinearValue(Points(state, Power::Tempo), _settings.tempoSpeedBase, _settings.tempoSpeedPerPoint);
}

float TempoPpm(TertiaryState const& state)
{
    Settings const& settings = SettingsFor(state);
    return LinearValue(Points(state, Power::Tempo), settings.tempoPpmBase,
        settings.tempoPpmPerPoint);
}

float TempoCooldownMs(TertiaryState const& state)
{
    return LinearValue(Points(state, Power::Tempo), _settings.tempoCooldownBaseMs,
        _settings.tempoCooldownPerPointMs);
}

float EchoPercent(TertiaryState const& state)
{
    return LinearValue(Points(state, Power::Echo), _settings.echoPercentBase, _settings.echoPercentPerPoint);
}
float EchoPpm(TertiaryState const& state)
{
    Settings const& settings = SettingsFor(state);
    return LinearValue(Points(state, Power::Echo), settings.echoPpmBase,
        settings.echoPpmPerPoint);
}

float OpportunityPpm(TertiaryState const& state)
{
    return LinearValue(Points(state, Power::Opportunity), _settings.opportunityPpmBase,
        _settings.opportunityPpmPerPoint);
}

void CastPassive(Player* player, uint32 spellId, int32 bp0, int32 bp1 = 0, int32 bp2 = 0)
{
    player->RemoveAurasDueToSpell(spellId);
    CustomSpellValues values;
    values.AddSpellMod(SPELLVALUE_BASE_POINT0, bp0);
    values.AddSpellMod(SPELLVALUE_BASE_POINT1, bp1);
    values.AddSpellMod(SPELLVALUE_BASE_POINT2, bp2);
    player->CastCustomSpell(spellId, values, player, TERTIARY_TRIGGER_FLAGS);
}

void RefreshEquipment(Player* player, TertiaryState& state)
{
    state.rawPoints = {};
    uint16 fabledMask = 0;
    float ratingCost = RatingCost(player);
    float allStatMultiplier = AllStatPercentMultiplier(player);
    std::array<uint32, POWER_COUNT> ratingPoints{};

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        if (!SettingsFor(state).enabled || !_dbcReady)
            continue;

        BonusDefinition definition;
        if (!DecodeBonusId(ResolvedTertiaryBonusOf(item, player), definition))
            continue;

        if (definition.fabledEffect)
        {
            fabledMask |= Fabled::EffectBit(Fabled::Effect(definition.fabledEffect));
            continue;
        }

        for (uint8 index = 0; index < POWER_COUNT; ++index)
            if (definition.powerMask & (1u << index))
                ratingPoints[index] += definition.pointsPerPower;
    }

    for (uint8 index = 0; index < POWER_COUNT; ++index)
    {
        state.rawPoints[index] = ratingPoints[index];
        state.points[index] = float(ratingPoints[index]) / ratingCost * allStatMultiplier;
    }

    state.settingsRevision = _settingsRevision;
    state.needsRefresh = false;
    ProcessingGuard guard(state);

    player->RemoveAurasDueToSpell(SPELL_AVOIDANCE_PASSIVE);
    float avoidance = Avoidance(state);
    if (avoidance > 0.0f)
        CastPassive(player, SPELL_AVOIDANCE_PASSIVE, -int32(std::lround(avoidance)));

    player->RemoveAurasDueToSpell(SPELL_FLEETFOOT_GROUND_PASSIVE);
    player->RemoveAurasDueToSpell(SPELL_FLEETFOOT_FLIGHT_PASSIVE);
    float fleetfoot = Fleetfoot(state);
    if (fleetfoot > 0.0f)
    {
        int32 speed = int32(std::lround(fleetfoot));
        CastPassive(player, SPELL_FLEETFOOT_GROUND_PASSIVE, speed, speed, speed);
        CastPassive(player, SPELL_FLEETFOOT_FLIGHT_PASSIVE, speed, speed);
    }

    Fabled::SetMask(player, fabledMask);
}

TertiaryState* EnsureState(Player* player)
{
    TertiaryState* state = GetState(player);
    if (state->needsRefresh || state->settingsRevision != _settingsRevision)
        RefreshEquipment(player, *state);
    return state;
}

void SendTertiarySnapshotMessage(Player* player, std::string const& payload)
{
    std::string message = Acore::StringFormat("TStats\t{}:{}", TERTIARY_PROTOCOL_VERSION, payload);
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON, player, player, message);
    player->SendDirectMessage(&packet);
}

void SendTertiaryInventorySnapshot(Player* player);

struct ReforgeSelection final : DataMap::Base
{
    ObjectGuid sourceGuid;
    std::vector<ObjectGuid> menuGuids;
};

std::vector<Item*> CarriedItems(Player* player)
{
    std::vector<Item*> items;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back(item);

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = player->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (Item* item = bag->GetItemByPos(slot))
                items.push_back(item);
    }
    return items;
}

Item* FindCarriedItem(Player* player, ObjectGuid guid)
{
    for (Item* item : CarriedItems(player))
        if (item->GetGUID() == guid)
            return item;
    return nullptr;
}

uint32 FabledReforgeCost(ItemTemplate const* itemTemplate)
{
    Fabled::Settings const& settings = Fabled::GetSettings();
    uint64 scaled = uint64(std::llround(double(itemTemplate->SellPrice)
        * double(settings.reforgeVendorMultiplier)));
    return uint32(std::clamp<uint64>(scaled, settings.reforgeMinCost, settings.reforgeMaxCost));
}

std::string MoneyText(uint32 copper)
{
    uint32 gold = copper / GOLD;
    uint32 silver = (copper % GOLD) / SILVER;
    uint32 remainder = copper % SILVER;
    return Acore::StringFormat("{}g {}s {}c", gold, silver, remainder);
}

bool ValidateReforgeContext(Player* player, std::string& error)
{
    if (!Fabled::GetSettings().reforgeEnabled)
        error = "Fabled reforging is currently disabled.";
    else if (!player->IsAlive())
        error = "You must be alive to reforge an item.";
    else if (player->IsInCombat())
        error = "You cannot reforge an item while in combat.";
    else if (player->GetTradeData())
        error = "You cannot reforge an item while trading.";
    else
        return true;
    return false;
}

bool ValidateReforgeItems(Player* player, Item* source, Item* destination,
    Fabled::Effect& effect, uint32& cost, std::string& error)
{
    if (!source || !destination || source == destination)
    {
        error = "The selected items are no longer available in your carried bags.";
        return false;
    }
    if (source->GetOwnerGUID() != player->GetGUID() || destination->GetOwnerGUID() != player->GetGUID()
        || source->GetCount() != 1 || destination->GetCount() != 1
        || source->IsInTrade() || destination->IsInTrade())
    {
        error = "Both items must be unstacked, unlocked, and owned by you.";
        return false;
    }

    effect = Fabled::EffectFromBonusId(TertiaryBonusOf(source));
    if (effect == Fabled::Effect::None)
    {
        error = "The source item no longer has a Fabled effect.";
        return false;
    }
    if (Fabled::EffectFromBonusId(TertiaryBonusOf(destination)) != Fabled::Effect::None)
    {
        error = "The destination item is already Fabled.";
        return false;
    }
    if (!Fabled::CanReforgeTo(effect, destination->GetTemplate()))
    {
        error = "That Fabled effect cannot be applied to the selected item.";
        return false;
    }

    cost = FabledReforgeCost(destination->GetTemplate());
    if (!player->HasEnoughMoney(cost))
    {
        error = "You do not have enough money for this reforge.";
        return false;
    }
    return true;
}

bool ReforgeFabled(Player* player, ObjectGuid sourceGuid, ObjectGuid destinationGuid,
    std::string& message)
{
    if (!ValidateReforgeContext(player, message))
        return false;

    Item* source = FindCarriedItem(player, sourceGuid);
    Item* destination = FindCarriedItem(player, destinationGuid);
    Fabled::Effect effect = Fabled::Effect::None;
    uint32 cost = 0;
    if (!ValidateReforgeItems(player, source, destination, effect, cost, message))
        return false;

    uint8 sourceBag = source->GetBagSlot();
    uint8 sourceSlot = source->GetSlot();
    CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
    destination->SetBonusSeed(MakeItemBonusSeed(Fabled::BonusId(effect)));
    destination->SetBinding(true);
    destination->SetNotRefundable(player, true, &transaction);
    destination->ClearSoulboundTradeable(player, &transaction);
    player->RemoveTradeableItem(destination);
    player->DestroyItem(sourceBag, sourceSlot, true, &transaction);
    player->ModifyMoney(-int32(cost));
    player->SaveInventoryAndGoldToDB(transaction);
    CharacterDatabase.CommitTransaction(transaction);

    SendTertiaryInventorySnapshot(player);
    message = Acore::StringFormat("{} was reforged onto {} for {}. The source item was destroyed.",
        Fabled::Name(effect), destination->GetTemplate()->Name1, MoneyText(cost));
    return true;
}

void SendTertiaryInventorySnapshot(Player* player)
{
    uint32 generation = NextTertiaryProtocolGeneration();
    SendTertiarySnapshotMessage(player, Acore::StringFormat("I:C:{}", generation));

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        std::string definition = TertiaryDefinitionWirePayload(ResolvedTertiaryBonusOf(item, player));
        if (!definition.empty())
            SendTertiarySnapshotMessage(player, Acore::StringFormat("I:E:{}:{}:{}",
                generation, uint32(slot - EQUIPMENT_SLOT_START + 1), definition));
    }

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        std::string definition = TertiaryDefinitionWirePayload(ResolvedTertiaryBonusOf(item, player));
        if (!definition.empty())
            SendTertiarySnapshotMessage(player, Acore::StringFormat("I:B:{}:0:{}:{}",
                generation, uint32(slot - INVENTORY_SLOT_ITEM_START + 1), definition));
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag const* bag = player->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        uint32 clientBag = uint32(bagSlot - INVENTORY_SLOT_BAG_START + 1);
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            Item const* item = bag->GetItemByPos(slot);
            std::string definition = TertiaryDefinitionWirePayload(ResolvedTertiaryBonusOf(item, player));
            if (!definition.empty())
                SendTertiarySnapshotMessage(player, Acore::StringFormat("I:B:{}:{}:{}:{}",
                    generation, clientBag, slot + 1, definition));
        }
    }

    SendTertiarySnapshotMessage(player, Acore::StringFormat("I:D:{}", generation));
}

void SendTertiaryLootSnapshot(Player* player, Loot* loot)
{
    uint32 generation = NextTertiaryProtocolGeneration();
    SendTertiarySnapshotMessage(player, Acore::StringFormat("L:C:{}", generation));
    if (loot)
    {
        for (uint32 slot = 0; slot < loot->GetMaxSlotInLootFor(player); ++slot)
        {
            LootItem const* lootItem = loot->LootItemInSlot(slot, player);
            ItemTemplate const* itemTemplate =
                lootItem ? sObjectMgr->GetItemTemplate(lootItem->itemid) : nullptr;
            std::string definition = lootItem ? TertiaryDefinitionWirePayload(
                ResolvedTertiaryBonusFromSeed(lootItem->bonusSeed, player, itemTemplate)) : "";
            if (!definition.empty())
                SendTertiarySnapshotMessage(player,
                    Acore::StringFormat("L:R:{}:{}:{}", generation, slot + 1, definition));
        }
    }
    SendTertiarySnapshotMessage(player, Acore::StringFormat("L:D:{}", generation));
}

void SendTertiaryAuctionSnapshot(Player* player, uint8 listType,
    AuctionListResultView items)
{
    uint32 generation = NextTertiaryProtocolGeneration();
    SendTertiarySnapshotMessage(player,
        Acore::StringFormat("A:C:{}:{}", generation, uint32(listType)));
    for (uint32 index = 0; index < items.size(); ++index)
    {
        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(items[index].itemEntry);
        std::string definition = TertiaryDefinitionWirePayload(
            ResolvedTertiaryBonusFromSeed(items[index].itemBonusSeed, player, itemTemplate));
        if (!definition.empty())
            SendTertiarySnapshotMessage(player, Acore::StringFormat("A:R:{}:{}:{}:{}",
                generation, uint32(listType), index + 1, definition));
    }
    SendTertiarySnapshotMessage(player,
        Acore::StringFormat("A:D:{}:{}", generation, uint32(listType)));
}

void SendFabledSettingsSnapshot(Player* player)
{
    Fabled::Settings const& settings = Fabled::GetSettings();
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:0:{:.3f}",
        settings.impactRadiusYd));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:1:{}",
        settings.premonitionWindowMs));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:2:{:.3f}:{}:{}",
        settings.momentumPctPerStack, settings.momentumMaxStacks, settings.momentumWindowMs));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:3:{}:{:.3f}",
        settings.ghostwalkDelayMs, settings.ghostwalkSpeedPct));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:6:{}:{}:{:.3f}",
        settings.vengefulPhaseMs, settings.vengefulLockoutMs, settings.vengefulResHealthPct));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:7:{:.3f}:{:.3f}",
        settings.crossfirePct, settings.crossfireRangeYd));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:8:{:.3f}:{}",
        settings.toxicityPctPerTick, settings.toxicityDurationMs));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:9:{:.3f}",
        settings.leviathanSwimPct));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:10:{}",
        settings.overkillWindowMs));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("F:11:{:.3f}:{}",
        settings.bloodMagicHealthPerMana, settings.bloodMagicDebtDurationMs));
}

void SendTertiarySnapshot(Player* player)
{
    TertiaryState const& state = *EnsureState(player);
    SendTertiarySnapshotMessage(player, Acore::StringFormat("S:0:{}:{:.3f}",
        state.rawPoints[PowerIndex(Power::Avoidance)], Avoidance(state)));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("S:1:{}:{:.3f}",
        state.rawPoints[PowerIndex(Power::Fleetfoot)], Fleetfoot(state)));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("S:2:{}:{:.3f}",
        state.rawPoints[PowerIndex(Power::Siphon)], Siphon(state)));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("S:3:{}:{:.3f}:{:.3f}:{:.0f}:{}:{}:{}",
        state.rawPoints[PowerIndex(Power::Tempo)], TempoSpeed(state), TempoPpm(state),
        TempoCooldownMs(state), _settings.tempoDurationMs, _settings.tempoMinCooldownMs,
        _settings.tempoMaxCooldownMs));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("S:4:{}:{:.3f}:{:.3f}:{}:{:.1f}:{:.1f}",
        state.rawPoints[PowerIndex(Power::Echo)], EchoPercent(state), EchoPpm(state),
        _settings.echoDurationMs, _settings.echoDamageRadius, _settings.echoHealingRadius));

    float unbrokenPoints = Points(state, Power::Unbroken);
    SendTertiarySnapshotMessage(player, Acore::StringFormat("S:5:{}:{:.3f}:{:.3f}:{:.3f}:{:.3f}:{}:{}",
        state.rawPoints[PowerIndex(Power::Unbroken)],
        _settings.unbrokenShieldBase + unbrokenPoints * _settings.unbrokenShieldPerPoint,
        unbrokenPoints * _settings.unbrokenHealingPerPoint,
        unbrokenPoints * _settings.unbrokenSpeedPerPoint,
        _settings.unbrokenThreshold, _settings.unbrokenDurationMs, _settings.unbrokenCooldownMs));
    SendTertiarySnapshotMessage(player, Acore::StringFormat("S:6:{}:{:.3f}:{}",
        state.rawPoints[PowerIndex(Power::Opportunity)], OpportunityPpm(state),
        _settings.opportunityDurationMs));
    SendFabledSettingsSnapshot(player);
    SendTertiaryInventorySnapshot(player);
}


bool RollNormalizedProc(uint64& lastCheckMs, float ppm)
{
    if (ppm <= 0.0f)
        return false;

    uint64 now = GameTime::GetGameTimeMS().count();
    uint64 elapsed = lastCheckMs ? std::min<uint64>(now - lastCheckMs, 2000) : 1000;
    lastCheckMs = now;
    return roll_chance_f(ppm * float(elapsed) / 600.0f);
}

uint32 ScaledAmount(uint32 amount, float percent)
{
    if (!amount || percent <= 0.0f)
        return 0;
    double scaled = std::ceil(double(amount) * percent / 100.0);
    return uint32(std::clamp<double>(scaled, 0.0, std::numeric_limits<uint32>::max()));
}

uint64 ScaledAmount64(uint32 amount, float percent)
{
    if (!amount || percent <= 0.0f)
        return 0;
    long double scaled = std::floor(static_cast<long double>(amount) * percent / 100.0L);
    return uint64(std::clamp<long double>(scaled, 0.0L, std::numeric_limits<uint64>::max()));
}

void SaturatingAdd(uint64& total, uint64 amount)
{
    total = std::numeric_limits<uint64>::max() - total < amount
        ? std::numeric_limits<uint64>::max()
        : total + amount;
}

void CastTertiarySpell(Player* player, TertiaryState& state, Unit* target, uint32 spellId,
    int32 bp0, int32 bp1 = 0, int32 bp2 = 0, uint32 durationMs = 0)
{
    ProcessingGuard guard(state);
    CustomSpellValues values;
    values.AddSpellMod(SPELLVALUE_BASE_POINT0, bp0);
    values.AddSpellMod(SPELLVALUE_BASE_POINT1, bp1);
    values.AddSpellMod(SPELLVALUE_BASE_POINT2, bp2);
    if (durationMs)
        values.AddSpellMod(SPELLVALUE_AURA_DURATION, int32(durationMs));
    player->CastCustomSpell(spellId, values, target, TERTIARY_TRIGGER_FLAGS);
}

void CastTertiaryAmount(Player* player, TertiaryState& state, Unit* target, uint32 spellId, uint64 amount)
{
    while (amount)
    {
        int32 chunk = int32(std::min<uint64>(amount, std::numeric_limits<int32>::max()));
        CastTertiarySpell(player, state, target, spellId, chunk);
        amount -= uint64(chunk);
    }
}

void TryTempo(Player* player, TertiaryState& state)
{
    float ppm = TempoPpm(state);
    if (!RollNormalizedProc(state.lastTempoCheckMs, ppm))
        return;

    int32 speed = int32(std::lround(TempoSpeed(state)));
    if (speed <= 0)
        return;

    uint32 durationMs = SettingsFor(state).tempoDurationMs;
    CastTertiarySpell(player, state, player, SPELL_TEMPO, speed, speed, speed, durationMs);
    CastTertiarySpell(player, state, player, SPELL_TEMPO_COOLDOWN, 0, 0, 0, durationMs);
}

void TryOpportunity(Player* player, TertiaryState& state)
{
    if (player->HasAura(SPELL_OPPORTUNITY) || state.opportunityCast || state.opportunityChannel)
        return;

    float ppm = OpportunityPpm(state);
    if (!RollNormalizedProc(state.lastOpportunityCheckMs, ppm))
        return;

    CastTertiarySpell(player, state, player, SPELL_OPPORTUNITY, 0, 0, 0, _settings.opportunityDurationMs);
}

void AddEchoOutput(TertiaryState& state, Unit* target, uint32 amount, bool healing)
{
    uint64 stored = ScaledAmount64(amount, state.echoPercent);
    if (!stored)
        return;

    if (healing)
        SaturatingAdd(state.echoHealing, stored);
    else
    {
        SaturatingAdd(state.echoDamage, stored);
        state.echoDamageAnchor = target->GetGUID();
    }
}

void TryEcho(Player* player, TertiaryState& state, Unit* target, uint32 amount, bool healing,
    bool canProc)
{
    if (state.echoActive)
    {
        AddEchoOutput(state, target, amount, healing);
        return;
    }

    float percent = EchoPercent(state);
    if (!canProc || percent <= 0.0f
        || !RollNormalizedProc(state.lastEchoCheckMs, EchoPpm(state)))
        return;

    state.echoActive = true;
    state.echoEndsAtMs = uint64(GameTime::GetGameTimeMS().count()) + SettingsFor(state).echoDurationMs;
    state.echoPercent = percent;
    state.echoDamage = 0;
    state.echoHealing = 0;
    state.echoDamageAnchor.Clear();
    CastTertiarySpell(player, state, player, SPELL_ECHO_AURA, int32(std::lround(percent)), 0, 0,
        SettingsFor(state).echoDurationMs);
    AddEchoOutput(state, target, amount, healing);
}

void ApplySiphon(Player* player, TertiaryState& state, uint32 amount)
{
    uint32 healing = ScaledAmount(amount, Siphon(state));
    if (healing && player->IsAlive() && player->GetHealth() < player->GetMaxHealth())
        CastTertiarySpell(player, state, player, SPELL_SIPHON_HEAL, int32(healing));
}

void HandleAction(Player* player, Unit* target, uint32 amount, bool healing, bool canProc)
{
    if (!_settings.enabled || !_dbcReady || !player || !target || !amount)
        return;

    TertiaryState* state = FindState(player);
    if (!state)
        return;
    state = EnsureState(player);

    if (canProc)
        ApplySiphon(player, *state, amount);
    TryEcho(player, *state, target, amount, healing, canProc);
    if (canProc)
    {
        TryTempo(player, *state);
        TryOpportunity(player, *state);
    }
}

bool HasDirectDamageEffect(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        switch (effect.Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_HEALTH_LEECH:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
            case SPELL_EFFECT_POWER_BURN:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool HasDirectCritEffect(SpellInfo const* spellInfo)
{
    if (!spellInfo || !spellInfo->IsCritCapable())
        return false;
    if (HasDirectDamageEffect(spellInfo))
        return true;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        switch (effect.Effect)
        {
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_PCT:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
            case SPELL_EFFECT_HEAL_MECHANICAL:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool HasPeriodicCritEffect(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
            case SPELL_AURA_PERIODIC_LEECH:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool HasDeferredAreaCritOutput(SpellInfo const* spellInfo)
{
    if (!spellInfo || spellInfo->IsPositive())
        return false;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        if (effect.Effect != SPELL_EFFECT_PERSISTENT_AREA_AURA)
            continue;

        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_PERIODIC_DUMMY:
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool HasScriptedCritOutput(SpellInfo const* spellInfo)
{
    if (!spellInfo || spellInfo->DmgClass == SPELL_DAMAGE_CLASS_NONE)
        return false;

    return std::any_of(spellInfo->Effects.begin(), spellInfo->Effects.end(),
        [](SpellEffectInfo const& effect)
        {
            return effect.Effect == SPELL_EFFECT_DUMMY
                || effect.Effect == SPELL_EFFECT_SCRIPT_EFFECT;
        });
}

bool HasPeriodicTriggeredDamageEffect(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
    {
        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
            case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
                break;
            default:
                continue;
        }
        SpellInfo const* output = sSpellMgr->GetSpellInfo(effect.TriggerSpell);
        if (output && output->SpellFamilyName == spellInfo->SpellFamilyName
            && (HasDirectDamageEffect(output) || HasPeriodicCritEffect(output)))
            return true;
    }
    return false;
}

bool HasAreaEffect(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;
    if (spellInfo->IsAffectingArea())
        return true;

    return std::any_of(spellInfo->Effects.begin(), spellInfo->Effects.end(),
        [](SpellEffectInfo const& effect)
        {
            return effect.Effect == SPELL_EFFECT_PERSISTENT_AREA_AURA;
        });
}

bool IsKnownDamagingPeriodicArea(SpellInfo const* spellInfo)
{
    return spellInfo && !spellInfo->IsPositive() && HasAreaEffect(spellInfo)
        && (HasPeriodicCritEffect(spellInfo) || HasPeriodicTriggeredDamageEffect(spellInfo));
}

bool IsPeriodicAreaCandidate(SpellInfo const* spellInfo)
{
    return IsKnownDamagingPeriodicArea(spellInfo)
        || (spellInfo && !spellInfo->IsPositive() && HasAreaEffect(spellInfo)
            && HasDeferredAreaCritOutput(spellInfo));
}

bool IsTempoPeriodicEffect(SpellInfo const* spellInfo, AuraEffect const* effect)
{
    if (!effect || !effect->IsPeriodic())
        return false;

    switch (effect->GetAuraType())
    {
        case SPELL_AURA_PERIODIC_DAMAGE:
        case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
        case SPELL_AURA_PERIODIC_LEECH:
            return true;
        case SPELL_AURA_PERIODIC_DUMMY:
            return HasDeferredAreaCritOutput(spellInfo);
        case SPELL_AURA_PERIODIC_TRIGGER_SPELL:
        case SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
            return HasDeferredAreaCritOutput(spellInfo)
                || HasPeriodicTriggeredDamageEffect(spellInfo);
        default:
            return false;
    }
}

bool MatchesPeriodicArea(PeriodicAreaState const& area, ObjectGuid ownerGuid,
    ObjectGuid casterGuid, uint32 spellId)
{
    return area.ownerGuid == ownerGuid && area.casterGuid == casterGuid
        && area.spellId == spellId;
}

PeriodicAreaState const* FindPeriodicArea(TertiaryState const& state, ObjectGuid ownerGuid,
    ObjectGuid casterGuid, uint32 spellId)
{
    auto area = std::find_if(state.periodicAreas.begin(), state.periodicAreas.end(),
        [ownerGuid, casterGuid, spellId](PeriodicAreaState const& candidate)
        {
            return MatchesPeriodicArea(candidate, ownerGuid, casterGuid, spellId);
        });
    return area != state.periodicAreas.end() ? &*area : nullptr;
}

void RemovePeriodicArea(TertiaryState& state, Aura const* aura)
{
    if (!aura || !aura->GetOwner())
        return;

    ObjectGuid const ownerGuid = aura->GetOwner()->GetGUID();
    ObjectGuid const casterGuid = aura->GetCasterGUID();
    uint32 const spellId = aura->GetSpellInfo()->Id;
    state.periodicAreas.erase(std::remove_if(state.periodicAreas.begin(),
        state.periodicAreas.end(), [ownerGuid, casterGuid, spellId](PeriodicAreaState const& area)
        {
            return MatchesPeriodicArea(area, ownerGuid, casterGuid, spellId);
        }), state.periodicAreas.end());
}

void RegisterPeriodicArea(Spell* spell, TertiaryState& state, bool opportunity)
{
    SpellInfo const* spellInfo = spell ? spell->GetSpellInfo() : nullptr;
    Aura* aura = spell ? spell->GetCreatedAura() : nullptr;
    if (!aura || !aura->GetOwner() || !IsPeriodicAreaCandidate(spellInfo))
        return;

    bool hasTempoPeriodicEffect = false;
    for (uint8 index = 0; index < MAX_SPELL_EFFECTS; ++index)
        if (IsTempoPeriodicEffect(spellInfo, aura->GetEffect(index)))
        {
            hasTempoPeriodicEffect = true;
            break;
        }
    if (!hasTempoPeriodicEffect)
        return;

    ObjectGuid const ownerGuid = aura->GetOwner()->GetGUID();
    ObjectGuid const casterGuid = aura->GetCasterGUID();
    uint32 const spellId = spellInfo->Id;
    auto existing = std::find_if(state.periodicAreas.begin(), state.periodicAreas.end(),
        [ownerGuid, casterGuid, spellId](PeriodicAreaState const& area)
        {
            return MatchesPeriodicArea(area, ownerGuid, casterGuid, spellId);
        });
    if (existing != state.periodicAreas.end())
    {
        existing->opportunity = existing->opportunity || opportunity;
        existing->damaging = existing->damaging || IsKnownDamagingPeriodicArea(spellInfo);
        return;
    }

    state.periodicAreas.push_back({ ownerGuid, casterGuid, spellId,
        IsKnownDamagingPeriodicArea(spellInfo), opportunity });
}

bool IsOpportunityAreaOutput(TertiaryState const& state, Spell const* spell)
{
    SpellInfo const* sourceInfo = spell ? spell->GetTriggeredByAuraSpellInfo() : nullptr;
    if (!sourceInfo)
        return false;

    PeriodicAreaState const* area = FindPeriodicArea(state,
        spell->GetTriggeredByAuraOwnerGUID(), spell->GetTriggeredByAuraCasterGUID(),
        sourceInfo->Id);
    return area && area->opportunity;
}

void MarkDamagingAreaOutput(TertiaryState& state, Spell const* spell)
{
    SpellInfo const* outputInfo = spell ? spell->GetSpellInfo() : nullptr;
    SpellInfo const* sourceInfo = spell ? spell->GetTriggeredByAuraSpellInfo() : nullptr;
    if (!sourceInfo || (!HasDirectDamageEffect(outputInfo) && !HasPeriodicCritEffect(outputInfo)))
        return;

    ObjectGuid const ownerGuid = spell->GetTriggeredByAuraOwnerGUID();
    ObjectGuid const casterGuid = spell->GetTriggeredByAuraCasterGUID();
    for (PeriodicAreaState& area : state.periodicAreas)
        if (MatchesPeriodicArea(area, ownerGuid, casterGuid, sourceInfo->Id))
            area.damaging = true;
}

bool IsSameFamilyOutput(SpellInfo const* source, SpellInfo const* output)
{
    return source && output && source->SpellFamilyName != SPELLFAMILY_GENERIC
        && source->SpellFamilyName == output->SpellFamilyName;
}

bool HasScriptedChannelOutput(SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->IsChanneled() && HasScriptedCritOutput(spellInfo);
}

bool HasCritCapableChannelOutput(SpellInfo const* spellInfo)
{
    if (!spellInfo || !spellInfo->IsChanneled())
        return false;
    if (spellInfo->IsCritCapable() || HasScriptedChannelOutput(spellInfo))
        return true;

    for (SpellEffectInfo const& effect : spellInfo->Effects)
        if (SpellInfo const* triggered = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
            if (triggered->IsCritCapable())
                return true;
    return false;
}


bool IsOpportunityAbility(Spell* spell)
{
    SpellInfo const* spellInfo = spell ? spell->GetSpellInfo() : nullptr;
    return spellInfo && !spell->IsTriggered() && !spell->m_CastItem && !spellInfo->IsPassive()
        && spellInfo->SpellFamilyName != SPELLFAMILY_GENERIC
        && !spellInfo->HasAttribute(SPELL_ATTR0_IS_TRADESKILL)
        && !spellInfo->IsAutoRepeatRangedSpell()
        && (HasDirectCritEffect(spellInfo) || HasPeriodicCritEffect(spellInfo)
            || HasDeferredAreaCritOutput(spellInfo) || HasCritCapableChannelOutput(spellInfo)
            || (!spellInfo->IsPositive() && HasScriptedCritOutput(spellInfo)));
}

bool IsTempoCooldownEligible(Spell* spell)
{
    SpellInfo const* spellInfo = spell ? spell->GetSpellInfo() : nullptr;
    if (!spellInfo || spell->IsTriggered() || spell->m_CastItem || spellInfo->IsPassive()
        || spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC
        || spellInfo->HasAttribute(SPELL_ATTR0_IS_TRADESKILL))
        return false;

    uint32 recovery = spellInfo->GetRecoveryTime();
    return recovery >= _settings.tempoMinCooldownMs && recovery <= _settings.tempoMaxCooldownMs;
}

void ReduceCooldown(Player* player, uint32 spellId, uint32 reduction)
{
    uint32 remaining = player->GetSpellCooldownDelay(spellId);
    uint32 applied = std::min(reduction, remaining);
    if (applied)
        player->ModifySpellCooldown(spellId, -int32(applied));
}

void ApplyTempoCooldown(Player* player, TertiaryState& state, SpellInfo const* spellInfo)
{
    uint32 reduction = uint32(std::lround(TempoCooldownMs(state)));
    if (!reduction)
        return;

    ReduceCooldown(player, spellInfo->Id, reduction);

    uint32 category = spellInfo->GetCategory();
    if (!category || !spellInfo->CategoryRecoveryTime)
        return;

    SpellCategoryStore::const_iterator categorySpells = sSpellsByCategoryStore.find(category);
    if (categorySpells == sSpellsByCategoryStore.end())
        return;

    for (std::pair<bool, uint32> const& categorySpell : categorySpells->second)
    {
        if (categorySpell.first || categorySpell.second == spellInfo->Id)
            continue;
        SpellInfo const* sibling = sSpellMgr->GetSpellInfo(categorySpell.second);
        if (sibling && sibling->SpellFamilyName == spellInfo->SpellFamilyName)
            ReduceCooldown(player, sibling->Id, reduction);
    }
}

void TriggerUnbroken(Player* player, TertiaryState& state)
{
    state.pendingUnbroken = false;
    if (!player->IsAlive() || player->HasSpellCooldown(SPELL_UNBROKEN))
        return;

    float points = Points(state, Power::Unbroken);
    if (points <= 0.0f)
        return;

    float shieldPct = _settings.unbrokenShieldBase
        + points * _settings.unbrokenShieldPerPoint;
    float healingPct = points * _settings.unbrokenHealingPerPoint;
    int32 shield = int32(ScaledAmount(player->GetMaxHealth(), shieldPct));
    uint32 ticks = std::max<uint32>(1, _settings.unbrokenDurationMs / 1000);
    int32 healPerTick = int32(ScaledAmount(player->GetMaxHealth(), healingPct) / ticks);
    healPerTick = std::max(healPerTick, 1);
    int32 speed = int32(std::lround(points * _settings.unbrokenSpeedPerPoint));

    player->RemoveAurasWithMechanic(1ULL << MECHANIC_DAZE);
    CastTertiarySpell(player, state, player, SPELL_UNBROKEN, shield, healPerTick, speed,
        _settings.unbrokenDurationMs);
    player->AddSpellCooldown(SPELL_UNBROKEN, 0, _settings.unbrokenCooldownMs, false);
}

void ResetEcho(Player* player, TertiaryState& state)
{
    state.echoActive = false;
    state.echoEndsAtMs = 0;
    state.echoPercent = 0.0f;
    state.echoDamage = 0;
    state.echoHealing = 0;
    state.echoDamageAnchor.Clear();
    player->RemoveAurasDueToSpell(SPELL_ECHO_AURA);
}

bool IsCurrentSpell(Player* player, Spell const* spell)
{
    return spell && (player->GetCurrentSpell(CURRENT_MELEE_SPELL) == spell
        || player->GetCurrentSpell(CURRENT_GENERIC_SPELL) == spell
        || player->GetCurrentSpell(CURRENT_CHANNELED_SPELL) == spell
        || player->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) == spell);
}

void RearmFailedCast(Player* player, TertiaryState& state, Spell const* spell)
{
    if (state.tempoPendingCast == spell)
    {
        uint64 expiresAt = state.tempoPendingChargeExpiresAtMs;
        state.tempoPendingCast = nullptr;
        state.tempoPendingChargeExpiresAtMs = 0;
        uint64 now = GameTime::GetGameTimeMS().count();
        if (!player->HasAura(SPELL_TEMPO_COOLDOWN) && expiresAt > now)
            CastTertiarySpell(player, state, player, SPELL_TEMPO_COOLDOWN, 0, 0, 0,
                uint32(expiresAt - now));
    }

    bool restoreOpportunity = false;
    if (state.opportunityChannel == spell)
    {
        restoreOpportunity = state.opportunityCast == spell;
        state.opportunityCast = nullptr;
        state.opportunityChannel = nullptr;
        state.opportunityCastInfo = nullptr;
        state.opportunityChannelInfo = nullptr;
    }
    else if (state.opportunityCast == spell)
    {
        state.opportunityCast = nullptr;
        state.opportunityCastInfo = nullptr;
        restoreOpportunity = true;
    }

    if (restoreOpportunity && player->IsAlive() && _settings.enabled && _dbcReady
        && Points(state, Power::Opportunity) > 0.0f && !player->HasAura(SPELL_OPPORTUNITY))
    {
        CastTertiarySpell(player, state, player, SPELL_OPPORTUNITY, 0, 0, 0,
            _settings.opportunityDurationMs);
    }
}

void ResetProcState(Player* player, TertiaryState& state)
{
    state.pendingUnbroken = false;
    state.lastTempoCheckMs = 0;
    state.lastEchoCheckMs = 0;
    state.lastOpportunityCheckMs = 0;
    state.tempoPendingCast = nullptr;
    state.tempoPendingChargeExpiresAtMs = 0;
    state.opportunityCast = nullptr;
    state.opportunityChannel = nullptr;
    state.opportunityCastInfo = nullptr;
    state.opportunityChannelInfo = nullptr;
    state.periodicAreas.clear();
    ResetEcho(player, state);
    player->RemoveAurasDueToSpell(SPELL_TEMPO);
    player->RemoveAurasDueToSpell(SPELL_TEMPO_COOLDOWN);
    player->RemoveAurasDueToSpell(SPELL_UNBROKEN);
    player->RemoveAurasDueToSpell(SPELL_OPPORTUNITY);
}

Unit* EchoDamageAnchor(Player* player, ObjectGuid fallback)
{
    Unit* selected = player->GetSelectedUnit();
    if (selected && selected->IsAlive() && !player->IsFriendlyTo(selected)
        && selected->IsInCombatWith(player))
        return selected;

    Unit* recorded = ObjectAccessor::GetUnit(*player, fallback);
    if (recorded && recorded->IsAlive() && !player->IsFriendlyTo(recorded)
        && recorded->IsInCombatWith(player))
        return recorded;

    std::list<Unit*> nearby;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, _settings.echoDamageRadius);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearby, check);
    Cell::VisitObjects(player, searcher, _settings.echoDamageRadius);
    Unit* nearest = nullptr;
    for (Unit* target : nearby)
        if (target->IsAlive() && target->IsEngagedBy(player)
            && (!nearest || player->GetDistance(target) < player->GetDistance(nearest)))
            nearest = target;
    return nearest;
}

void ReleaseEchoDamage(Player* player, TertiaryState& state, uint64 pool, ObjectGuid anchorGuid)
{
    Unit* anchor = EchoDamageAnchor(player, anchorGuid);
    if (!anchor || !pool)
        return;

    std::list<Unit*> nearby;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck check(anchor, player, _settings.echoDamageRadius);
    Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(anchor, nearby, check);
    Cell::VisitObjects(anchor, searcher, _settings.echoDamageRadius);

    std::vector<Unit*> targets;
    for (Unit* target : nearby)
        if (target->IsEngagedBy(player))
            targets.push_back(target);
    if (std::find(targets.begin(), targets.end(), anchor) == targets.end())
        targets.push_back(anchor);

    std::sort(targets.begin(), targets.end(), [anchor](Unit const* left, Unit const* right)
    {
        return anchor->GetDistance(left) < anchor->GetDistance(right);
    });

    uint64 quotient = pool / targets.size();
    uint64 remainder = pool % targets.size();
    for (Unit* target : targets)
    {
        uint64 amount = quotient + (remainder ? 1 : 0);
        remainder -= remainder != 0;
        if (amount)
            CastTertiaryAmount(player, state, target, SPELL_ECHO_DAMAGE, amount);
    }
}

void ReleaseEchoHealing(Player* player, TertiaryState& state, uint64 pool)
{
    if (!pool)
        return;

    std::list<Player*> nearby;
    Acore::AnyPlayerInObjectRangeCheck check(player, _settings.echoHealingRadius);
    Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(player, nearby, check);
    Cell::VisitObjects(player, searcher, _settings.echoHealingRadius);
    if (std::find(nearby.begin(), nearby.end(), player) == nearby.end())
        nearby.push_back(player);

    std::vector<Player*> targets;
    for (Player* target : nearby)
        if (target->IsAlive() && target->GetHealth() < target->GetMaxHealth() && target->IsInSameRaidWith(player))
            targets.push_back(target);

    std::sort(targets.begin(), targets.end(), [](Player const* left, Player const* right)
    {
        return left->GetHealthPct() < right->GetHealthPct();
    });

    for (Player* target : targets)
    {
        uint64 missing = target->GetMaxHealth() - target->GetHealth();
        uint64 amount = std::min(pool, missing);
        if (amount)
            CastTertiaryAmount(player, state, target, SPELL_ECHO_HEAL, amount);
        pool -= amount;
        if (!pool)
            break;
    }
}

void ProcessEcho(Player* player, TertiaryState& state)
{
    if (!state.echoActive)
        return;

    uint64 now = GameTime::GetGameTimeMS().count();
    if (!player->IsAlive() || (!player->HasAura(SPELL_ECHO_AURA) && now < state.echoEndsAtMs))
    {
        ResetEcho(player, state);
        return;
    }
    if (now < state.echoEndsAtMs)
        return;

    uint64 damage = state.echoDamage;
    uint64 healing = state.echoHealing;
    ObjectGuid anchor = state.echoDamageAnchor;
    ResetEcho(player, state);
    ReleaseEchoDamage(player, state, damage, anchor);
    ReleaseEchoHealing(player, state, healing);
}


class TertiaryStatsWorld : public WorldScript
{
public:
    TertiaryStatsWorld() : WorldScript("TertiaryStatsWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        _settings.enabled = sConfigMgr->GetOption<bool>("TertiaryStats.Enable", true);
        _settings.rollChance = ClampChance(
            sConfigMgr->GetOption<float>("TertiaryStats.Roll.PowerChance", 70.0f));
        _settings.ordinaryChance = ClampChance(
            sConfigMgr->GetOption<float>("TertiaryStats.Roll.OrdinaryChance", 70.0f));
        _settings.minQuality = sConfigMgr->GetOption<uint32>("TertiaryStats.Eligibility.MinQuality", ITEM_QUALITY_UNCOMMON);
        _settings.maxQuality = sConfigMgr->GetOption<uint32>("TertiaryStats.Eligibility.MaxQuality", ITEM_QUALITY_EPIC);
        _settings.minItemLevel = sConfigMgr->GetOption<uint32>("TertiaryStats.Eligibility.MinItemLevel", 1);

        for (uint8 index = 0; index < POWER_COUNT; ++index)
        {
            std::string key = "TertiaryStats.Roll.Weight." + std::string(POWER_NAMES[index]);
            _settings.weights[index] = std::max(0.0f, sConfigMgr->GetOption<float>(key, 1.0f));
        }

        _settings.avoidancePerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.AvoidancePerPoint", 0.5f));
        _settings.fleetfootBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.FleetfootBase", 15.0f));
        _settings.fleetfootPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.FleetfootPerPoint", 1.1f));
        _settings.siphonBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.SiphonBase", 1.0f));
        _settings.siphonPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.SiphonPerPoint", 0.2f));

        _settings.tempoSpeedBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.TempoSpeedBase", 5.0f));
        _settings.tempoSpeedPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.TempoSpeedPerPoint", 1.2f));
        _settings.tempoPpmBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.TempoPPMBase", 3.0f));
        _settings.tempoPpmPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.TempoPPMPerPoint", 0.075f));
        _settings.tempoCooldownBaseMs = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.TempoCooldownBaseMs", 1000.0f));
        _settings.tempoCooldownPerPointMs = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.TempoCooldownPerPointMs", 100.0f));
        _settings.tempoDurationMs = ClampAuraDuration(
            sConfigMgr->GetOption<uint32>("TertiaryStats.Tempo.DurationMs", 8000));
        _settings.tempoMinCooldownMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Tempo.MinCooldownMs", 3000);
        _settings.tempoMaxCooldownMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Tempo.MaxCooldownMs", 30000);
        if (_settings.tempoMinCooldownMs > _settings.tempoMaxCooldownMs)
            std::swap(_settings.tempoMinCooldownMs, _settings.tempoMaxCooldownMs);

        _settings.echoPercentBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.EchoPercentBase", 4.0f));
        _settings.echoPercentPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.EchoPercentPerPoint", 0.46f));
        _settings.echoPpmBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.EchoPPMBase", 2.0f));
        _settings.echoPpmPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.EchoPPMPerPoint", 0.05f));
        _settings.echoDurationMs = ClampAuraDuration(
            sConfigMgr->GetOption<uint32>("TertiaryStats.Echo.DurationMs", 6000));
        _settings.echoDamageRadius = std::max(1.0f, sConfigMgr->GetOption<float>("TertiaryStats.Echo.DamageRadius", 10.0f));
        _settings.echoHealingRadius = std::max(1.0f, sConfigMgr->GetOption<float>("TertiaryStats.Echo.HealingRadius", 30.0f));

        _settings.unbrokenShieldBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.UnbrokenShieldBase", 10.0f));
        _settings.unbrokenShieldPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.UnbrokenShieldPerPoint", 0.25f));
        _settings.unbrokenHealingPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.UnbrokenHealingPerPoint", 0.5f));
        _settings.unbrokenSpeedPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.UnbrokenSpeedPerPoint", 2.0f));
        _settings.unbrokenThreshold = std::clamp(sConfigMgr->GetOption<float>("TertiaryStats.Unbroken.ThresholdPct", 35.0f), 1.0f, 99.0f);
        _settings.unbrokenDurationMs = ClampAuraDuration(
            sConfigMgr->GetOption<uint32>("TertiaryStats.Unbroken.DurationMs", 6000));
        _settings.unbrokenCooldownMs = sConfigMgr->GetOption<uint32>("TertiaryStats.Unbroken.CooldownMs", 45000);

        _settings.opportunityPpmBase = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.OpportunityPPMBase", 0.5f));
        _settings.opportunityPpmPerPoint = std::max(0.0f, sConfigMgr->GetOption<float>("TertiaryStats.Value.OpportunityPPMPerPoint", 0.225f));
        _settings.opportunityDurationMs = ClampAuraDuration(
            sConfigMgr->GetOption<uint32>("TertiaryStats.Opportunity.DurationMs", 8000));

        Fabled::LoadSettings();
        ++_settingsRevision;
    }

    void OnStartup() override
    {
        ValidateTertiarySpells();
        if (Fabled::ValidateSpells())
            LOG_INFO("module", "TertiaryStats: fabled effects enabled");
    }
};


class TertiaryStatsItem : public AllItemScript
{
public:
    TertiaryStatsItem() : AllItemScript("TertiaryStatsItem",
        { ITEMHOOK_ON_ITEM_BONUS_SEED_GENERATE }) { }

    void OnItemBonusSeedGenerate(ItemTemplate const* itemTemplate, uint32 entropy,
        uint32& bonusSeed) override
    {
        if (!ValidateTertiarySpells() || !Fabled::ValidateSpells())
            return;

        BonusRng rng(entropy, itemTemplate->ItemId);
        Settings const& settings = _itemRollTestSettings ? *_itemRollTestSettings : _settings;
        Fabled::Settings const& fabledSettings = _fabledRollTestSettings
            ? *_fabledRollTestSettings : Fabled::GetSettings();
        bonusSeed = MakeItemBonusSeed(RollBonus(itemTemplate, rng, settings, fabledSettings));
        if (bonusSeed != MakeItemBonusSeed(0))
            bonusSeed = AddHeirloomProfile(bonusSeed, HeirloomProfileFor(itemTemplate));
    }
};

class TertiaryStatsPlayer : public PlayerScript
{
public:
    TertiaryStatsPlayer() : PlayerScript("TertiaryStatsPlayer", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_SPELL_CAST,
        PLAYERHOOK_ON_PLAYER_ENTER_COMBAT,
        PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT,
        PLAYERHOOK_ON_EQUIP,
        PLAYERHOOK_ON_UNEQUIP_ITEM,
        PLAYERHOOK_ON_ENVIRONMENTAL_DAMAGE,
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
        PLAYERHOOK_ON_PLAYER_RESURRECT,
        PLAYERHOOK_CAN_ITEM_LOSE_DURABILITY,
        PLAYERHOOK_CAN_ATTACK_WHILE_MOUNTED,
        PLAYERHOOK_CAN_USE_GAMEOBJECT_WHILE_MOUNTED,
        PLAYERHOOK_ON_BEFORE_SEND_LOOT,
        PLAYERHOOK_ON_AFTER_SEND_AUCTION_LIST,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
    }) { }

    void OnPlayerBeforeSendLoot(Player* player, ObjectGuid /*lootGuid*/, Loot* loot) override
    {
        SendTertiaryLootSnapshot(player, loot);
    }

    void OnPlayerAfterSendAuctionList(Player* player, uint8 listType,
        AuctionListResultView items) override
    {
        SendTertiaryAuctionSnapshot(player, listType, items);
    }


    void OnPlayerLogin(Player* player) override
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (TertiaryBonusOf(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot)))
            {
                GetState(player)->needsRefresh = true;
                return;
            }
        }
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (TertiaryState* state = FindState(player))
            state->needsRefresh = true;
        SendTertiaryInventorySnapshot(player);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& message,
        Player* receiver) override
    {
        constexpr std::string_view PREFIX = "TStats\t";
        if (player != receiver || type != CHAT_MSG_WHISPER || language != LANG_ADDON
            || message.compare(0, PREFIX.size(), PREFIX) != 0)
            return true;

        std::string_view payload(message.data() + PREFIX.size(), message.size() - PREFIX.size());
        if (payload == "V1:S")
            SendTertiarySnapshot(player);
        else if (payload == "V1:I")
            SendTertiaryInventorySnapshot(player);

        return false;
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        TertiaryState* state = FindState(player);
        if (!state)
            return;
        if (!SettingsFor(*state).enabled)
        {
            if (!state->disabled)
            {
                if (state->needsRefresh || state->settingsRevision != _settingsRevision)
                    RefreshEquipment(player, *state);
                ResetProcState(player, *state);
                state->disabled = true;
            }
            return;
        }

        state->disabled = false;

        if (!_dbcReady)
            return;

        state = EnsureState(player);
        Fabled::HandleUpdate(player, diff);
        if (state->pendingUnbroken)
            TriggerUnbroken(player, *state);
        ProcessEcho(player, *state);

        if (state->tempoPendingCast && !IsCurrentSpell(player, state->tempoPendingCast))
            RearmFailedCast(player, *state, state->tempoPendingCast);

        if (state->opportunityCast && !IsCurrentSpell(player, state->opportunityCast)
            && (!state->opportunityChannel || !IsCurrentSpell(player, state->opportunityChannel)))
        {
            RearmFailedCast(player, *state, state->opportunityCast);
        }

        if (state->opportunityChannel && !IsCurrentSpell(player, state->opportunityChannel))
        {
            state->opportunityChannel = nullptr;
            state->opportunityChannelInfo = nullptr;
        }
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!_settings.enabled || !_dbcReady || !spell)
            return;

        TertiaryState* state = FindState(player);
        if (!state)
            return;
        state = EnsureState(player);
        if (!spell->IsTriggered())
        {
            if (state->tempoPendingCast && state->tempoPendingCast != spell
                && !IsCurrentSpell(player, state->tempoPendingCast))
            {
                RearmFailedCast(player, *state, state->tempoPendingCast);
            }
            if (state->opportunityCast && state->opportunityCast != spell
                && !IsCurrentSpell(player, state->opportunityCast)
                && (!state->opportunityChannel || !IsCurrentSpell(player, state->opportunityChannel)))
            {
                RearmFailedCast(player, *state, state->opportunityCast);
            }
        }
        if (spell->IsTriggered())
            return;

        if (player->HasAura(SPELL_TEMPO_COOLDOWN) && IsTempoCooldownEligible(spell))
        {
            Aura* charge = player->GetAura(SPELL_TEMPO_COOLDOWN);
            state->tempoPendingCast = spell;
            state->tempoPendingChargeExpiresAtMs = GameTime::GetGameTimeMS().count()
                + uint64(std::max<int32>(charge ? charge->GetDuration() : 0, 0));
            player->RemoveAurasDueToSpell(SPELL_TEMPO_COOLDOWN);
        }

        if (player->HasAura(SPELL_OPPORTUNITY) && IsOpportunityAbility(spell))
        {
            state->opportunityCast = spell;
            state->opportunityCastInfo = spell->GetSpellInfo();
            state->opportunityChannel = spell->GetSpellInfo()->IsChanneled() ? spell : nullptr;
            state->opportunityChannelInfo = state->opportunityChannel
                ? state->opportunityCastInfo : nullptr;
            player->RemoveAurasDueToSpell(SPELL_OPPORTUNITY);
        }
    }

    void OnPlayerEquip(Player* player, Item* item, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/) override
    {
        if (TertiaryBonusOf(item) || FindState(player))
            GetState(player)->needsRefresh = true;
    }

    void OnPlayerUnequip(Player* player, Item* item) override
    {
        if (TertiaryBonusOf(item) || FindState(player))
            GetState(player)->needsRefresh = true;
    }


    void OnPlayerJustDied(Player* player) override
    {
        if (_settings.enabled && _dbcReady)
            Fabled::HandleDeath(player);
    }

    void OnPlayerResurrect(Player* player, float /*restorePercent*/,
        bool& /*applySickness*/) override
    {
        if (_settings.enabled && _dbcReady)
            Fabled::HandleResurrect(player);
    }

    bool CanAttackWhileMounted(Player* player, Unit* victim, bool meleeAttack) override
    {
        return _settings.enabled && _dbcReady
            && Fabled::HandleCanAttackWhileMounted(player, victim, meleeAttack);
    }

    bool CanUseGameObjectWhileMounted(Player* player, GameObject* gameObject) override
    {
        return _settings.enabled && _dbcReady
            && Fabled::HandleCanUseGameObjectWhileMounted(player, gameObject);
    }

    void OnPlayerEnterCombat(Player* player, Unit* /*enemy*/) override
    {
        if (_settings.enabled && _dbcReady)
            Fabled::HandleCombatEnter(player);
    }

    void OnPlayerLeaveCombat(Player* player) override
    {
        if (_settings.enabled && _dbcReady)
            Fabled::HandleCombatExit(player);
    }

    void OnPlayerEnvironmentalDamage(Player* player, EnviromentalDamage type, uint32& damage) override
    {
        if (!_settings.enabled || !_dbcReady || type == DAMAGE_FALL_TO_VOID || !damage)
            return;

        TertiaryState* state = FindState(player);
        if (!state)
            return;
        state = EnsureState(player);
        Fabled::HandleEnvironmentalDamage(player, type, damage);
        damage = uint32(std::floor(float(damage)
            * std::max(0.0f, 1.0f - Avoidance(*state) / 100.0f)));
    }

    bool CanItemLoseDurability(Player* /*player*/, Item* item) override
    {
        return !ItemHasPower(item, Power::Unbroken);
    }
};

class TertiaryStatsUnit : public UnitScript
{
public:
    TertiaryStatsUnit() : UnitScript("TertiaryStatsUnit", true,
        { UNITHOOK_ON_HEAL_FINAL, UNITHOOK_ON_DAMAGE_FINAL,
          UNITHOOK_MODIFY_SPELL_EFFECT_IMMUNITY_MASK, UNITHOOK_ON_AURA_APPLY,
          UNITHOOK_ON_AURA_REMOVE, UNITHOOK_ON_UNIT_DEATH }) { }

    void OnHealFinal(HealInfo const& healInfo) override
    {
        Unit* healer = healInfo.GetHealer();
        Unit* receiver = healInfo.GetTarget();
        uint32 gain = healInfo.GetEffectiveHeal();
        if (!healer || !receiver || !gain)
            return;

        Player* owner = healer->GetCharmerOrOwnerPlayerOrPlayerItself();
        HandleAction(owner, receiver, gain, true,
            !IsTertiaryOutput(healInfo.GetSpellInfo()));
    }

    void OnDamageFinal(Unit* attacker, Unit* victim, uint32 damage,
        DamageEffectType damageType, SpellInfo const* spellInfo,
        Spell const* damageSpell) override
    {
        if (!_settings.enabled || !_dbcReady || !attacker || !victim || !damage)
            return;

        Player* owner = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        TertiaryState* ownerState = owner ? FindState(owner) : nullptr;
        bool tertiaryOutput = IsTertiaryOutput(spellInfo);
        bool periodic = damageType == DOT
            || (damageSpell && damageSpell->GetTriggeredByAuraSpellInfo()
                && damageSpell->GetTriggeredByAuraTickNumber() > 0);
        Fabled::DamageKind kind = periodic ? Fabled::DamageKind::Periodic
            : (spellInfo ? Fabled::DamageKind::Spell : Fabled::DamageKind::Melee);

        if (!tertiaryOutput)
        {
            bool processingTertiaryOutput = ownerState && EnsureState(owner)->processing;
            if (owner && owner != victim && !owner->IsFriendlyTo(victim) && !processingTertiaryOutput)
                Fabled::HandleBeforeDealtDamage(owner, victim, damage, spellInfo, kind);

            Player* player = victim->ToPlayer();
            if (player && damage < player->GetHealth())
            {
                if (TertiaryState* state = FindState(player))
                {
                    state = EnsureState(player);
                    if (Points(*state, Power::Unbroken) > 0.0f && !player->HasSpellCooldown(SPELL_UNBROKEN))
                    {
                        float oldHealthPct = player->GetHealthPct();
                        float newHealthPct = 100.0f * float(player->GetHealth() - damage) / float(player->GetMaxHealth());
                        if (oldHealthPct >= _settings.unbrokenThreshold && newHealthPct < _settings.unbrokenThreshold)
                            state->pendingUnbroken = true;
                    }
                }
            }
        }

        if (!owner || owner == victim || owner->IsFriendlyTo(victim))
            return;

        uint32 effectiveDamage = std::min(damage, victim->GetHealth());
        if (ownerState)
            HandleAction(owner, victim, effectiveDamage, false, !tertiaryOutput);
        if (!tertiaryOutput)
            Fabled::HandleDealtDamageFinal(owner, victim, damage, spellInfo, kind);
    }

    void OnUnitDeath(Unit* victim, Unit* killer) override
    {
        if (!_settings.enabled || !_dbcReady || !victim || !killer)
            return;

        Player* owner = killer->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (owner && owner != victim)
            Fabled::HandleKill(owner, victim);
    }

    void ModifySpellEffectImmunityMask(Unit* target, Unit* caster,
        SpellInfo const* spellInfo, uint8 candidateEffectMask,
        uint8& immuneEffectMask) override
    {
        Player* player = target ? target->ToPlayer() : nullptr;
        if (_settings.enabled && _dbcReady && player)
            Fabled::HandleModifySpellEffectImmunityMask(player, caster, spellInfo,
                candidateEffectMask, immuneEffectMask);
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {

        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (!_settings.enabled || !_dbcReady || !player || !aura)
            return;

        Fabled::HandleAuraApply(player, aura);
        SpellInfo const* spellInfo = aura->GetSpellInfo();
        TertiaryState* state = FindState(player);
        if (state && IsAllStatPercentAura(spellInfo))
            state->needsRefresh = true;

        if (!(spellInfo->GetAllEffectsMechanicMask() & (1ULL << MECHANIC_DAZE)))
            return;
        if (!state)
            return;
        state = EnsureState(player);
        if (!state->processing && Points(*state, Power::Unbroken) > 0.0f
            && !player->HasSpellCooldown(SPELL_UNBROKEN))
            state->pendingUnbroken = true;
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        Player* player = unit ? unit->ToPlayer() : nullptr;
        if (_settings.enabled && _dbcReady && player && aurApp)
            Fabled::HandleAuraRemove(player, aurApp->GetBase());
        TertiaryState* state = player ? FindState(player) : nullptr;
        if (_settings.enabled && _dbcReady && state && aurApp
            && IsAllStatPercentAura(aurApp->GetBase()->GetSpellInfo()))
            state->needsRefresh = true;
    }
};

class TertiaryStatsSpell : public AllSpellScript
{
public:
    TertiaryStatsSpell() : AllSpellScript("TertiaryStatsSpell",
        { ALLSPELLHOOK_ON_CALC_CRIT_CHANCE, ALLSPELLHOOK_ON_CALC_PERIODIC_CRIT_CHANCE,
          ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_CAST_CANCEL, ALLSPELLHOOK_ON_PREPARE,
          ALLSPELLHOOK_MODIFY_AURA_EFFECT_PERIODIC_TIME_RATE, ALLSPELLHOOK_ON_AURA_REMOVE,
          ALLSPELLHOOK_CAN_CAST_WHILE_MOVING, ALLSPELLHOOK_CAN_CAST_WHILE_MOUNTED }) { }

    void ModifyAuraEffectPeriodicTimeRate(AuraEffect const* effect, Unit* caster,
        double& timeRate) override
    {
        if (!_settings.enabled || !_dbcReady || !effect || !caster
            || !IsTempoPeriodicEffect(effect->GetSpellInfo(), effect))
            return;

        Player* player = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
        TertiaryState* state = player ? FindState(player) : nullptr;
        Aura const* aura = effect->GetBase();
        if (!state || !player->HasAura(SPELL_TEMPO) || !aura || !aura->GetOwner())
            return;

        PeriodicAreaState const* area = FindPeriodicArea(*state,
            aura->GetOwner()->GetGUID(), aura->GetCasterGUID(), aura->GetSpellInfo()->Id);
        if (area && area->damaging)
            timeRate *= 1.0 + double(TempoSpeed(*state)) / 100.0;
    }

    void OnAuraRemove(Aura const* aura, AuraRemoveMode /*removeMode*/) override
    {
        if (!aura)
            return;

        Unit* caster = aura->GetCaster();
        Player* player = caster ? caster->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
        if (TertiaryState* state = player ? FindState(player) : nullptr)
            RemovePeriodicArea(*state, aura);
    }

    void OnCalcCritChance(Spell* spell, Unit* target, float& critChance) override
    {
        if (!_settings.enabled || !_dbcReady || !spell || !target || !spell->GetSpellInfo()->IsCritCapable())
            return;

        Player* player = spell->GetCaster()->ToPlayer();
        if (!player)
            return;

        TertiaryState* state = FindState(player);
        if (!state)
            return;
        state = EnsureState(player);
        MarkDamagingAreaOutput(*state, spell);
        if (state->processing)
            return;

        bool empowered = state->opportunityCast == spell
            || IsOpportunityAreaOutput(*state, spell);
        if (!empowered && state->opportunityCast && spell->IsTriggered())
            empowered = IsSameFamilyOutput(state->opportunityCastInfo, spell->GetSpellInfo());
        if (!empowered && state->opportunityChannel
            && player->GetCurrentSpell(CURRENT_CHANNELED_SPELL) == state->opportunityChannel)
        {
            SpellInfo const* channelInfo = state->opportunityChannelInfo;
            SpellInfo const* triggeredBy = spell->GetTriggeredByAuraSpellInfo();
            empowered = IsSameFamilyOutput(channelInfo, spell->GetSpellInfo())
                && (triggeredBy == channelInfo || spell != state->opportunityChannel);
        }

        if (empowered)
            critChance = 100.0f;
    }

    void OnCalcPeriodicCritChance(SpellInfo const* spellInfo, Unit const* caster,
        Unit const* /*target*/, float& critChance) override
    {
        if (!_settings.enabled || !_dbcReady || !spellInfo || !caster
            || !HasPeriodicCritEffect(spellInfo))
            return;

        Player* player = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!player)
            return;

        TertiaryState* state = FindState(player);
        if (!state)
            return;
        state = EnsureState(player);
        if (!state->processing && state->opportunityCastInfo == spellInfo)
            critChance = 100.0f;
    }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (_settings.enabled && _dbcReady && player && spell)
            Fabled::HandleSpellPrepare(player, spell);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!_settings.enabled || !_dbcReady || !player || !spell || !spellInfo)
            return;

        Fabled::HandleSpellCastComplete(player, spell);
        TertiaryState* state = FindState(player);
        if (!state)
            return;
        state = EnsureState(player);
        MarkDamagingAreaOutput(*state, spell);
        if (state->tempoPendingCast == spell)
        {
            ApplyTempoCooldown(player, *state, spellInfo);
            state->tempoPendingCast = nullptr;
            state->tempoPendingChargeExpiresAtMs = 0;
        }

        bool opportunity = state->opportunityCast == spell;
        RegisterPeriodicArea(spell, *state, opportunity);
        if (opportunity)
        {
            state->opportunityCast = nullptr;
            state->opportunityCastInfo = nullptr;
        }
    }


    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/, bool /*bySelf*/) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!player || !spell)
            return;

        if (TertiaryState* state = FindState(player))
            RearmFailedCast(player, *state, spell);
    }

    bool CanCastWhileMoving(Spell const* spell) override
    {
        return _settings.enabled && _dbcReady && Fabled::HandleCanCastWhileMoving(spell);
    }

    bool CanCastWhileMounted(Spell const* spell) override
    {
        return _settings.enabled && _dbcReady && Fabled::HandleCanCastWhileMounted(spell);
    }
};

class spell_tertiary_unbroken_daze final : public SpellScript
{
    PrepareSpellScript(spell_tertiary_unbroken_daze);

    void PreventMountDispel(SpellEffIndex effIndex)
    {
        Player* player = GetHitPlayer();
        if (!_settings.enabled || !_dbcReady || !player)
            return;

        if (TertiaryState* state = FindState(player); state && EnsureState(player)->pendingUnbroken)
            PreventHitDefaultEffect(effIndex);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_tertiary_unbroken_daze::PreventMountDispel,
            EFFECT_1, SPELL_EFFECT_DISPEL_MECHANIC);
    }

};
constexpr uint32 GOSSIP_ACTION_REFORGE_BACK = GOSSIP_ACTION_INFO_DEF + 1;
constexpr uint32 GOSSIP_ACTION_REFORGE_SOURCE = GOSSIP_ACTION_INFO_DEF + 100;
constexpr uint32 GOSSIP_ACTION_REFORGE_DESTINATION = GOSSIP_ACTION_INFO_DEF + 200;

class npc_fabled_reforger final : public CreatureScript
{
public:
    npc_fabled_reforger() : CreatureScript("npc_fabled_reforger") { }

    void BuildSourceMenu(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();
        ReforgeSelection* selection =
            player->CustomData.GetDefault<ReforgeSelection>(REFORGE_STATE_KEY);
        selection->sourceGuid.Clear();
        selection->menuGuids.clear();

        for (Item* item : CarriedItems(player))
        {
            Fabled::Effect effect = Fabled::EffectFromBonusId(TertiaryBonusOf(item));
            if (effect == Fabled::Effect::None || item->GetCount() != 1 || item->IsInTrade())
                continue;

            selection->menuGuids.push_back(item->GetGUID());
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                Acore::StringFormat("{} — {}", item->GetTemplate()->Name1,
                    Fabled::Name(effect)),
                GOSSIP_SENDER_MAIN,
                GOSSIP_ACTION_REFORGE_SOURCE + uint32(selection->menuGuids.size() - 1));
        }

        if (selection->menuGuids.empty())
            ChatHandler(player->GetSession()).SendSysMessage(
                "No Fabled source items were found in your carried bags.");
        SendGossipMenuFor(player, creature->GetEntry(), creature->GetGUID());
    }

    void BuildDestinationMenu(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();
        ReforgeSelection* selection =
            player->CustomData.GetDefault<ReforgeSelection>(REFORGE_STATE_KEY);
        Item* source = FindCarriedItem(player, selection->sourceGuid);
        Fabled::Effect effect = source
            ? Fabled::EffectFromBonusId(TertiaryBonusOf(source)) : Fabled::Effect::None;
        selection->menuGuids.clear();
        if (effect == Fabled::Effect::None)
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "The selected Fabled source item is no longer available.");
            BuildSourceMenu(player, creature);
            return;
        }

        for (Item* item : CarriedItems(player))
        {
            if (item == source || item->GetCount() != 1 || item->IsInTrade()
                || !Fabled::CanReforgeTo(effect, item->GetTemplate())
                || Fabled::EffectFromBonusId(TertiaryBonusOf(item)) != Fabled::Effect::None)
                continue;

            uint32 cost = FabledReforgeCost(item->GetTemplate());
            selection->menuGuids.push_back(item->GetGUID());
            std::string confirmation = Acore::StringFormat(
                "Reforge {} onto {} for {}? The source item will be destroyed, "
                "the destination's existing tertiary powers will be replaced, and the destination "
                "will become soulbound and non-refundable.",
                Fabled::Name(effect), item->GetTemplate()->Name1, MoneyText(cost));
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
                Acore::StringFormat("{} — {}", item->GetTemplate()->Name1, MoneyText(cost)),
                GOSSIP_SENDER_MAIN,
                GOSSIP_ACTION_REFORGE_DESTINATION + uint32(selection->menuGuids.size() - 1),
                confirmation, 0, false);
        }

        if (selection->menuGuids.empty())
            ChatHandler(player->GetSession()).PSendSysMessage(
                "No eligible %s destination items were found in your carried bags.",
                Fabled::AnchorName(Fabled::AnchorFor(effect)));
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN,
            GOSSIP_ACTION_REFORGE_BACK);
        SendGossipMenuFor(player, creature->GetEntry(), creature->GetGUID());
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        std::string error;
        if (!ValidateReforgeContext(player, error))
        {
            ChatHandler(player->GetSession()).SendSysMessage(error);
            CloseGossipMenuFor(player);
            return true;
        }
        BuildSourceMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (!player || !creature)
            return false;

        std::string error;
        if (!ValidateReforgeContext(player, error))
        {
            ChatHandler(player->GetSession()).SendSysMessage(error);
            CloseGossipMenuFor(player);
            return true;
        }

        ReforgeSelection* selection =
            player->CustomData.GetDefault<ReforgeSelection>(REFORGE_STATE_KEY);
        if (action == GOSSIP_ACTION_REFORGE_BACK)
        {
            BuildSourceMenu(player, creature);
            return true;
        }
        if (action >= GOSSIP_ACTION_REFORGE_DESTINATION)
        {
            uint32 index = action - GOSSIP_ACTION_REFORGE_DESTINATION;
            if (index >= selection->menuGuids.size())
            {
                BuildSourceMenu(player, creature);
                return true;
            }

            ObjectGuid destinationGuid = selection->menuGuids[index];
            std::string message;
            ReforgeFabled(player, selection->sourceGuid, destinationGuid, message);
            ChatHandler(player->GetSession()).SendSysMessage(message);
            CloseGossipMenuFor(player);
            return true;
        }
        if (action >= GOSSIP_ACTION_REFORGE_SOURCE)
        {
            uint32 index = action - GOSSIP_ACTION_REFORGE_SOURCE;
            if (index >= selection->menuGuids.size()
                || !FindCarriedItem(player, selection->menuGuids[index]))
            {
                BuildSourceMenu(player, creature);
                return true;
            }

            selection->sourceGuid = selection->menuGuids[index];
            BuildDestinationMenu(player, creature);
            return true;
        }

        BuildSourceMenu(player, creature);
        return true;
    }
};

class TertiaryStatsTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(_settings.enabled && _dbcReady,
            "tertiary system enabled with custom spells");
        context.Expect(ClampAuraDuration(std::numeric_limits<uint32>::max())
                == uint32(std::numeric_limits<int32>::max()),
            "aura durations clamp to the signed duration range");

        Fabled::Settings fabledSettings = Fabled::GetSettings();
        float savedQualityFabledChance = fabledSettings.chance;
        std::array<float, 3> savedFabledMultipliers = fabledSettings.chanceMultipliers;
        fabledSettings.chance = 3.0f;
        fabledSettings.chanceMultipliers = { 1.0f, 2.0f, 4.0f };
        bool qualityChancesMatch = _settings.rollChance == 70.0f
            && FabledRollChance(ITEM_QUALITY_UNCOMMON, fabledSettings) == 3.0f
            && FabledRollChance(ITEM_QUALITY_RARE, fabledSettings) == 6.0f
            && FabledRollChance(ITEM_QUALITY_EPIC, fabledSettings) == 12.0f;
        fabledSettings.chance = savedQualityFabledChance;
        fabledSettings.chanceMultipliers = savedFabledMultipliers;
        context.Expect(qualityChancesMatch,
            "power slots cascade at seventy percent while quality scales fabled conversion");
        bool anchorMapMatches =
            Fabled::AnchorFor(Fabled::Effect::Impact) == Fabled::Anchor::Feet
            && Fabled::AnchorFor(Fabled::Effect::Premonition) == Fabled::Anchor::Trinket
            && Fabled::AnchorFor(Fabled::Effect::Momentum) == Fabled::Anchor::Finger
            && Fabled::AnchorFor(Fabled::Effect::Ghostwalk) == Fabled::Anchor::Back
            && Fabled::AnchorFor(Fabled::Effect::Cavalier) == Fabled::Anchor::Legs
            && Fabled::AnchorFor(Fabled::Effect::Keeper) == Fabled::Anchor::Chest
            && Fabled::AnchorFor(Fabled::Effect::VengefulGhost) == Fabled::Anchor::Trinket
            && Fabled::AnchorFor(Fabled::Effect::Crossfire) == Fabled::Anchor::Finger
            && Fabled::AnchorFor(Fabled::Effect::AlchemistsGut) == Fabled::Anchor::Trinket
            && Fabled::AnchorFor(Fabled::Effect::LeviathansGift) == Fabled::Anchor::Head
            && Fabled::AnchorFor(Fabled::Effect::Overkill) == Fabled::Anchor::Weapon
            && Fabled::AnchorFor(Fabled::Effect::BloodMagic) == Fabled::Anchor::Finger
            && Fabled::AnchorFor(Fabled::Effect::Warcaster) == Fabled::Anchor::Finger
            && Fabled::AnchorFor(Fabled::Effect::Indomitable) == Fabled::Anchor::Trinket;
        context.Expect(anchorMapMatches,
            "combat Fabled effects share ring and trinket anchors");

        std::span<Fabled::Effect const> fingerPool =
            Fabled::EffectPoolFor(Fabled::Anchor::Finger);
        std::span<Fabled::Effect const> trinketPool =
            Fabled::EffectPoolFor(Fabled::Anchor::Trinket);
        bool poolsMatch = fingerPool.size() == 4
            && fingerPool[0] == Fabled::Effect::Crossfire
            && fingerPool[1] == Fabled::Effect::Momentum
            && fingerPool[2] == Fabled::Effect::Warcaster
            && fingerPool[3] == Fabled::Effect::BloodMagic
            && trinketPool.size() == 4
            && trinketPool[0] == Fabled::Effect::Premonition
            && trinketPool[1] == Fabled::Effect::VengefulGhost
            && trinketPool[2] == Fabled::Effect::AlchemistsGut
            && trinketPool[3] == Fabled::Effect::Indomitable
            && Fabled::EffectPoolFor(Fabled::Anchor::Shoulders).empty()
            && Fabled::EffectPoolFor(Fabled::Anchor::Neck).empty()
            && Fabled::EffectPoolFor(Fabled::Anchor::Wrists).empty()
            && Fabled::EffectPoolFor(Fabled::Anchor::Hands).empty()
            && Fabled::EffectPoolFor(Fabled::Anchor::Waist).empty();
        context.Expect(poolsMatch,
            "rings and trinkets expose four-effect pools while quiet slots expose none");

        ItemTemplate const* boots = sObjectMgr->GetItemTemplate(16859);
        ItemTemplate const* trinket = sObjectMgr->GetItemTemplate(90000);
        ItemTemplate const* weapon = sObjectMgr->GetItemTemplate(TEST_SUFFIX_ITEM_ENTRY);
        context.Expect(boots && trinket && weapon
                && Fabled::EffectPoolFor(boots).size() == 1
                && Fabled::EffectPoolFor(boots)[0] == Fabled::Effect::Impact
                && Fabled::EffectPoolFor(trinket).size() == 4
                && Fabled::EffectPoolFor(weapon).size() == 1
                && Fabled::EffectPoolFor(weapon)[0] == Fabled::Effect::Overkill,
            "item inventory types resolve to their assigned Fabled pools");
        if (!actor || !_settings.enabled || !_dbcReady)
        {
            context.Finish();
            return;
        }
        ItemTemplate const* heirloomTrinket = sObjectMgr->GetItemTemplate(90000);
        uint16 fixedFleetfoot = EncodeBonusId(PowerBit(Power::Fleetfoot), 100);
        uint32 profiledSeed = AddHeirloomProfile(
            MakeItemBonusSeed(fixedFleetfoot), HeirloomProfile::Trinket);
        BonusDefinition resolvedProfiled;
        BonusDefinition resolvedLegacy;
        bool profiledResolved = DecodeBonusId(
            ResolvedTertiaryBonusFromSeed(profiledSeed, actor), resolvedProfiled);
        bool legacyResolved = DecodeBonusId(
            ResolvedTertiaryBonusFromSeed(
                MakeItemBonusSeed(fixedFleetfoot), actor, heirloomTrinket),
            resolvedLegacy);
        uint32 expectedHeirloomPoints =
            HeirloomPointBudget(HeirloomProfile::Trinket, actor->GetLevel());
        context.Expect(heirloomTrinket
                && HeirloomProfileFor(heirloomTrinket) == HeirloomProfile::Trinket
                && profiledResolved && legacyResolved
                && resolvedProfiled.powerMask == PowerBit(Power::Fleetfoot)
                && resolvedProfiled.pointsPerPower == expectedHeirloomPoints
                && resolvedLegacy.pointsPerPower == expectedHeirloomPoints,
            "heirloom tertiary points resolve from the wearer's level",
            "level=" + std::to_string(actor->GetLevel())
                + " points=" + std::to_string(resolvedProfiled.pointsPerPower));

        Settings heirloomRollSettings = _settings;
        heirloomRollSettings.rollChance = 100.0f;
        heirloomRollSettings.ordinaryChance = 100.0f;
        heirloomRollSettings.maxQuality = ITEM_QUALITY_EPIC;
        Fabled::Settings heirloomFabledSettings = Fabled::GetSettings();
        heirloomFabledSettings.chance = 0.0f;
        Item* generatedHeirloom = nullptr;
        {
            ScopedItemRollSettings scopedHeirloomSettings(
                heirloomRollSettings, heirloomFabledSettings);
            generatedHeirloom = Item::CreateItem(90000, 1, actor);
        }
        context.Expect(generatedHeirloom && TertiaryBonusOf(generatedHeirloom)
                && HeirloomProfileFromSeed(generatedHeirloom->GetBonusSeed()) == HeirloomProfile::Trinket,
            "heirlooms remain explicitly eligible when ordinary quality eligibility ends at epic");
        if (generatedHeirloom)
        {
            generatedHeirloom->RemoveFromWorld();
            delete generatedHeirloom;
        }

        Settings deterministicRollSettings = _settings;
        deterministicRollSettings.rollChance = 100.0f;
        Fabled::Settings deterministicFabledSettings = Fabled::GetSettings();
        deterministicFabledSettings.chance = 100.0f;
        deterministicFabledSettings.chanceMultipliers.fill(1.0f);
        Item* deterministicFabledItem = nullptr;
        {
            ScopedItemRollSettings scopedFabledSettings(
                deterministicRollSettings, deterministicFabledSettings);
            deterministicFabledItem = Item::CreateItem(16859, 1, actor);
        }
        context.Expect(deterministicFabledItem
                && Fabled::EffectFromBonusId(TertiaryBonusOf(deterministicFabledItem))
                    == Fabled::Effect::Impact,
            "Fabled conversion chooses the effect assigned to the item's slot");
        if (deterministicFabledItem)
        {
            deterministicFabledItem->RemoveFromWorld();
            delete deterministicFabledItem;
        }

        Settings suffixRollSettings = _settings;
        suffixRollSettings.rollChance = 100.0f;
        suffixRollSettings.ordinaryChance = 100.0f;
        Fabled::Settings suffixFabledSettings = Fabled::GetSettings();
        suffixFabledSettings.chance = 0.0f;
        Item* suffixItem = nullptr;
        {
            ScopedItemRollSettings scopedSuffixSettings(
                suffixRollSettings, suffixFabledSettings);
            suffixItem = Item::CreateItem(TEST_SUFFIX_ITEM_ENTRY, 1, actor);
        }
        int32 suffixProperty = suffixItem ? suffixItem->GetItemRandomPropertyId() : 0;
        uint32 suffixSeed = suffixItem ? suffixItem->GetBonusSeed() : 0;
        context.Expect(suffixItem && suffixProperty < 0 && suffixItem->GetItemSuffixFactor()
                && (suffixSeed >> ITEM_BONUS_SEED_VERSION_SHIFT) == ITEM_BONUS_SEED_VERSION,
            "random-suffix items preserve their suffix factor and receive a versioned bonus seed",
            "property=" + std::to_string(suffixProperty)
                + " suffix=" + std::to_string(suffixItem ? suffixItem->GetItemSuffixFactor() : 0)
                + " seed=" + std::to_string(suffixSeed));
        if (suffixItem)
        {
            suffixItem->RemoveFromWorld();
            delete suffixItem;
        }
        bool sourceAdded = actor->AddItem(16957, 1);
        bool destinationAdded = actor->AddItem(12006, 1);
        Item* reforgeSource = sourceAdded ? actor->GetItemByEntry(16957) : nullptr;
        Item* reforgeDestination = destinationAdded ? actor->GetItemByEntry(12006) : nullptr;
        ObjectGuid reforgeSourceGuid = reforgeSource ? reforgeSource->GetGUID() : ObjectGuid::Empty;
        ObjectGuid reforgeDestinationGuid =
            reforgeDestination ? reforgeDestination->GetGUID() : ObjectGuid::Empty;
        uint32 savedMoney = actor->GetMoney();
        uint32 expectedReforgeCost = reforgeDestination
            ? FabledReforgeCost(reforgeDestination->GetTemplate()) : 0;
        if (reforgeSource && reforgeDestination)
        {
            reforgeSource->SetBonusSeed(
                MakeItemBonusSeed(Fabled::BonusId(Fabled::Effect::Warcaster)));
            reforgeDestination->SetBonusSeed(MakeItemBonusSeed(fixedFleetfoot));
            reforgeSource->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
            reforgeSource->SetRefundRecipient(actor->GetGUID().GetCounter());
            reforgeSource->SetPaidMoney(123);
            AllowedLooterSet sourceAllowedLooters = { actor->GetGUID() };
            reforgeSource->SetSoulboundTradeable(sourceAllowedLooters);
            actor->AddTradeableItem(reforgeSource);
            actor->SetMoney(expectedReforgeCost + 12345);
            CharacterDatabaseTransaction fixtureTransaction = CharacterDatabase.BeginTransaction();
            actor->SaveInventoryAndGoldToDB(fixtureTransaction);
            CharacterDatabase.AsyncCommitTransaction(fixtureTransaction).m_future.get();
            CharacterDatabase.DirectExecute(
                "REPLACE INTO `item_refund_instance` (`item_guid`, `player_guid`, `paidMoney`, `paidExtendedCost`) VALUES ({}, {}, 123, 0)",
                reforgeSourceGuid.GetCounter(), actor->GetGUID().GetCounter());
            CharacterDatabase.DirectExecute(
                "REPLACE INTO `item_soulbound_trade_data` (`itemGuid`, `allowedPlayers`) VALUES ({}, '{}')",
                reforgeSourceGuid.GetCounter(), actor->GetGUID().GetCounter());
        }
        std::string reforgeMessage;
        bool reforged = reforgeSource && reforgeDestination
            && ReforgeFabled(actor, reforgeSourceGuid, reforgeDestinationGuid, reforgeMessage);
        Item* reforgedDestination = actor->GetItemByGuid(reforgeDestinationGuid);
        _reforgeSourceGuid = reforged ? reforgeSourceGuid : ObjectGuid::Empty;
        context.Expect(reforged && !actor->GetItemByGuid(reforgeSourceGuid)
                && reforgedDestination
                && Fabled::EffectFromBonusId(TertiaryBonusOf(reforgedDestination))
                    == Fabled::Effect::Warcaster
                && reforgedDestination->IsSoulBound()
                && actor->GetMoney() == 12345,
            "reforging destroys the source, binds the destination, and charges gold in one transaction",
            reforgeMessage);
        if (reforgedDestination)
            actor->DestroyItem(reforgedDestination->GetBagSlot(), reforgedDestination->GetSlot(), true);
        if (Item* leftoverSource = actor->GetItemByGuid(reforgeSourceGuid))
            actor->DestroyItem(leftoverSource->GetBagSlot(), leftoverSource->GetSlot(), true);
        actor->SetMoney(savedMoney);

        bool rollbackSourceAdded = actor->AddItem(16958, 1);
        Item* rollbackSource = rollbackSourceAdded ? actor->GetItemByEntry(16958) : nullptr;
        ObjectGuid rollbackSourceGuid = rollbackSource ? rollbackSource->GetGUID() : ObjectGuid::Empty;
        if (rollbackSource)
        {
            rollbackSource->SetFlag(ITEM_FIELD_FLAGS, ITEM_FIELD_FLAG_REFUNDABLE);
            AllowedLooterSet rollbackAllowedLooters = { actor->GetGUID() };
            rollbackSource->SetSoulboundTradeable(rollbackAllowedLooters);
            CharacterDatabaseTransaction fixtureSave = CharacterDatabase.BeginTransaction();
            actor->SaveInventoryAndGoldToDB(fixtureSave);
            CharacterDatabase.AsyncCommitTransaction(fixtureSave).m_future.get();
            CharacterDatabase.DirectExecute(
                "REPLACE INTO `item_refund_instance` (`item_guid`, `player_guid`, `paidMoney`, `paidExtendedCost`) VALUES ({}, {}, 1, 0)",
                rollbackSourceGuid.GetCounter(), actor->GetGUID().GetCounter());
            CharacterDatabase.DirectExecute(
                "REPLACE INTO `item_soulbound_trade_data` (`itemGuid`, `allowedPlayers`) VALUES ({}, '{}')",
                rollbackSourceGuid.GetCounter(), actor->GetGUID().GetCounter());

            {
                CharacterDatabaseTransaction discarded = CharacterDatabase.BeginTransaction();
                actor->DestroyItem(rollbackSource->GetBagSlot(), rollbackSource->GetSlot(), false, &discarded);
            }
            bool refundSurvived = bool(CharacterDatabase.Query(
                "SELECT 1 FROM `item_refund_instance` WHERE `item_guid` = {}", rollbackSourceGuid.GetCounter()));
            bool tradeSurvived = bool(CharacterDatabase.Query(
                "SELECT 1 FROM `item_soulbound_trade_data` WHERE `itemGuid` = {}", rollbackSourceGuid.GetCounter()));
            context.Expect(refundSurvived && tradeSurvived,
                "discarding item destruction leaves ancillary refund and BOP-trade rows intact");

            CharacterDatabase.DirectExecute(
                "DELETE FROM `item_refund_instance` WHERE `item_guid` = {}", rollbackSourceGuid.GetCounter());
            CharacterDatabase.DirectExecute(
                "DELETE FROM `item_soulbound_trade_data` WHERE `itemGuid` = {}", rollbackSourceGuid.GetCounter());
            rollbackSource->RemoveFromWorld();
            CharacterDatabaseTransaction fixtureCleanup = CharacterDatabase.BeginTransaction();
            actor->SaveInventoryAndGoldToDB(fixtureCleanup);
            CharacterDatabase.AsyncCommitTransaction(fixtureCleanup).m_future.get();
        }


        TertiaryState* echoState = GetState(actor);
        Creature* deadEchoAnchor = context.SpawnDummy(3.0f, -0.6f);
        Creature* liveEchoFallback = context.SpawnDummy(3.0f, 0.6f);
        bool echoTargetsReady = deadEchoAnchor && liveEchoFallback
            && context.Engage(deadEchoAnchor->GetGUID())
            && context.Engage(liveEchoFallback->GetGUID());
        context.Expect(echoTargetsReady, "Echo dead-anchor targets are engaged");
        if (echoTargetsReady)
        {
            ObjectGuid deadAnchorGuid = deadEchoAnchor->GetGUID();
            ObjectGuid liveFallbackGuid = liveEchoFallback->GetGUID();
            uint32 fallbackHealth = liveEchoFallback->GetHealth();
            actor->SetSelection(deadAnchorGuid);
            echoState->echoActive = true;
            echoState->echoEndsAtMs = GameTime::GetGameTimeMS().count();
            echoState->echoPercent = 100.0f;
            echoState->echoDamage = 1000;
            echoState->echoDamageAnchor = deadAnchorGuid;
            context.Expect(context.DespawnDummy(deadAnchorGuid),
                "Echo's recorded anchor despawns before release");
            context.Expect(context.Engage(liveFallbackGuid),
                "Echo fallback remains engaged after its recorded anchor despawns");
            ProcessEcho(actor, *echoState);
            liveEchoFallback = context.GetCreature(liveFallbackGuid);
            context.Expect(liveEchoFallback && liveEchoFallback->GetHealth() < fallbackHealth,
                "Echo falls back to another engaged enemy when its anchor is gone");
            actor->SetSelection(ObjectGuid::Empty);
        }
        ResetEcho(actor, *echoState);
        context.DespawnAllDummies();


        Creature* dummy = context.SpawnDummy();
        context.Expect(dummy != nullptr && context.Engage(dummy ? dummy->GetGUID() : ObjectGuid::Empty),
            "hostile test target available");
        if (!dummy)
        {
            context.Finish();
            return;
        }
        _dummyGuid = dummy->GetGUID();

        constexpr uint32 TEST_ITEM = 14136; // Robe of Winter Night: unrestricted rare cloth.
        constexpr uint8 ORDINARY_MASK =
            PowerBit(Power::Avoidance) | PowerBit(Power::Fleetfoot) | PowerBit(Power::Siphon);
        Settings ordinaryRollSettings = _settings;
        ordinaryRollSettings.rollChance = 100.0f;
        ordinaryRollSettings.ordinaryChance = 100.0f;
        ordinaryRollSettings.weights.fill(0.0f);
        ordinaryRollSettings.weights[PowerIndex(Power::Avoidance)] = 1.0f;
        ordinaryRollSettings.weights[PowerIndex(Power::Fleetfoot)] = 1.0f;
        ordinaryRollSettings.weights[PowerIndex(Power::Siphon)] = 1.0f;
        Fabled::Settings ordinaryFabledSettings = Fabled::GetSettings();
        ordinaryFabledSettings.chance = 0.0f;
        uint16 equipmentPosition = uint16(INVENTORY_SLOT_BAG_0) << 8 | EQUIPMENT_SLOT_CHEST;
        {
            ScopedItemRollSettings scopedOrdinarySettings(
                ordinaryRollSettings, ordinaryFabledSettings);
            _item = actor->EquipNewItem(equipmentPosition, TEST_ITEM, true);
        }
        context.Expect(_item && _item->GetEntry() == TEST_ITEM, "eligible test item equipped");
        if (!_item || _item->GetEntry() != TEST_ITEM)
        {
            context.Finish();
            return;
        }

        _itemGuid = _item->GetGUID();
        _pointBudget = ItemPointBudget(_item->GetTemplate());
        context.Expect(_pointBudget > 0, "native item point budget resolved",
            "points=" + std::to_string(_pointBudget));
        if (!_pointBudget)
        {
            context.Finish();
            return;
        }
        _nativeProperty = _item->GetItemRandomPropertyId();
        _nativeSuffixFactor = _item->GetItemSuffixFactor();

        uint16 generatedBonusId = TertiaryBonusOf(_item);
        BonusDefinition generatedDefinition;
        bool definitionResolved = DecodeBonusId(generatedBonusId, generatedDefinition);
        context.Expect(IsEligibleEquipment(_item->GetTemplate()) && definitionResolved
                && generatedDefinition.powerMask == ORDINARY_MASK
                && generatedDefinition.pointsPerPower == _pointBudget,
            "generated item deterministically decodes its three-power bonus",
            "bonusId=" + std::to_string(generatedBonusId));
        context.Expect(TertiaryDefinitionWirePayload(generatedBonusId)
                == Acore::StringFormat("O:{}:0:1:2", _pointBudget)
                && TertiaryDefinitionWirePayload(Fabled::BonusId(Fabled::Effect::Warcaster))
                    == "F:13",
            "item definitions serialize as semantic ordinary and Fabled protocol records");
        uint32 bonusSeed = _item->GetBonusSeed();
        context.Expect((bonusSeed >> ITEM_BONUS_SEED_VERSION_SHIFT) == ITEM_BONUS_SEED_VERSION
                && TertiaryBonusOf(_item) == generatedBonusId,
            "item bonus seed reproduces the generated bonus");
        context.Expect(_item->GetItemRandomPropertyId() == _nativeProperty
                && _item->GetItemSuffixFactor() == _nativeSuffixFactor,
            "tertiary assignment preserves the native random affix");

        TertiaryState* state = RefreshForMask(actor, ORDINARY_MASK);
        context.Expect(HasExactPoints(*state, ORDINARY_MASK), "ordinary points aggregate from equipped item");
        context.Expect(actor->HasAura(SPELL_AVOIDANCE_PASSIVE)
                && actor->HasAura(SPELL_FLEETFOOT_GROUND_PASSIVE)
                && actor->HasAura(SPELL_FLEETFOOT_FLIGHT_PASSIVE),
            "avoidance and fleetfoot passives active");

        constexpr std::array<uint32, 6> KINGS_AURAS = {
            20217, 25898, 43223, 56525, 58054, 72586
        };
        constexpr uint32 SPELL_CELEBRATE_GOOD_TIMES = 26035;
        for (uint32 spellId : KINGS_AURAS)
            actor->RemoveAurasDueToSpell(spellId);
        actor->RemoveAurasDueToSpell(SPELL_CELEBRATE_GOOD_TIMES);
        state = EnsureState(actor);

        auto verifyAllStatAura = [&](uint32 spellId, char const* label)
        {
            float multiplierBefore = AllStatPercentMultiplier(actor);
            float pointsBefore = Points(*state, Power::Fleetfoot);
            actor->CastSpell(actor, spellId, true);
            Aura* aura = actor->GetAura(spellId);
            state = EnsureState(actor);
            float multiplierAfter = AllStatPercentMultiplier(actor);
            float expectedPoints = pointsBefore * multiplierAfter / multiplierBefore;
            Aura* fleetfootAura = actor->GetAura(SPELL_FLEETFOOT_GROUND_PASSIVE);
            int32 fleetfootAmount = fleetfootAura ? fleetfootAura->GetEffect(EFFECT_0)->GetAmount() : 0;

            context.Expect(aura && aura->GetEffect(EFFECT_0)->GetAmount() == 10
                    && multiplierAfter > multiplierBefore
                    && std::fabs(Points(*state, Power::Fleetfoot) - expectedPoints) < 0.001f
                    && fleetfootAmount == int32(std::lround(Fleetfoot(*state))),
                std::string(label) + " scales tertiary stats",
                "multiplier=" + std::to_string(multiplierAfter)
                    + ",points=" + std::to_string(Points(*state, Power::Fleetfoot))
                    + ",fleetfoot=" + std::to_string(fleetfootAmount));

            actor->RemoveAurasDueToSpell(spellId);
            state = EnsureState(actor);
            context.Expect(!actor->HasAura(spellId)
                    && std::fabs(Points(*state, Power::Fleetfoot) - pointsBefore) < 0.001f,
                std::string(label) + " removal restores tertiary stats");
        };

        verifyAllStatAura(20217, "Blessing of Kings");
        verifyAllStatAura(SPELL_CELEBRATE_GOOD_TIMES, "base-percent all-stat buffs");

        uint32 environmentalDamage = 1000;
        sScriptMgr->OnPlayerEnvironmentalDamage(actor, DAMAGE_FALL, environmentalDamage);
        context.Expect(environmentalDamage < 1000, "avoidance reduces environmental damage",
            "damage=" + std::to_string(environmentalDamage));

        actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 2));
        uint32 healthBeforeSiphon = actor->GetHealth();
        context.Expect(context.Damage(_dummyGuid, 1000), "ordinary damage action executed");
        context.Expect(actor->GetHealth() > healthBeforeSiphon, "siphon heals from effective damage",
            "before=" + std::to_string(healthBeforeSiphon)
                + ",after=" + std::to_string(actor->GetHealth()));

        actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 2));
        uint32 healthBeforeSmallSiphon = actor->GetHealth();
        context.Expect(context.Damage(_dummyGuid, 1), "small ordinary damage action executed");
        context.Expect(actor->GetHealth() == healthBeforeSmallSiphon + 1,
            "fractional siphon healing rounds up",
            "before=" + std::to_string(healthBeforeSmallSiphon)
                + ",after=" + std::to_string(actor->GetHealth()));
        context.Expect(ScaledAmount(100, 1.0f) == 1 && ScaledAmount(150, 2.0f) == 3,
            "whole-number siphon healing remains exact");
        SpellInfo const* nativeHealSpell = sSpellMgr->GetSpellInfo(2061); // Flash Heal.
        context.Expect(nativeHealSpell != nullptr, "native Siphon healing spell resolved");
        if (nativeHealSpell)
        {
            dummy->SetHealth(std::max<uint32>(1, dummy->GetMaxHealth() / 2));
            actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 2));
            uint32 healthBeforeHealingSiphon = actor->GetHealth();
            HealInfo nativeHeal(actor, dummy, 1000, nativeHealSpell,
                nativeHealSpell->GetSchoolMask());
            actor->HealBySpell(nativeHeal);
            context.Expect(nativeHeal.GetEffectiveHeal() > 0
                    && actor->GetHealth() > healthBeforeHealingSiphon,
                "Siphon heals from effective native healing",
                "effective=" + std::to_string(nativeHeal.GetEffectiveHeal()));
        }

        SpellInfo const* procDisabledSpell = sSpellMgr->GetSpellInfo(2136); // Fire Blast
        context.Expect(procDisabledSpell != nullptr, "proc-disabled Siphon spell resolved");
        if (procDisabledSpell)
        {
            actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 2));
            uint32 healthBeforeProcDisabledSiphon = actor->GetHealth();
            uint32 targetHealthBefore = dummy->GetHealth();
            actor->CastSpell(dummy, procDisabledSpell->Id,
                TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS));
            context.Expect(dummy->GetHealth() < targetHealthBefore,
                "proc-disabled class spell deals damage");
            context.Expect(actor->GetHealth() > healthBeforeProcDisabledSiphon,
                "Siphon accepts legitimate proc-disabled class damage",
                "before=" + std::to_string(healthBeforeProcDisabledSiphon)
                    + ",after=" + std::to_string(actor->GetHealth()));
        }
        context.Expect(sScriptMgr->CanItemLoseDurability(actor, _item),
            "non-unbroken item can lose durability");

        constexpr uint8 EXOTIC_MASK =
            PowerBit(Power::Tempo) | PowerBit(Power::Echo) | PowerBit(Power::Unbroken);
        state = RefreshForMask(actor, EXOTIC_MASK);
        context.Expect(HasExactPoints(*state, EXOTIC_MASK), "exotic points aggregate from equipped item");
        context.Expect(!actor->HasAura(SPELL_AVOIDANCE_PASSIVE)
                && !actor->HasAura(SPELL_FLEETFOOT_GROUND_PASSIVE)
                && !actor->HasAura(SPELL_FLEETFOOT_FLIGHT_PASSIVE),
            "ordinary passives removed after equipment changes");

        Settings& procTestSettings = TestSettings(actor);
        procTestSettings.tempoPpmBase = 60.0f;
        procTestSettings.tempoPpmPerPoint = 0.0f;
        procTestSettings.echoPpmBase = 60.0f;
        procTestSettings.echoPpmPerPoint = 0.0f;
        state->lastEchoCheckMs = 0;
        state->lastTempoCheckMs = 0;
        bool exoticDamageExecuted = context.Damage(_dummyGuid, 1000);
        context.Expect(exoticDamageExecuted, "exotic damage action executed");
        context.Expect(actor->HasAura(SPELL_TEMPO)
                && actor->HasAura(SPELL_TEMPO_COOLDOWN),
            "tempo procs separate speed and cooldown-charge auras");
        Aura* tempoAura = actor->GetAura(SPELL_TEMPO);
        context.Expect(tempoAura
                && tempoAura->GetMaxDuration() == int32(_settings.tempoDurationMs),
            "tempo aura matches the configured duration",
            "duration=" + std::to_string(tempoAura ? tempoAura->GetMaxDuration() : 0));
        context.Expect(actor->HasAura(SPELL_ECHO_AURA) && state->echoActive && state->echoDamage > 0,
            "echo opens and stores effective damage",
            "stored=" + std::to_string(state->echoDamage));
        if (Aura* charge = actor->GetAura(SPELL_TEMPO_COOLDOWN))
            charge->SetDuration(1);
        if (tempoAura)
            tempoAura->SetDuration(1);
        state->lastTempoCheckMs = 0;
        TryTempo(actor, *state);
        context.Expect(tempoAura && tempoAura->GetDuration() == int32(_settings.tempoDurationMs)
                && actor->GetAura(SPELL_TEMPO_COOLDOWN)
                && actor->GetAura(SPELL_TEMPO_COOLDOWN)->GetDuration()
                    == int32(_settings.tempoDurationMs),
            "a new Tempo proc refreshes both aura durations");

        actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 2));
        uint64 tertiaryHealingBefore = state->echoHealing;
        CastTertiarySpell(actor, *state, actor, SPELL_SIPHON_HEAL, 100);
        context.Expect(state->echoHealing > tertiaryHealingBefore,
            "active Echo records effective tertiary healing");

        ResetEcho(actor, *state);
        actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 2));
        state->lastEchoCheckMs = 0;
        CastTertiarySpell(actor, *state, actor, SPELL_SIPHON_HEAL, 100);
        context.Expect(!state->echoActive,
            "tertiary output cannot awaken a new Echo");
        ClearTestSettings(actor);
        float expectedEchoPpm = _settings.echoPpmBase
            + Points(*state, Power::Echo) * _settings.echoPpmPerPoint;
        context.Expect(std::fabs(EchoPpm(*state) - expectedEchoPpm) < 0.001f,
            "Echo PPM scales with equipped Echo points");

        SpellInfo const* cooldownSpellInfo = sSpellMgr->GetSpellInfo(2136); // Fire Blast
        context.Expect(cooldownSpellInfo != nullptr, "tempo test spell resolved");
        if (cooldownSpellInfo)
        {
            Spell cooldownSpell(actor, cooldownSpellInfo, TRIGGERED_NONE);
            actor->RemoveSpellCooldown(cooldownSpellInfo->Id);
            actor->AddSpellCooldown(cooldownSpellInfo->Id, 0, 10000);
            uint32 before = actor->GetSpellCooldownDelay(cooldownSpellInfo->Id);
            sScriptMgr->OnPlayerSpellCast(actor, &cooldownSpell, false);
            bool claimed = state->tempoPendingCast == &cooldownSpell;
            sScriptMgr->OnSpellCast(&cooldownSpell, actor, cooldownSpellInfo, false);
            uint32 after = actor->GetSpellCooldownDelay(cooldownSpellInfo->Id);
            context.Expect(claimed && after < before && !state->tempoPendingCast,
                "tempo reduces one eligible native cooldown",
                "before=" + std::to_string(before) + ",after=" + std::to_string(after));
            CastTertiarySpell(actor, *state, actor, SPELL_TEMPO_COOLDOWN, 0, 0, 0,
                _settings.tempoDurationMs);
            sScriptMgr->OnPlayerSpellCast(actor, &cooldownSpell, false);
            bool cancelClaimed = state->tempoPendingCast == &cooldownSpell;
            sScriptMgr->OnSpellCastCancel(&cooldownSpell, actor, cooldownSpellInfo, true);
            context.Expect(cancelClaimed && !state->tempoPendingCast
                    && actor->HasAura(SPELL_TEMPO_COOLDOWN),
                "a canceled cast restores the Tempo cooldown charge");
            sScriptMgr->OnPlayerSpellCast(actor, &cooldownSpell, false);
            bool abandonedClaimed = state->tempoPendingCast == &cooldownSpell;
            sScriptMgr->OnPlayerUpdate(actor, 0);
            context.Expect(abandonedClaimed && !state->tempoPendingCast
                    && actor->HasAura(SPELL_TEMPO_COOLDOWN),
                "a failed cast recheck restores the Tempo cooldown charge");
        }

        actor->RemoveSpellCooldown(SPELL_UNBROKEN);
        actor->RemoveAurasDueToSpell(SPELL_UNBROKEN);
        actor->CastSpell(actor, SPELL_BROWN_HORSE, true);
        context.Expect(actor->IsMounted(), "unbroken daze test starts mounted");
        dummy->CastSpell(actor, SPELL_DAZED, true);
        context.Expect(state->pendingUnbroken && actor->HasAura(SPELL_DAZED),
            "mounted daze arms unbroken");
        context.Expect(actor->IsMounted(), "unbroken prevents daze from dismounting");
        sScriptMgr->OnPlayerUpdate(actor, 0);
        context.Expect(!actor->HasAura(SPELL_DAZED) && actor->IsMounted(),
            "unbroken removes daze without removing the mount");
        actor->RemoveAurasByType(SPELL_AURA_MOUNTED);
        actor->Dismount();

        actor->RemoveSpellCooldown(SPELL_UNBROKEN);
        actor->RemoveAurasDueToSpell(SPELL_UNBROKEN);
        actor->SetHealth(actor->GetMaxHealth());
        uint32 thresholdDamage = uint32(std::ceil(double(actor->GetMaxHealth()) * 0.70));
        Unit::DealDamage(dummy, actor, thresholdDamage, nullptr, DIRECT_DAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false, true);
        context.Expect(state->pendingUnbroken, "unbroken arms after a nonlethal threshold crossing");

        _stage = Stage::AwaitUnbroken;
        _elapsed = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        _elapsed += diff;
        if (_stage == Stage::AwaitAreaBaseline || _stage == Stage::AwaitAreaEmpowered)
        {
            Player* actor = context.GetActor();
            if (!actor)
            {
                context.Fail("area test actor remains available");
                return;
            }

            int64 damage = FindAreaDamage(context);
            if (!damage)
            {
                if (_elapsed < 3000)
                    return;
                context.Fail(_stage == Stage::AwaitAreaBaseline
                    ? "baseline Death and Decay deals damage"
                    : "Opportunity Death and Decay deals damage");
                RestoreShadowCrit(actor);
                return;
            }

            if (_stage == Stage::AwaitAreaBaseline)
            {
                _baselineAreaDamage = damage;
                context.Expect(damage > 0, "baseline Death and Decay deals damage",
                    "damage=" + std::to_string(damage));
                TertiaryState* state = EnsureState(actor);
                PeriodicAreaState const* area = FindPeriodicArea(*state, _areaOwnerGuid,
                    actor->GetGUID(), 49938);
                context.Expect(area && area->damaging && _areaPeriodicEffect,
                    "Death and Decay promotes its exact periodic area after damaging output");

                PeriodicAreaState otherOwner{
                    _areaDummyGuid, actor->GetGUID(), 49938, false, true };
                state->periodicAreas.push_back(otherOwner);
                PeriodicAreaState const* exactArea = FindPeriodicArea(*state, _areaOwnerGuid,
                    actor->GetGUID(), 49938);
                PeriodicAreaState const* isolatedArea = FindPeriodicArea(*state, _areaDummyGuid,
                    actor->GetGUID(), 49938);
                context.Expect(exactArea && exactArea->damaging && !exactArea->opportunity
                        && isolatedArea && !isolatedArea->damaging && isolatedArea->opportunity,
                    "same-spell area state remains isolated by source owner GUID");
                state->periodicAreas.pop_back();

                if (_areaPeriodicEffect)
                {
                    float savedTempoPoints = state->points[PowerIndex(Power::Tempo)];
                    float targetTempoPercent =
                        std::ceil(_settings.tempoSpeedBase / 10.0f) * 10.0f + 7.5f;
                    if (_settings.tempoSpeedPerPoint > 0.0f)
                        state->points[PowerIndex(Power::Tempo)] =
                            (targetTempoPercent - _settings.tempoSpeedBase)
                            / _settings.tempoSpeedPerPoint;

                    CastTertiarySpell(actor, *state, actor, SPELL_TEMPO, 10, 10, 10,
                        _settings.tempoDurationMs);
                    double timeRate = 1.0 + double(TempoSpeed(*state)) / 100.0;
                    constexpr uint32 SPLIT_UPDATES = 40;
                    _areaPeriodicEffect->SetPeriodicTimer(10000);
                    for (uint32 update = 0; update < SPLIT_UPDATES; ++update)
                        _areaPeriodicEffect->Update(1, actor);
                    int32 splitElapsed = 10000 - _areaPeriodicEffect->GetPeriodicTimer();
                    int32 expectedElapsed = int32(std::floor(double(SPLIT_UPDATES) * timeRate));
                    int32 elapsedWithoutRemainder =
                        int32(SPLIT_UPDATES) * int32(std::floor(timeRate));
                    context.Expect(splitElapsed == expectedElapsed
                            && expectedElapsed != elapsedWithoutRemainder,
                        "Tempo scheduler preserves fractional time across split updates",
                        "elapsed=" + std::to_string(splitElapsed)
                            + ",expected=" + std::to_string(expectedElapsed));

                    _areaPeriodicEffect->SetPeriodicTimer(10000);
                    _areaPeriodicEffect->Update(1, actor);
                    _areaPeriodicEffect->SetPeriodicTimer(10000);
                    _areaPeriodicEffect->Update(5, actor);
                    context.Expect(10000 - _areaPeriodicEffect->GetPeriodicTimer()
                            == int32(std::floor(5.0 * timeRate)),
                        "setting a periodic timer clears its fractional remainder");

                    state->points[PowerIndex(Power::Tempo)] = savedTempoPoints;
                    actor->RemoveAurasDueToSpell(SPELL_TEMPO);
                    _areaPeriodicEffect->SetPeriodicTimer(10000);
                    _areaPeriodicEffect->Update(SPLIT_UPDATES, actor);
                    context.Expect(10000 - _areaPeriodicEffect->GetPeriodicTimer()
                            == int32(SPLIT_UPDATES),
                        "area cadence returns to normal when Tempo expires");
                }
                actor->RemoveAurasDueToSpell(SPELL_TEMPO);
                _areaPeriodicEffect = nullptr;
                actor->RemoveDynObject(49938);
                context.Expect(state->periodicAreas.empty(),
                    "base aura removal synchronously releases periodic area state");
                context.Expect(StartDeathAndDecay(context, actor, *state, true),
                    "Opportunity Death and Decay creates its area");
                _stage = Stage::AwaitAreaEmpowered;
                _elapsed = 0;
                return;
            }

            TertiaryState* state = EnsureState(actor);
            context.Expect(damage > _baselineAreaDamage,
                "Opportunity critically empowers scripted area ticks",
                "baseline=" + std::to_string(_baselineAreaDamage)
                    + ",empowered=" + std::to_string(damage));
            context.Expect(!actor->HasAura(SPELL_OPPORTUNITY)
                    && std::any_of(state->periodicAreas.begin(), state->periodicAreas.end(),
                        [](PeriodicAreaState const& area) { return area.opportunity; }),
                "Death and Decay consumes Opportunity and retains its area source");
            _areaPeriodicEffect = nullptr;
            actor->RemoveDynObject(49938);
            context.Expect(state->periodicAreas.empty(),
                "expired areas release persistent Opportunity state synchronously");
            RestoreShadowCrit(actor);
            FinishFeatureTests(context, actor, state);
            return;
        }

        if (_stage == Stage::AwaitUnbroken)
        {
            if (_elapsed < 500)
                return;

            Player* actor = context.GetActor();
            if (!actor || !_item)
            {
                context.Fail("test objects remain available");
                return;
            }

            TertiaryState* state = EnsureState(actor);
            bool unbrokenReady = actor->HasAura(SPELL_UNBROKEN)
                && actor->HasSpellCooldown(SPELL_UNBROKEN)
                && !state->pendingUnbroken;
            if (!unbrokenReady && _elapsed < 3000)
                return;
            context.Expect(unbrokenReady,
                "unbroken applies its aura and internal cooldown");
            Aura* unbrokenAura = actor->GetAura(SPELL_UNBROKEN);
            uint32 expectedShield = ScaledAmount(actor->GetMaxHealth(),
                _settings.unbrokenShieldBase
                    + Points(*state, Power::Unbroken) * _settings.unbrokenShieldPerPoint);
            context.Expect(unbrokenAura && unbrokenAura->GetEffect(EFFECT_0)
                    && uint32(unbrokenAura->GetEffect(EFFECT_0)->GetAmount()) == expectedShield,
                "Unbroken shield combines its base and point scaling",
                "expected=" + std::to_string(expectedShield));
            context.Expect(!sScriptMgr->CanItemLoseDurability(actor, _item),
                "unbroken item prevents durability loss");

            ResetEcho(actor, *state);
            actor->RemoveAurasDueToSpell(SPELL_TEMPO);
            actor->RemoveAurasDueToSpell(SPELL_TEMPO_COOLDOWN);

            constexpr uint8 TEMPO_OPPORTUNITY_MASK =
                PowerBit(Power::Tempo) | PowerBit(Power::Opportunity);
            state = RefreshForMask(actor, TEMPO_OPPORTUNITY_MASK);
            context.Expect(HasExactPoints(*state, TEMPO_OPPORTUNITY_MASK),
                "Tempo and Opportunity points aggregate from the referenced bonus row");

            TertiaryState noOpportunityState;
            context.Expect(OpportunityPpm(noOpportunityState) == 0.0f,
                "opportunity base ppm requires equipped points");
            float expectedOpportunityPpm = _settings.opportunityPpmBase
                + Points(*state, Power::Opportunity) * _settings.opportunityPpmPerPoint;
            context.Expect(std::lround(OpportunityPpm(*state) * 1000.0f)
                    == std::lround(expectedOpportunityPpm * 1000.0f),
                "opportunity ppm combines its base and equipped points",
                "ppm=" + std::to_string(OpportunityPpm(*state)));

            SpellInfo const* opportunitySpellInfo = sSpellMgr->GetSpellInfo(133); // Fireball
            context.Expect(opportunitySpellInfo != nullptr, "opportunity test spell resolved");
            if (opportunitySpellInfo)
            {
                Unit* target = context.GetUnit(_dummyGuid);
                CastTertiarySpell(actor, *state, actor, SPELL_OPPORTUNITY, 0, 0, 0,
                    _settings.opportunityDurationMs);
                Spell opportunitySpell(actor, opportunitySpellInfo, TRIGGERED_NONE);
                sScriptMgr->OnPlayerSpellCast(actor, &opportunitySpell, false);
                bool claimed = state->opportunityCast == &opportunitySpell;
                float critChance = 0.0f;
                sScriptMgr->OnCalcCritChance(&opportunitySpell, target, critChance);
                sScriptMgr->OnSpellCast(&opportunitySpell, actor, opportunitySpellInfo, false);
                context.Expect(claimed && !actor->HasAura(SPELL_OPPORTUNITY)
                        && critChance == 100.0f,
                    "opportunity guarantees a critical through cast hooks",
                    "claimed=" + std::to_string(claimed)
                        + ",crit=" + std::to_string(critChance));

                CastTertiarySpell(actor, *state, actor, SPELL_OPPORTUNITY, 0, 0, 0,
                    _settings.opportunityDurationMs);
                Spell canceledOpportunitySpell(actor, opportunitySpellInfo, TRIGGERED_NONE);
                sScriptMgr->OnPlayerSpellCast(actor, &canceledOpportunitySpell, false);
                bool cancelClaimed = state->opportunityCast == &canceledOpportunitySpell;
                sScriptMgr->OnSpellCastCancel(&canceledOpportunitySpell, actor,
                    opportunitySpellInfo, true);
                context.Expect(cancelClaimed && !state->opportunityCast
                        && !state->opportunityChannel && actor->HasAura(SPELL_OPPORTUNITY),
                    "a canceled cast rearms Opportunity");
                sScriptMgr->OnPlayerSpellCast(actor, &canceledOpportunitySpell, false);
                bool abandonedClaimed =
                    state->opportunityCast == &canceledOpportunitySpell;
                sScriptMgr->OnPlayerUpdate(actor, 0);
                context.Expect(abandonedClaimed && !state->opportunityCast
                        && !state->opportunityChannel && actor->HasAura(SPELL_OPPORTUNITY),
                    "a failed cast recheck rearms Opportunity on the next update");
            }

            SpellInfo const* judgementInfo = sSpellMgr->GetSpellInfo(20271);
            SpellInfo const* judgementDamageInfo = sSpellMgr->GetSpellInfo(54158);
            context.Expect(judgementInfo && judgementDamageInfo,
                "Judgement of Light and its damage spell resolve");
            if (judgementInfo && judgementDamageInfo)
            {
                CastTertiarySpell(actor, *state, actor, SPELL_OPPORTUNITY, 0, 0, 0,
                    _settings.opportunityDurationMs);
                Spell judgementSpell(actor, judgementInfo, TRIGGERED_NONE);
                sScriptMgr->OnPlayerSpellCast(actor, &judgementSpell, false);
                bool judgementClaimed = !actor->HasAura(SPELL_OPPORTUNITY)
                    && state->opportunityCast == &judgementSpell;

                Spell judgementDamage(actor, judgementDamageInfo, TRIGGERED_FULL_MASK);
                sScriptMgr->OnPlayerSpellCast(actor, &judgementDamage, false);
                float judgementCritChance = 5.0f;
                sScriptMgr->OnCalcCritChance(&judgementDamage,
                    context.GetUnit(_dummyGuid), judgementCritChance);
                context.Expect(judgementClaimed
                        && state->opportunityCast == &judgementSpell
                        && judgementCritChance == 100.0f,
                    "Opportunity follows opaque casts into immediate damage",
                    "claimed=" + std::to_string(judgementClaimed)
                        + ",crit=" + std::to_string(judgementCritChance));
                sScriptMgr->OnSpellCast(&judgementDamage, actor, judgementDamageInfo, false);
                sScriptMgr->OnSpellCast(&judgementSpell, actor, judgementInfo, false);
                actor->RemoveAurasDueToSpell(SPELL_OPPORTUNITY);
            }

            SpellInfo const* consecrationInfo = sSpellMgr->GetSpellInfo(26573);
            context.Expect(consecrationInfo != nullptr,
                "Consecration resolves for Opportunity");
            if (consecrationInfo)
            {
                CastTertiarySpell(actor, *state, actor, SPELL_OPPORTUNITY, 0, 0, 0,
                    _settings.opportunityDurationMs);
                Spell consecration(actor, consecrationInfo, TRIGGERED_NONE);
                sScriptMgr->OnPlayerSpellCast(actor, &consecration, false);
                float periodicCritChance = 0.0f;
                sScriptMgr->OnCalcPeriodicCritChance(consecrationInfo, actor,
                    context.GetUnit(_dummyGuid), periodicCritChance);
                context.Expect(!actor->HasAura(SPELL_OPPORTUNITY)
                        && state->opportunityCast == &consecration
                        && periodicCritChance == 100.0f,
                    "Opportunity empowers Consecration periodic criticals",
                    "claimed=" + std::to_string(state->opportunityCast == &consecration)
                        + ",crit=" + std::to_string(periodicCritChance));
                sScriptMgr->OnSpellCast(&consecration, actor, consecrationInfo, false);
                context.Expect(!state->opportunityCast,
                    "completed periodic casts release transient Opportunity state");
                actor->RemoveAurasDueToSpell(SPELL_OPPORTUNITY);
            }

            SpellInfo const* deathAndDecayInfo = sSpellMgr->GetSpellInfo(49938);
            SpellInfo const* blizzardInfo = sSpellMgr->GetSpellInfo(42940);
            SpellInfo const* rainOfFireInfo = sSpellMgr->GetSpellInfo(47820);
            SpellInfo const* frostTrapInfo = sSpellMgr->GetSpellInfo(13810);
            context.Expect(deathAndDecayInfo && blizzardInfo && rainOfFireInfo
                    && frostTrapInfo && consecrationInfo,
                "representative area spells resolve");
            if (deathAndDecayInfo && blizzardInfo && rainOfFireInfo
                && frostTrapInfo && consecrationInfo)
            {
                Spell deathAndDecay(actor, deathAndDecayInfo, TRIGGERED_NONE);
                Spell blizzard(actor, blizzardInfo, TRIGGERED_NONE);
                context.Expect(HasDeferredAreaCritOutput(deathAndDecayInfo)
                        && IsOpportunityAbility(&deathAndDecay)
                        && IsPeriodicAreaCandidate(deathAndDecayInfo)
                        && !IsKnownDamagingPeriodicArea(deathAndDecayInfo),
                    "script-driven areas await runtime damage classification");
                bool const critChannel = HasCritCapableChannelOutput(blizzardInfo);
                bool const opportunityChannel = IsOpportunityAbility(&blizzard);
                bool const tempoArea = IsKnownDamagingPeriodicArea(blizzardInfo);
                context.Expect(critChannel && opportunityChannel && tempoArea,
                    "triggered-damage channels support Opportunity and Tempo",
                    "crit=" + std::to_string(critChannel)
                        + ",opportunity=" + std::to_string(opportunityChannel)
                        + ",tempo=" + std::to_string(tempoArea));
                context.Expect(IsKnownDamagingPeriodicArea(consecrationInfo),
                    "native periodic ground damage supports Tempo");
                context.Expect(IsKnownDamagingPeriodicArea(rainOfFireInfo),
                    "triggered periodic ground damage supports Tempo");
                context.Expect(!IsKnownDamagingPeriodicArea(frostTrapInfo),
                    "non-damaging ground utility remains outside Tempo");
            }

            Creature* areaDummy = context.SpawnDummy(2.0f);
            _areaDummyGuid = areaDummy ? areaDummy->GetGUID() : ObjectGuid::Empty;
            context.Expect(areaDummy != nullptr
                    && context.Engage(_areaDummyGuid),
                "Death and Decay test target is available");
            if (!areaDummy)
            {
                FinishFeatureTests(context, actor, state);
                return;
            }

            uint16 shadowCritField = uint16(PLAYER_SPELL_CRIT_PERCENTAGE1) + SPELL_SCHOOL_SHADOW;
            _savedShadowCrit = actor->GetFloatValue(shadowCritField);
            actor->SetFloatValue(shadowCritField, 0.0f);
            _shadowCritOverridden = true;
            context.Expect(StartDeathAndDecay(context, actor, *state, false),
                "baseline Death and Decay creates its area");
            _stage = Stage::AwaitAreaBaseline;
            _elapsed = 0;
            return;
        }

        if (_stage != Stage::AwaitPersistence)
            return;

        _pollElapsed += diff;
        if (_pollElapsed < 100)
            return;
        _pollElapsed = 0;

        uint32 persistedSeed = 0;
        bool rowFound = false;
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT `bonusSeed` FROM `item_instance` WHERE `guid` = {}", _itemGuid.GetCounter()))
        {
            persistedSeed = result->Fetch()[0].Get<uint32>();
            rowFound = true;
        }

        bool reforgeMetadataRemoved = !_reforgeSourceGuid
            || (!CharacterDatabase.Query(
                    "SELECT 1 FROM `item_refund_instance` WHERE `item_guid` = {}",
                    _reforgeSourceGuid.GetCounter())
                && !CharacterDatabase.Query(
                    "SELECT 1 FROM `item_soulbound_trade_data` WHERE `itemGuid` = {}",
                    _reforgeSourceGuid.GetCounter()));
        if (rowFound && uint16(persistedSeed & ITEM_BONUS_ID_MASK) == _expectedBonusId
            && reforgeMetadataRemoved)
        {
            context.Expect(true, "item atomically persists its bonus seed",
                "seed=" + std::to_string(persistedSeed));
            context.Expect(true, "reforging commits source refund and BOP-trade cleanup atomically");
            context.Finish();
            _stage = Stage::Done;
            return;
        }

        if (_elapsed < 5000)
            return;

        context.Expect(false, "item persistence and reforge metadata cleanup complete",
            (rowFound ? "seed=" + std::to_string(persistedSeed) : "item row missing")
                + " reforgeMetadata=" + std::to_string(reforgeMetadataRemoved));
        context.Finish();
        _stage = Stage::Done;
    }

    void Cancel(TestHarness::Context& context) override
    {
        if (Player* actor = context.GetActor())
        {
            actor->RemoveDynObject(49938);
            RestoreShadowCrit(actor);
        }
    }


private:
    enum class Stage
    {
        AwaitUnbroken,
        AwaitAreaBaseline,
        AwaitAreaEmpowered,
        AwaitPersistence,
        Done
    };

    int64 FindAreaDamage(TestHarness::Context& context) const
    {
        bool awaitingFinalDamage = false;
        ObjectGuid source;
        for (TestHarness::Event const& event : context.GetEvents())
        {
            if (event.type == TestHarness::EventType::SpellDamage
                && event.spellId == 52212 && event.target == _areaDummyGuid)
            {
                awaitingFinalDamage = true;
                source = event.source;
            }
            else if (awaitingFinalDamage && event.type == TestHarness::EventType::Damage
                && event.source == source && event.target == _areaDummyGuid)
                return event.amount;
        }
        return 0;
    }

    bool StartDeathAndDecay(TestHarness::Context& context, Player* actor,
        TertiaryState& state, bool empowered)
    {
        Creature* dummy = context.GetCreature(_areaDummyGuid);
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(49938);
        if (!dummy || !spellInfo)
            return false;

        _areaPeriodicEffect = nullptr;
        _areaOwnerGuid.Clear();
        actor->RemoveDynObject(spellInfo->Id);
        if (empowered)
            CastTertiarySpell(actor, state, actor, SPELL_OPPORTUNITY, 0, 0, 0,
                _settings.opportunityDurationMs);
        context.ClearEvents();
        dummy->SetHealth(dummy->GetMaxHealth());

        Spell spell(actor, spellInfo, TRIGGERED_NONE);
        SpellCastTargets targets;
        targets.SetDst(dummy->GetPositionX(), dummy->GetPositionY(),
            dummy->GetPositionZ(), dummy->GetOrientation());
        spell.InitExplicitTargets(targets);
        spell.cast(true);
        if (Aura* aura = spell.GetCreatedAura())
        {
            if (aura->GetOwner())
                _areaOwnerGuid = aura->GetOwner()->GetGUID();
            for (uint8 index = 0; index < MAX_SPELL_EFFECTS; ++index)
                if (IsTempoPeriodicEffect(spellInfo, aura->GetEffect(index)))
                {
                    _areaPeriodicEffect = aura->GetEffect(index);
                    break;
                }
        }
        return actor->GetDynObject(spellInfo->Id) != nullptr
            && !_areaOwnerGuid.IsEmpty() && _areaPeriodicEffect;
    }

    void RestoreShadowCrit(Player* actor)
    {
        if (!_shadowCritOverridden)
            return;

        uint16 shadowCritField = uint16(PLAYER_SPELL_CRIT_PERCENTAGE1) + SPELL_SCHOOL_SHADOW;
        actor->SetFloatValue(shadowCritField, _savedShadowCrit);
        _shadowCritOverridden = false;
    }

    void FinishFeatureTests(TestHarness::Context& context, Player* actor, TertiaryState* state)
    {
        constexpr uint8 TEMPO_OPPORTUNITY_MASK =
            PowerBit(Power::Tempo) | PowerBit(Power::Opportunity);
        CastPassive(actor, SPELL_AVOIDANCE_PASSIVE, -10);
        CastPassive(actor, SPELL_FLEETFOOT_GROUND_PASSIVE, 10, 10, 10);
        CastTertiarySpell(actor, *state, actor, SPELL_TEMPO, 10, 10, 10,
            _settings.tempoDurationMs);
        CastTertiarySpell(actor, *state, actor, SPELL_ECHO_AURA, 10, 0, 0,
            _settings.echoDurationMs);
        CastTertiarySpell(actor, *state, actor, SPELL_TEMPO_COOLDOWN, 0, 0, 0,
            _settings.tempoDurationMs);
        state->pendingUnbroken = true;
        state->echoActive = true;
        TestSettings(actor).enabled = false;
        state->needsRefresh = true;
        sScriptMgr->OnPlayerUpdate(actor, 0);
        context.Expect(state->disabled && HasExactPoints(*state, 0)
                && !state->pendingUnbroken && !state->tempoPendingCast
                && !state->echoActive && state->periodicAreas.empty()
                && !actor->HasAura(SPELL_AVOIDANCE_PASSIVE)
                && !actor->HasAura(SPELL_FLEETFOOT_GROUND_PASSIVE)
                && !actor->HasAura(SPELL_TEMPO)
                && !actor->HasAura(SPELL_TEMPO_COOLDOWN)
                && !actor->HasAura(SPELL_ECHO_AURA),
            "live disable removes tertiary effects and resets proc state");
        TestSettings(actor).enabled = true;
        state->needsRefresh = true;
        sScriptMgr->OnPlayerUpdate(actor, 0);
        context.Expect(!state->disabled && HasExactPoints(*state, TEMPO_OPPORTUNITY_MASK),
            "live re-enable refreshes equipped tertiary bonuses");
        ClearTestSettings(actor);

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        actor->SaveInventoryAndGoldToDB(transaction);
        CharacterDatabase.CommitTransaction(transaction);

        _expectedBonusId = TertiaryBonusOf(_item);
        _stage = Stage::AwaitPersistence;
        _elapsed = 0;
        _pollElapsed = 0;
    }

    TertiaryState* RefreshForMask(Player* actor, uint8 mask)
    {
        uint16 bonusId = EncodeBonusId(mask, _pointBudget);
        _item->SetBonusSeed(MakeItemBonusSeed(bonusId));
        TertiaryState* state = GetState(actor);
        state->needsRefresh = true;
        return EnsureState(actor);
    }

    bool HasExactPoints(TertiaryState const& state, uint8 mask) const
    {
        for (uint8 index = 0; index < POWER_COUNT; ++index)
            if (state.rawPoints[index] != ((mask & (1u << index)) ? _pointBudget : 0))
                return false;
        return true;
    }

    Stage _stage = Stage::Done;
    Item* _item = nullptr;
    ObjectGuid _itemGuid;
    ObjectGuid _reforgeSourceGuid;
    ObjectGuid _dummyGuid;
    uint16 _pointBudget = 0;
    uint32 _elapsed = 0;
    uint32 _pollElapsed = 0;
    uint16 _expectedBonusId = 0;
    int32 _nativeProperty = 0;
    ObjectGuid _areaDummyGuid;
    ObjectGuid _areaOwnerGuid;
    AuraEffect* _areaPeriodicEffect = nullptr;
    int64 _baselineAreaDamage = 0;
    float _savedShadowCrit = 0.0f;
    bool _shadowCritOverridden = false;
    uint32 _nativeSuffixFactor = 0;
};

} // namespace

void AddSC_tertiary_stats()
{
    Fabled::RegisterScripts();
    Fabled::RegisterTests();

    TestHarness::RegisterSuite("tertiary-stats",
        [] { return std::make_unique<TertiaryStatsTestSuite>(); });
    TestHarness::RegisterSuiteGroup("tertiary-all", {
        "tertiary-stats",
        "fabled-impact",
        "fabled-premonition",
        "fabled-momentum",
        "fabled-ghostwalk",
        "fabled-cavalier",
        "fabled-keeper",
        "fabled-vengeful-ghost",
        "fabled-crossfire",
        "fabled-alchemists-gut",
        "fabled-leviathans-gift",
        "fabled-overkill",
        "fabled-blood-magic",
        "fabled-warcaster",
        "fabled-indomitable"
    });
    RegisterSpellScript(spell_tertiary_unbroken_daze);
    Fabled::RegisterVengefulGhostSpellScripts();
    Fabled::RegisterBloodMagicSpellScripts();
    new TertiaryStatsWorld();
    new TertiaryStatsItem();
    new TertiaryStatsPlayer();
    new npc_fabled_reforger();
    new TertiaryStatsUnit();
    new TertiaryStatsSpell();
}

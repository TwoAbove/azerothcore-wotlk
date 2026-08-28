/*
 * mod-tertiary-stats: persistent per-character Fabled memories.
 */
#include "fabled.h"
#include "item_bonus_seed.h"

#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "test_harness.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "Util.h"

#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <string>

namespace Fabled
{
namespace
{
constexpr char ATTUNEMENT_KEY[] = "mod-tertiary-fabled-attunements";

struct AttunementState final : DataMap::Base
{
    uint16 unlocked = 0;
    std::array<Effect, EQUIPMENT_SLOT_END> slots{};
    bool loaded = false;
};

AttunementState& StateFor(Player* player)
{
    return *player->CustomData.GetDefault<AttunementState>(ATTUNEMENT_KEY);
}

void Assign(AttunementState& state, Effect effect, uint8 slot)
{
    for (Effect& assigned : state.slots)
        if (assigned == effect)
            assigned = Effect::None;

    if (slot < state.slots.size())
    {
        state.slots[slot] = effect;
        state.unlocked |= EffectBit(effect);
    }
}

void Persist(Player* player, AttunementState& state, Effect effect, uint8 slot)
{
    uint32 guid = player->GetGUID().GetCounter();
    CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
    transaction->Append(
        "UPDATE `mod_tertiary_fabled` SET `slot`=NULL WHERE `guid`={} AND `slot`={}",
        guid, uint32(slot));
    transaction->Append(
        "INSERT INTO `mod_tertiary_fabled` (`guid`, `effect`, `slot`) VALUES ({}, {}, {}) "
        "ON DUPLICATE KEY UPDATE `slot`=VALUES(`slot`)",
        guid, uint32(effect), uint32(slot));
    CharacterDatabase.CommitTransaction(transaction);
    Assign(state, effect, slot);
}
} // namespace

void LoadAttunements(Player* player)
{
    if (!player)
        return;

    AttunementState& state = StateFor(player);
    if (state.loaded)
        return;

    state.unlocked = 0;
    state.slots = {};
    state.loaded = true;

    QueryResult result = CharacterDatabase.Query(
        "SELECT `effect`, `slot` FROM `mod_tertiary_fabled` WHERE `guid`={}",
        player->GetGUID().GetCounter());
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint8 effectIndex = fields[0].Get<uint8>();
        if (effectIndex < 1 || effectIndex > EFFECT_COUNT)
        {
            LOG_ERROR("module", "TertiaryStats: character {} has invalid Fabled effect {}",
                player->GetGUID().GetCounter(), uint32(effectIndex));
            continue;
        }

        Effect effect = Effect(effectIndex);
        state.unlocked |= EffectBit(effect);
        if (fields[1].IsNull())
            continue;

        uint8 slot = fields[1].Get<uint8>();
        if (slot >= EQUIPMENT_SLOT_END || AnchorForEquipmentSlot(slot) != AnchorFor(effect))
        {
            LOG_ERROR("module", "TertiaryStats: character {} has invalid {} attunement slot {}",
                player->GetGUID().GetCounter(), Name(effect), uint32(slot));
            continue;
        }
        state.slots[slot] = effect;
    } while (result->NextRow());
}

uint16 UnlockedEffects(Player* player)
{
    LoadAttunements(player);
    return player ? StateFor(player).unlocked : 0;
}

bool IsUnlocked(Player* player, Effect effect)
{
    return effect != Effect::None && effect < Effect::Max
        && (UnlockedEffects(player) & EffectBit(effect));
}

Effect AttunedEffect(Player* player, uint8 slot)
{
    LoadAttunements(player);
    if (!player || slot >= EQUIPMENT_SLOT_END)
        return Effect::None;
    return StateFor(player).slots[slot];
}

bool SetAttunement(Player* player, Effect effect, uint8 slot, std::string& message)
{
    if (!player || effect == Effect::None || effect >= Effect::Max
        || slot >= EQUIPMENT_SLOT_END || AnchorForEquipmentSlot(slot) != AnchorFor(effect))
    {
        message = "That Fabled effect cannot be attuned to the selected slot.";
        return false;
    }
    if (player->IsInCombat())
    {
        message = "Fabled attunements cannot be changed in combat.";
        return false;
    }
    if (!IsUnlocked(player, effect))
    {
        message = "That Fabled memory has not been learned.";
        return false;
    }

    AttunementState& state = StateFor(player);
    Persist(player, state, effect, slot);
    message = std::string(Name(effect)) + " is now attuned to "
        + AnchorName(AnchorForEquipmentSlot(slot)) + ".";
    return true;
}

bool ClearAttunement(Player* player, uint8 slot, std::string& message)
{
    if (!player || slot >= EQUIPMENT_SLOT_END)
    {
        message = "That equipment slot is invalid.";
        return false;
    }
    if (player->IsInCombat())
    {
        message = "Fabled attunements cannot be changed in combat.";
        return false;
    }

    LoadAttunements(player);
    AttunementState& state = StateFor(player);
    Effect effect = state.slots[slot];
    if (effect == Effect::None)
    {
        message = "That slot has no Fabled attunement.";
        return false;
    }

    CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
    transaction->Append(
        "UPDATE `mod_tertiary_fabled` SET `slot`=NULL WHERE `guid`={} AND `effect`={}",
        player->GetGUID().GetCounter(), uint32(effect));
    CharacterDatabase.CommitTransaction(transaction);
    state.slots[slot] = Effect::None;
    message = std::string(Name(effect)) + " is no longer attuned.";
    return true;
}

bool LearnFromEquippedItem(Player* player, Item* item, uint8 slot, std::string& message)
{
    Effect effect = item ? EffectFromItemBonusSeed(item->GetBonusSeed()) : Effect::None;
    if (!player || !item || effect == Effect::None || slot >= EQUIPMENT_SLOT_END)
    {
        message = "That item does not carry a Fabled memory.";
        return false;
    }

    LoadAttunements(player);
    AttunementState& state = StateFor(player);
    if (state.unlocked & EffectBit(effect))
    {
        message.clear();
        return false;
    }

    bool compatible = AnchorForEquipmentSlot(slot) == AnchorFor(effect)
        && CanAttuneTo(effect, item->GetTemplate());
    if (!compatible)
    {
        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        transaction->Append(
            "INSERT IGNORE INTO `mod_tertiary_fabled` (`guid`, `effect`, `slot`) "
            "VALUES ({}, {}, NULL)",
            player->GetGUID().GetCounter(), uint32(effect));
        CharacterDatabase.CommitTransaction(transaction);
        state.unlocked |= EffectBit(effect);
        message = std::string("Learned ") + Name(effect) + ". Choose a "
            + AnchorName(AnchorFor(effect)) + " slot in the Fabled panel to attune it.";
        return true;
    }

    Persist(player, state, effect, slot);
    message = std::string("Learned and attuned ") + Name(effect) + ".";
    return true;
}

void RegisterAttunementSnapshotParticipant()
{
    bool registered = TestHarness::RegisterSnapshotParticipant(
        "tertiary-fabled",
        [](ObjectGuid::LowType guid, std::string& payload)
        {
            payload.clear();
            QueryResult result = CharacterDatabase.Query(
                "SELECT `effect`, `slot` FROM `mod_tertiary_fabled` "
                "WHERE `guid`={} ORDER BY `effect`",
                guid);
            if (!result)
                return true;

            do
            {
                Field* fields = result->Fetch();
                if (!payload.empty())
                    payload += ';';
                payload += Acore::StringFormat("{}:", uint32(fields[0].Get<uint8>()));
                payload += fields[1].IsNull()
                    ? "n" : std::to_string(uint32(fields[1].Get<uint8>()));
            } while (result->NextRow());
            return true;
        },
        [](ObjectGuid::LowType guid, std::string_view payload)
            -> std::optional<TestHarness::SnapshotRestorePlan>
        {
            using SnapshotRow = std::pair<uint8, std::optional<uint8>>;
            std::vector<SnapshotRow> rows;
            if (!payload.empty())
            {
                for (std::string_view token : Acore::Tokenize(payload, ';', false))
                {
                    std::size_t separator = token.find(':');
                    if (separator == std::string_view::npos)
                        return std::nullopt;

                    Optional<uint32> effect = Acore::StringTo<uint32>(
                        token.substr(0, separator));
                    if (!effect || *effect > std::numeric_limits<uint8>::max())
                        return std::nullopt;

                    std::string_view slotToken = token.substr(separator + 1);
                    std::optional<uint8> slot;
                    if (slotToken != "n")
                    {
                        Optional<uint32> parsed = Acore::StringTo<uint32>(slotToken);
                        if (!parsed || *parsed > std::numeric_limits<uint8>::max())
                            return std::nullopt;
                        slot = uint8(*parsed);
                    }
                    rows.emplace_back(uint8(*effect), slot);
                }
            }

            return TestHarness::SnapshotRestorePlan{
                [guid, rows = std::move(rows)](
                    CharacterDatabaseTransaction const& transaction)
                {
                    transaction->Append(
                        "DELETE FROM `mod_tertiary_fabled` WHERE `guid`={}", guid);
                    for (auto const& [effect, slot] : rows)
                    {
                        if (slot)
                            transaction->Append(
                                "INSERT INTO `mod_tertiary_fabled` "
                                "(`guid`, `effect`, `slot`) VALUES ({}, {}, {})",
                                guid, uint32(effect), uint32(*slot));
                        else
                            transaction->Append(
                                "INSERT INTO `mod_tertiary_fabled` "
                                "(`guid`, `effect`, `slot`) VALUES ({}, {}, NULL)",
                                guid, uint32(effect));
                    }
                    return true;
                },
                {}
            };
        });
    if (!registered)
        LOG_FATAL("module", "TertiaryStats: failed to register Fabled snapshot participant");
}
} // namespace Fabled

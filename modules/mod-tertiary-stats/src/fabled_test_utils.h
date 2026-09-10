/*
 * mod-tertiary-stats: shared helpers for fabled test-harness suites.
 * Header-only; include from fabled_<name>.cpp suites.
 */
#ifndef MOD_TERTIARY_STATS_FABLED_TEST_UTILS_H
#define MOD_TERTIARY_STATS_FABLED_TEST_UTILS_H

#include "fabled.h"
#include "item_bonus_seed.h"

#include "Item.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "test_harness.h"

#include <string>


namespace Fabled::Test
{
struct Equipment
{
    uint32 itemEntry;
    uint8 slot;
};

inline Equipment EquipmentFor(Effect effect)
{
    switch (AnchorFor(effect))
    {
        case Anchor::Head:
            return { 9849, EQUIPMENT_SLOT_HEAD }; // Conjurer's Hood
        case Anchor::Chest:
            return { 14136, EQUIPMENT_SLOT_CHEST }; // Robe of Winter Night
        case Anchor::Legs:
            return { 2958, EQUIPMENT_SLOT_LEGS }; // Journeyman's Pants
        case Anchor::Feet:
            return { 2309, EQUIPMENT_SLOT_FEET }; // Embossed Leather Boots
        case Anchor::Back:
            return { 1355, EQUIPMENT_SLOT_BACK }; // Buckskin Cape
        case Anchor::Finger:
            return { 12010, EQUIPMENT_SLOT_FINGER1 }; // Fen Ring
        case Anchor::Trinket:
            return { 40684, EQUIPMENT_SLOT_TRINKET1 }; // Mirror of Truth
        case Anchor::Weapon:
            return { 25, EQUIPMENT_SLOT_MAINHAND }; // Worn Shortsword
        default:
            return {};
    }
}

inline Item* EquipFabled(TestHarness::Context& context, Player* player, Effect effect)
{
    Equipment equipment = EquipmentFor(effect);
    bool ready = player && player->IsInWorld() && player->IsAlive()
        && !player->IsInCombat() && IsReady() && equipment.itemEntry;
    context.Expect(ready, "Fabled fixture requires a live, in-world, out-of-combat actor and ready catalog");
    if (!ready)
        return nullptr;

    bool alreadyLearned = IsUnlocked(player, effect);

    player->DestroyItem(INVENTORY_SLOT_BAG_0, equipment.slot, true);
    Item* item = Item::CreateItem(equipment.itemEntry, 1, player, false, 0, false,
        MakeItemBonusSeed(0, 0, uint8(effect)));
    if (!item)
        return nullptr;

    uint16 destination = (INVENTORY_SLOT_BAG_0 << 8) | equipment.slot;
    item = player->EquipItem(destination, item, true);
    if (!item)
        return nullptr;

    // First discovery must learn and auto-attune through Player::EquipItem's
    // registered hook. Only a previously learned memory needs a client request.
    bool learned = IsUnlocked(player, effect);
    context.Expect(learned, "equipping the memory learns its Fabled effect", Name(effect));
    if (!learned)
        return nullptr;
    if (alreadyLearned && AttunedEffect(player, equipment.slot) != effect)
    {
        std::string request = "TStats\tV3:A:" + std::to_string(uint8(effect))
            + ":" + std::to_string(equipment.slot + 1);
        sScriptMgr->OnPlayerCanUseChat(player, CHAT_MSG_WHISPER, LANG_ADDON, request, player);
    }
    sScriptMgr->OnPlayerUpdate(player, 0);
    bool active = player->GetItemByPos(INVENTORY_SLOT_BAG_0, equipment.slot) == item
        && AttunedEffect(player, equipment.slot) == effect && Has(GetRuntime(player), effect);
    context.Expect(active, "equipped Fabled memory is attuned and active through registered hooks", Name(effect));
    if (!active)
        return nullptr;
    return item;
}

inline void UnequipFabled(TestHarness::Context& context, Player* player, Effect effect)
{
    Equipment equipment = EquipmentFor(effect);
    if (!player || !equipment.itemEntry)
        return;

    bool learned = IsUnlocked(player, effect);
    // Teardown ends combat before the same out-of-combat clear request a client
    // uses. DestroyItem alone does not dispatch OnPlayerUnequip.
    player->CombatStop(true);
    player->DestroyItem(INVENTORY_SLOT_BAG_0, equipment.slot, true);
    if (AttunedEffect(player, equipment.slot) != Effect::None)
    {
        std::string request = "TStats\tV3:A:0:" + std::to_string(equipment.slot + 1);
        sScriptMgr->OnPlayerCanUseChat(player, CHAT_MSG_WHISPER, LANG_ADDON, request, player);
    }
    sScriptMgr->OnPlayerUpdate(player, 0);
    context.Expect(!player->GetItemByPos(INVENTORY_SLOT_BAG_0, equipment.slot)
            && AttunedEffect(player, equipment.slot) == Effect::None
            && !Has(GetRuntime(player), effect),
        "removing and clearing Fabled equipment deactivates it through registered hooks", Name(effect));
    context.Expect(IsUnlocked(player, effect) == learned,
        "clearing a Fabled attunement preserves its learned memory", Name(effect));
}

// Mark the actor as moving for the server-side movement checks
// (Spell::prepare/update consult Unit::isMoving() via movement flags).
inline void SetMoving(Player* player, bool moving)
{
    if (moving)
        player->AddUnitMovementFlag(MOVEMENTFLAG_FORWARD);
    else
        player->RemoveUnitMovementFlag(MOVEMENTFLAG_FORWARD);
}

// Relocate the actor by a delta; combined with SetMoving this exercises
// the movement-cancel paths deterministically.
inline void Nudge(Player* player, float dx, float dy)
{
    player->UpdatePosition(player->GetPositionX() + dx, player->GetPositionY() + dy,
        player->GetPositionZ(), player->GetOrientation(), true);
}
}

#endif

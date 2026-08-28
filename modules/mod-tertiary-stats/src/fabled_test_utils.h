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

inline Item* EquipFabled(Player* player, Effect effect)
{
    Equipment equipment = EquipmentFor(effect);
    if (!player || !equipment.itemEntry)
        return nullptr;

    player->DestroyItem(INVENTORY_SLOT_BAG_0, equipment.slot, true);
    Item* item = Item::CreateItem(equipment.itemEntry, 1, player, false, 0, false,
        MakeItemBonusSeed(0, 0, uint8(effect)));
    if (!item)
        return nullptr;

    uint16 destination = (INVENTORY_SLOT_BAG_0 << 8) | equipment.slot;
    item = player->EquipItem(destination, item, true);
    if (!item)
        return nullptr;

    std::string message;
    LearnFromEquippedItem(player, item, equipment.slot, message);
    if (AttunedEffect(player, equipment.slot) != effect
        && !SetAttunement(player, effect, equipment.slot, message))
        return nullptr;

    Runtime& runtime = GetRuntime(player);
    SetMask(player, uint16(runtime.mask | EffectBit(effect)));
    return item;
}

inline void UnequipFabled(Player* player, Effect effect)
{
    Equipment equipment = EquipmentFor(effect);
    if (!player || !equipment.itemEntry)
        return;

    player->DestroyItem(INVENTORY_SLOT_BAG_0, equipment.slot, true);
    Runtime& runtime = GetRuntime(player);
    SetMask(player, uint16(runtime.mask & ~EffectBit(effect)));
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

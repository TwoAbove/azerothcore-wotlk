/*
 * mod-tertiary-stats: shared helpers for fabled test-harness suites.
 * Header-only; include from fabled_<name>.cpp suites.
 */
#ifndef MOD_TERTIARY_STATS_FABLED_TEST_UTILS_H
#define MOD_TERTIARY_STATS_FABLED_TEST_UTILS_H

#include "fabled.h"

#include "Item.h"
#include "Player.h"
#include "test_harness.h"

namespace Fabled::Test
{
// Equip a bare trinket carrying the given fabled effect in trinket slot 1.
// Returns nullptr on failure. Caller should destroy it in cleanup via
// player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1, true).
inline Item* EquipFabledTrinket(Player* player, Effect effect, uint32 itemEntry = 40684)
{
    player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1, true);
    Item* item = Item::CreateItem(
        itemEntry, 1, player, false, 0, false, MakeItemBonusSeed(BonusId(effect)));
    if (!item)
        return nullptr;

    uint16 destination = (INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_TRINKET1;
    player->EquipItem(destination, item, true);
    Runtime& runtime = GetRuntime(player);
    SetMask(player, uint16(runtime.mask | EffectBit(effect)));
    return item;
}

inline void UnequipFabled(Player* player, Effect effect)
{
    player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_TRINKET1, true);
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

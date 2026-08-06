#ifndef MOD_TERTIARY_STATS_ITEM_BONUS_SEED_H
#define MOD_TERTIARY_STATS_ITEM_BONUS_SEED_H

#include "Define.h"

constexpr uint32 ITEM_BONUS_SEED_VERSION = 1;
constexpr uint32 ITEM_BONUS_SEED_VERSION_SHIFT = 24;
constexpr uint32 ITEM_BONUS_ID_MASK = 0xFFFF;

constexpr uint32 MakeItemBonusSeed(uint16 bonusId)
{
    return (ITEM_BONUS_SEED_VERSION << ITEM_BONUS_SEED_VERSION_SHIFT) | bonusId;
}

#endif

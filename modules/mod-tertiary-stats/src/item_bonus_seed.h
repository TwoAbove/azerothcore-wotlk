#ifndef MOD_TERTIARY_STATS_ITEM_BONUS_SEED_H
#define MOD_TERTIARY_STATS_ITEM_BONUS_SEED_H

#include "Define.h"
// [31:24] version, [23:20] Fabled effect, [18:16] heirloom profile,
// [15:0] ordinary bonus. Bit 19 remains reserved.

constexpr uint32 ITEM_BONUS_SEED_VERSION = 1;
constexpr uint32 ITEM_BONUS_SEED_VERSION_SHIFT = 24;
constexpr uint32 ITEM_BONUS_ID_MASK = 0xFFFF;
constexpr uint32 ITEM_BONUS_HEIRLOOM_PROFILE_SHIFT = 16;
constexpr uint32 ITEM_BONUS_HEIRLOOM_PROFILE_MASK = 0x7;
constexpr uint32 ITEM_BONUS_FABLED_EFFECT_SHIFT = 20;
constexpr uint32 ITEM_BONUS_FABLED_EFFECT_MASK = 0xF;

constexpr uint32 MakeItemBonusSeed(uint16 bonusId, uint8 heirloomProfile = 0,
    uint8 fabledEffect = 0)
{
    return (ITEM_BONUS_SEED_VERSION << ITEM_BONUS_SEED_VERSION_SHIFT)
        | (uint32(fabledEffect & ITEM_BONUS_FABLED_EFFECT_MASK)
            << ITEM_BONUS_FABLED_EFFECT_SHIFT)
        | (uint32(heirloomProfile & ITEM_BONUS_HEIRLOOM_PROFILE_MASK)
            << ITEM_BONUS_HEIRLOOM_PROFILE_SHIFT)
        | bonusId;
}

constexpr uint8 ItemBonusFabledEffect(uint32 seed)
{
    return uint8((seed >> ITEM_BONUS_FABLED_EFFECT_SHIFT)
        & ITEM_BONUS_FABLED_EFFECT_MASK);
}


#endif

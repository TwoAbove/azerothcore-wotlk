#ifndef AZEROTHCORE_AUCTION_HOUSE_RESULT_H
#define AZEROTHCORE_AUCTION_HOUSE_RESULT_H

#include "ObjectGuid.h"
#include <span>
#include <vector>

struct AuctionListResultItem
{
    uint32 auctionId;
    ObjectGuid itemGuid;
    uint32 itemEntry;
    uint32 itemBonusSeed;
};

using AuctionListResult = std::vector<AuctionListResultItem>;
using AuctionListResultView = std::span<AuctionListResultItem const>;

#endif

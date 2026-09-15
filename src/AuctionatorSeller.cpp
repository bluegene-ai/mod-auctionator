#include "Auctionator.h"
#include "AuctionHouseMgr.h"
#include "AuctionatorSeller.h"
#include "Item.h"
#include "DatabaseEnv.h"
#include "PreparedStatement.h"
#include <random>
#include <algorithm>
#include <cctype>
#include <limits>
#include "QueryResult.h"


AuctionatorSeller::AuctionatorSeller(Auctionator* natorParam, uint32 auctionHouseIdParam)
{
    SetLogPrefix("[AuctionatorSeller] ");
    nator = natorParam;
    auctionHouseId = auctionHouseIdParam;

    ahMgr = nator->GetAuctionMgr(auctionHouseId);
};

AuctionatorSeller::~AuctionatorSeller()
{
    // TODO: clean up
};

static bool IsValidDatabaseIdentifier(const std::string& name)
{
    if (name.empty()) {
        return false;
    }

    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_';
    });
}

void AuctionatorSeller::LetsGetToIt(uint32 maxCount, uint32 houseId)
{
    if (!nator || !nator->config) {
        logError("AuctionatorSeller::LetsGetToIt called without a valid Auctionator instance.");
        return;
    }

    if (maxCount == 0) {
        return;
    }

    std::string characterDbName = CharacterDatabase.GetConnectionInfo()->database;
    if (!IsValidDatabaseIdentifier(characterDbName)) {
        logError("Invalid character database name for Auctionator seller SQL: " + characterDbName);
        return;
    }

    std::vector<CachedItem> cachedItems;

    std::string cacheQuery = R"(
        SELECT
            it.entry, it.name, it.BuyPrice, it.stackable, it.quality
            , COALESCE(mp.average_price, 0) as average_price, aicconf.max_count
        FROM
            mod_auctionator_itemclass_config aicconf
            INNER JOIN item_template it ON
                aicconf.class = it.class
                AND aicconf.subclass = it.subclass
                AND it.bonding != 1
                AND (it.bonding >= aicconf.bonding OR it.bonding = 0)
                AND it.VerifiedBuild != 1
            LEFT JOIN mod_auctionator_disabled_items dis ON it.entry = dis.item
            LEFT JOIN (
                SELECT mp1.entry, mp1.average_price
                FROM {}.mod_auctionator_market_price mp1
                INNER JOIN (
                    SELECT entry, MAX(scan_datetime) as max_scan
                    FROM {}.mod_auctionator_market_price
                    GROUP BY entry
                ) mp2 ON mp1.entry = mp2.entry AND mp1.scan_datetime = mp2.max_scan
            ) mp ON it.entry = mp.entry
        WHERE dis.item IS NULL
    )";

    QueryResult result = CharacterDatabase.Query(cacheQuery, characterDbName, characterDbName);

    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            CachedItem item;
            item.entry = fields[0].Get<uint32>();
            item.name = fields[1].Get<std::string>();
            item.basePrice = fields[2].Get<uint32>();
            item.stackable = fields[3].Get<uint32>();
            item.quality = fields[4].Get<uint32>();
            item.marketPrice = fields[5].Get<uint32>();
            item.maxCount = fields[6].Get<uint32>();
            cachedItems.push_back(item);
        } while (result->NextRow());
    }


    std::string countQuery = R"(
        SELECT ii.itemEntry, COUNT(*) as itemCount
        FROM {}.item_instance ii
        INNER JOIN {}.auctionhouse ah ON ii.guid = ah.itemguid
        WHERE ah.houseId = {}
        GROUP BY ii.itemEntry
    )";

    QueryResult countResult = CharacterDatabase.Query(countQuery, characterDbName, characterDbName, houseId);

    std::unordered_map<uint32, uint32> currentCounts;
    if (countResult)
    {
        do
        {
            Field* fields = countResult->Fetch();
            currentCounts[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
        } while (countResult->NextRow());
    }

    std::vector<CachedItem> shuffled = cachedItems;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(shuffled.begin(), shuffled.end(), gen);

    uint32 count = 0;
    for (const auto& item : shuffled)
    {
        uint32 currentCount = currentCounts[item.entry];
        if (currentCount >= item.maxCount) continue;

        std::string itemName = item.name;

        uint32 stackSize = std::max<uint32>(1, item.stackable);
        if (stackSize > 20) {
            stackSize = 20;
        }

        if (stackSize > 1 && nator->config->sellerConfig.randomizeStackSize) {
            stackSize = GetRandomNumber(1, stackSize);
            logDebug("Stack size: " + std::to_string(stackSize));
        }

        const float bidStartModifier = std::clamp(nator->config->sellerConfig.bidStartModifier, 0.0f, 1.0f);

        float qualityMultiplier = Auctionator::GetQualityMultiplier(nator->config->sellerMultipliers, item.quality);

        uint32 price = item.marketPrice > 0 ? item.marketPrice : item.basePrice;
        if (item.marketPrice > 0) {
            logDebug("Using Market over Template [" + itemName + "] " +
                std::to_string(item.marketPrice) + " <--> " + std::to_string(item.basePrice));
        }

        if (price == 0) {
            const double defaultPrice = 10000000.0 * qualityMultiplier;
            price = static_cast<uint32>(std::min(defaultPrice, static_cast<double>(std::numeric_limits<uint32>::max())));
        }

        uint32 bidPrice = price;
        logDebug("Bid start modifier: " + std::to_string(bidStartModifier));

        const uint32 bidLowerBound = static_cast<uint32>(std::max(0.0, static_cast<double>(bidPrice) * (1.0 - bidStartModifier)));
        const uint32 bidUpperBound = bidPrice;
        uint32 safeBidLower = std::min(bidLowerBound, bidUpperBound);
        uint32 safeBidUpper = std::max(bidLowerBound, bidUpperBound);
        bidPrice = GetRandomNumber(safeBidLower, safeBidUpper);
        logDebug("Bid price " + std::to_string(bidPrice) + " from price " + std::to_string(price));

        AuctionatorItem newItem = AuctionatorItem();
        newItem.itemId = item.entry;
        newItem.quantity = 1;
        newItem.houseId = houseId;

        const uint64 stackTotalPrice = static_cast<uint64>(price) * static_cast<uint64>(stackSize);
        const long double buyoutMultiplier = std::max<long double>(1.0L, static_cast<long double>(qualityMultiplier));
        const long double bidMultiplier = std::max<long double>(1.0L, static_cast<long double>(qualityMultiplier));
        const uint64 buyoutValue = static_cast<uint64>(std::llround(static_cast<long double>(stackTotalPrice) * buyoutMultiplier));
        const uint64 bidValue = static_cast<uint64>(std::llround(static_cast<long double>(bidPrice) * static_cast<long double>(stackSize) * bidMultiplier));
        newItem.buyout = static_cast<uint32>(std::min<uint64>(buyoutValue, std::numeric_limits<uint32>::max()));
        newItem.bid = static_cast<uint32>(std::min<uint64>(bidValue, std::numeric_limits<uint32>::max()));
        if (newItem.buyout == 0) {
            newItem.buyout = std::max<uint32>(1, static_cast<uint32>(stackTotalPrice));
        }
        if (newItem.bid == 0) {
            newItem.bid = std::max<uint32>(1, static_cast<uint32>(std::max<uint64>(1ULL, static_cast<uint64>(bidPrice) * static_cast<uint64>(stackSize))));
        }
        newItem.time = 60 * 60 * 12;
        newItem.stackSize = stackSize;

        logDebug("Adding item: " + itemName
            + " with quantity of " + std::to_string(newItem.quantity)
            + " at price of " +  std::to_string(newItem.buyout)
            + " to house " + std::to_string(houseId)
        );

        if (newItem.buyout > 0 || newItem.bid > 0) {
            nator->CreateAuction(newItem);
        }

        count++;
        if (count >= maxCount) {
            break;
        }
    }

    logInfo("Items added houseId("
        + std::to_string(houseId)
        + ") this run: " + std::to_string(count));

};

uint32 AuctionatorSeller::GetRandomNumber(uint32 min, uint32 max)
{
    if (min > max) {
        std::swap(min, max);
    }

    if (max == min) {
        return max;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(min, max);
    return dis(gen);
}

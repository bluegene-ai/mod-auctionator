
#include "AuctionatorSales.h"
#include "Auctionator.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"

#include <string>

namespace
{
    std::string const SaleTableName = "mod_auctionator_sale";

    // The module only ever records a sale it can attribute to one of its own listings, so
    // this is the whole workload of the hook: one INSERT per sold module listing.
    char const* const InsertSaleSql =
        "INSERT INTO mod_auctionator_sale "
        "(auction_id, item_entry, item_count, house_id, seller_guid, seller_is_bot, buyer_guid, "
        "price, startbid, buyout, deposit, cut, is_buyout) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})";
}

AuctionatorSales::AuctionatorSales()
{
    SetLogPrefix("[AuctionatorSales] ");
}

bool AuctionatorSales::TableIsReady()
{
    //
    // Probed once per process and cached: the answer cannot change while the worldserver is
    // running (applying the SQL update is a restart anyway), and the hook that calls this
    // runs on every single sale.
    //
    static bool checked = false;
    static bool ready = false;

    if (checked)
    {
        return ready;
    }

    checked = true;

    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES "
        "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", SaleTableName);

    if (!result)
    {
        logError("cannot inspect " + SaleTableName + " (is the characters database reachable?)");
        return false;
    }

    ready = result->Fetch()[0].Get<uint64>() > 0;

    if (!ready)
    {
        logError(SaleTableName + " is missing, so sales of module listings are NOT recorded. "
            "Apply data/sql/db-characters/updates/2026_09_27_00_sale_log.sql to the characters database.");
    }

    return ready;
}

bool AuctionatorSales::IsModuleListing(AuctionEntry const* auction) const
{
    // See the header: deposit = 0 is how every module-created listing is recognizable.
    if (auction->deposit == 0)
    {
        return true;
    }

    Auctionator* auctionator = Auctionator::getInstance();
    if (!auctionator->config)
    {
        return false;
    }

    return auction->owner.GetCounter() == auctionator->config->characterGuid;
}

void AuctionatorSales::RecordSale(AuctionEntry const* auction)
{
    if (!auction || !IsModuleListing(auction) || !TableIsReady())
    {
        return;
    }

    Auctionator* auctionator = Auctionator::getInstance();
    uint32 const botGuid = auctionator->config ? auctionator->config->characterGuid : 0;
    uint32 const ownerGuid = auction->owner.GetCounter();

    //
    // A buyout sets bid = buyout before settling (WorldSession::HandleAuctionPlaceBid), so
    // "price >= buyout with a buyout set" is a buyout and anything below is a winning bid.
    // A listing without a buyout can only have been won by bidding.
    //
    uint32 const isBuyout = (auction->buyout > 0 && auction->bid >= auction->buyout) ? 1 : 0;

    CharacterDatabase.Execute(InsertSaleSql,
        auction->Id,
        auction->item_template,
        auction->itemCount,
        (uint32)auction->houseId,
        ownerGuid,
        (botGuid != 0 && ownerGuid == botGuid) ? 1 : 0,
        auction->bidder.GetCounter(),
        auction->bid,
        auction->startbid,
        auction->buyout,
        auction->deposit,
        auction->GetAuctionCut(),
        isBuyout);

    logDebug("recorded sale of auction " + std::to_string(auction->Id)
        + ": item " + std::to_string(auction->item_template)
        + " x" + std::to_string(auction->itemCount)
        + " to " + std::to_string(auction->bidder.GetCounter())
        + " for " + std::to_string(auction->bid) + " copper"
        + (isBuyout ? " (buyout)" : " (bid)"));
}

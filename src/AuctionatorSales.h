
#ifndef AUCTIONATOR_SALES_H
#define AUCTIONATOR_SALES_H

#include "AuctionatorBase.h"
#include "AuctionHouseMgr.h"

//
// Sale log of the listings this module created, in the characters database:
//
//   mod_auctionator_sale(
//       id, auction_id, item_entry, item_count, house_id, seller_guid, seller_is_bot,
//       buyer_guid, price, startbid, buyout, deposit, cut, is_buyout, sold_at)
//
// The core keeps no record of a finished auction: `characters.auctionhouse` holds the
// live auctions only (its row is deleted together with the settlement), and the core's
// own `log_money` row is written for sales of 500 gold and up only. A module listing
// therefore became untraceable the moment a player won it, at every price below that -
// which matters here, because the sale gold is swallowed by the mail script (gold sink)
// and the sale mail itself is deleted instead of sent.
//
// Rows are written from the module's "auction successful" hook, i.e. for both ways a
// listing can end up sold: a buyout (WorldSession::HandleAuctionPlaceBid) and a winning
// bid at expiry (AuctionHouseObject::Update).
//
class AuctionatorSales : public AuctionatorBase
{
    public:
        AuctionatorSales();

        // Records one sale of a module-created listing and ignores everything else
        // (player auctions belong to the core's own accounting, not to this module).
        void RecordSale(AuctionEntry const* auction);

    private:
        // The table arrives with data/sql/db-characters/updates/2026_09_27_00_sale_log.sql.
        // A realm that has not applied it yet gets one warning instead of a failed INSERT
        // on every sale.
        bool TableIsReady();

        // Was this auction created by the module (as opposed to a player listing)?
        //
        // Every auction the module creates carries deposit = 0 - it never charges one,
        // because the core pays out "bid + deposit - cut" and a deposit would mint gold
        // (see the README economy section) - while the core charges player listings at
        // least AH_MINIMUM_DEPOSIT. The configured auctionator's guid is checked as well,
        // so a realm whose Rate.Auction.Deposit is 0 still records its bot sales.
        bool IsModuleListing(AuctionEntry const* auction) const;
};

#endif //AUCTIONATOR_SALES_H

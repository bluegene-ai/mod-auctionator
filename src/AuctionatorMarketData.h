
#ifndef AUCTIONATOR_MARKET_DATA_H
#define AUCTIONATOR_MARKET_DATA_H

#include "AuctionatorBase.h"
#include <cstdint>
#include <string>

//
// Maintenance of the market price table in the characters database:
//
//   mod_auctionator_market_price(
//       entry, scan_datetime, average_price, buyout, bid, `count`, source, imported_at)
//   PRIMARY KEY (entry, scan_datetime)
//
// The table is written by the CSV import (either the apps/marketprice script or,
// when Auctionator.MarketData.ImportFile is set, by this class on a timer) and read
// by the seller and the bidder on every cycle.
//
class AuctionatorMarketData : public AuctionatorBase
{
    public:
        struct ImportResult
        {
            bool ok = false;
            bool unchanged = false;      // file did not change since the last import
            bool truncated = false;      // stopped at Auctionator.MarketData.ImportMaxRows
            uint32 read = 0;
            uint32 imported = 0;
            uint32 invalid = 0;
            uint32 batches = 0;
            std::string path;
        };

        struct Stats
        {
            uint64 totalRows = 0;
            uint32 distinctItems = 0;
            uint32 freshItems = 0;
            std::string newestScan;
            std::string oldestScan;
        };

        AuctionatorMarketData();

        //
        // Imports a CSV export: header line optional, columns
        //   scan_datetime,item_entry,avg_price,minimum_buyout,minimum_bid,item_count
        // Rows are upserted on (entry, scan_datetime), so re-importing the same file
        // updates instead of failing and a partial export never touches other items.
        // With force = false an unchanged file (same size and mtime) is skipped.
        //
        ImportResult ImportFromFile(std::string const& path, std::string const& source,
            uint32 maxRows, bool force);

        // Deletes scans older than the given number of days (>= 1). Returns rows removed.
        uint32 PruneOlderThan(uint32 days);

        // Aggregate view of the table. maxAgeDays = 0 counts every item as fresh.
        bool GetStats(uint32 maxAgeDays, Stats& stats);

    private:
        // True when the table exists and has the columns this class needs.
        bool TableIsReady();
};

#endif

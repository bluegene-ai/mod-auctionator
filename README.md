# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) Azeroth Core Module

## mod-auctionator

[![core-build](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/core-build.yml/badge.svg)](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/core-build.yml)
[![codestyle](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/codestyle.yml/badge.svg)](https://github.com/bluegene-ai/mod-auctionator/actions/workflows/codestyle.yml)

> The badge URLs above point at this fork. If you push the module to another
> repository, replace `bluegene-ai/mod-auctionator` with your own `owner/repo`.

## Compatibility

This branch targets **[azerothcore/azerothcore-wotlk](https://github.com/azerothcore/azerothcore-wotlk) `master` only**.

* CI checks out upstream `master`, drops this module into `modules/` and builds it
  with the official AzerothCore reusable workflow
  (`azerothcore/reusable-workflows`) — NOPCH, clang, `-Wall -Wextra -Werror`,
  plus cppcheck over `modules/`. Green CI means "compiles and passes cppcheck
  against upstream master".
* Nothing else is supported: no core forks, no NPCBots or other patches, no
  release-branch backports. If upstream master changes an API the module uses,
  the build breaks on purpose and gets fixed here.
* Because CI builds **without precompiled headers**, every source file includes
  what it uses. Keep it that way when adding code.

<p align="left">
  <img src="https://github.com/araxiaonline/docs/blob/main/docs/media/logo-sm.png?raw=true" alt="Araxia Online" width="70" style="vertical-align: middle;"/>
  <span style="font-size: 20px; vertical-align: middle;" >Originally developed by Araxia Online</span>
</p>

## Description

This mod is meant to keep a healthy auction house stocked on a low-pop server. It's in it's early phases of building/testing/configuration but keeps a LOT of stuff in the AH. Code is also a little rough right now but I haven't done c++ in 20+ years so I am getting there.

## Installation and Setup

1. Download the code into your source code `modules/` folder.
2. Build the core with the module in place (`-DMODULES=static`), then deploy the new
   `worldserver` binaries.
3. Make sure the **server's `SourceDirectory` points at the source tree that contains
   `modules/mod-auctionator/data/sql/`** (leave it empty to use the compile-time source
   path). The AzerothCore updater applies the module's SQL from
   `<SourceDirectory>/modules/<module>/data/sql/`; if that path is not reachable the
   module's tables are never created, the seller finds no candidate items and the market
   import fails.
   If you prefer to do it by hand, run these against the matching database:
   * world database: `data/sql/db-world/base/2023-09-18.sql` and
     `data/sql/db-world/base/mod_auctionator_gm_list.sql`
   * characters database, in this order:
     `data/sql/db-characters/updates/2023_11_12_00_marketprice.sql`,
     `data/sql/db-characters/updates/2026_09_20_00_market_price_history.sql`,
     `data/sql/db-characters/updates/2026_01_29_00_improve_performance.sql`
   Verify with `SHOW TABLES LIKE 'mod_auctionator%';` in both databases.
4. **Create the module configuration.** The build ships
   `modules/mod-auctionator/conf/mod_auctionator.conf.dist`; the server only reads
   `configs/modules/mod_auctionator.conf` (no `.dist`), and Windows builds do not copy it
   for you:
   ```
   copy modules\mod-auctionator\conf\mod_auctionator.conf.dist configs\modules\mod_auctionator.conf
   ```
   Without that file every option falls back to its code default, and
   `Auctionator.Enabled` defaults to **0**, so the module stays silent.
5. Optionally add the module's logging lines to `worldserver.conf` (they do **not** work
   in the module config, which is loaded after the log system is initialised):
   ```
   Appender.AuctionatorLog=2,5,0,auctionator.log,w
   Appender.AuctionatorConsole=1,4,0,"1 9 3 6 5 8"
   Logger.auctionator=4,AuctionatorLog AuctionatorConsole
   ```
6. If you want a fresh start, clear the auction house with these SQL commands. Do this
   ONLY when your server is stopped. The AH is cached in memory and doing this while the
   server is running will not end well. Run this against your character database.

> The module itself never hardcodes a schema name: the seller and market queries
> resolve the world and characters databases at runtime from `worldserver.conf`
> (`WorldDatabaseInfo` / `CharacterDatabaseInfo`), so non-default names such as
> `acore_world80` / `acore_characters80` work out of the box.
>
> Because those queries span both schemas from one connection, the **world** database
> user needs `SELECT` on the characters schema (the default AzerothCore setup uses the
> same user for every database and already satisfies this). Without the grant the
> seller's item query fails with `Table ... doesn't exist` in the SQL log and the
> seller logs "seller item query produced no rows".


```
DELETE FROM `item_instance` WHERE `guid` IN (SELECT `itemguid` FROM `auctionhouse`);
DELETE FROM `auctionhouse`;
```

## GM commands

`.auctionator` on its own (or `.auctionator help`) prints the command list, and an
unknown subcommand answers with an explicit error instead of doing nothing.

### auctionator add <house> <item[,item...]> <price> [stack] [hours] [owner]

ItemID can be looked up in the database table `item_template`.

* `house` - `2` = alliance, `6` = horde, `7` = neutral
* `item` - one id, or several separated by commas (max 200 per command)
* `price` - **unit** price in copper; the buyout becomes `price * stack`
* `stack` - optional, default 1, capped by the item's max stack
* `hours` - optional, default 48, range 1..720
* `owner` - optional: `bot` (default), `me`, or a character guid

**Start bid.** A listing with a start bid of 0 can be taken for 1 copper: the core only
rejects a bid below `auction->startbid`, and 1 copper is never below 0. The command
therefore derives the start bid from the buyout with the same rule as the automatic
seller - `buyout * (1 - Auctionator.Seller.BidStartModifier)`, never below 1 - so with the
shipped `0.3` a 10000 copper listing starts bidding at 7000. Set
`Auctionator.Seller.BidStartModifier = 0` if GM listings should be buyout-only (the start
bid then equals the buyout, so a bid costs the same as buying it out).

**Gold handling.** With the default owner (`bot`, i.e. the configured
`Auctionator.CharacterGuid`) the sale money is mailed to the auctionator
character and recycled by the mail script: the gold leaves the economy. Pass
`me` or a character guid if you want the money to reach a real character
instead.

The item is created fresh from `item_template`, so any item can be listed,
including bind-on-pickup, quest and unique items. The command warns you when an
item is quest/unique, because those checks are bypassed.

Add an Iridescent Pearl to the Neutral AH for 1 gold buyout:

```
.auctionator add 7 5500 10000
```

List 20 Copper Bolts in the Horde AH at 5 silver each, for 12 hours, and let the
gold be recycled:

```
.auctionator add 6 4359 500 20 12
```

List a bind-on-pickup epic for yourself, 3 day duration, money to your character:

```
.auctionator add 7 19019 500000 1 72 me
```

### auctionator addlist [house] [owner]

Lists every enabled row of `mod_auctionator_gm_list` (world database) so a
curated set of special items can be restocked with one command. The optional
`house` and `owner` arguments override the per-row columns.

```
.auctionator addlist          # use each row's own house/owner
.auctionator addlist 7        # force the neutral house
.auctionator addlist 7 me     # neutral house, money goes to you
```

### auctionator auctionspercycle <value>

Set the number of auctions added per cycle to each auction house (shared by all
houses). The seller walks the whole candidate set and lists this many of them, so
there is no longer a separate query limit to keep in sync.

This has a direct impact on how hard each cycle hits your worldserver and your
mysql server, so on smaller hardware tune this down.

```
.auctionator auctionspercycle 45
```

### auctionator bidonown <value>

Allows the bidder to bid on the auctionator's own auctions. Only useful for
testing the buyer. Note that the bidder in general is a gold *faucet* (it pays
sellers without paying itself), see "Economy and safety guarantees" below.

Valid values are `1` to enable and `0` to disable.

```
.auctionator bidonown 1
```

### auctionator disable <target>

Disable the seller or bidder for a particular faction. `<target>` is one of
`hordeseller`, `allianceseller`, `neutralseller`, `hordebidder`,
`alliancebidder`, `neutralbidder` or `all`.

The change is applied to the running schedule immediately: no restart is needed
(only the master switch `Auctionator.Enabled` requires one).

```
.auctionator disable hordeseller
```

### auctionator enable <target>

Enable the seller or bidder for a particular faction (same targets as
`disable`). The first run of a newly enabled event happens about a minute later,
then the configured cycle time takes over.

```
.auctionator enable hordeseller
```

### auctionator expireall <house> [all]

Expire the auctionator's own auctions for the specified house on the next tick.
Player auctions are left alone unless you pass `all` explicitly (the core then
returns their items and refunds their bidders, nothing is destroyed). Listings that were
created with an explicit owner (`.auctionator add ... me` or a character guid) count as
somebody else's and need `all` as well.

```
.auctionator expireall 7
.auctionator expireall 7 all
```

### auctionator multiplier <typetext> <qualitytext> <multiplier>

Typetext is either `seller` or `bidder` depending on which you want to set.

Set the sell and buy multiplier for a quality. Qualities are:

* `poor`
* `normal`
* `uncommon`
* `rare`
* `epic`
* `legendary`

Multiplier is a decimal value. Defaults are set in the config file.

For the **seller** multiplier, note that it scales the *fallback* price only
(`item_template.BuyPrice`, or `Auctionator.Seller.DefaultPrice` when the item has
no vendor price either). An imported market price is never multiplied, otherwise
market driven pricing would not be market driven.

A **bidder** multiplier below `1.0` is honoured as configured and lowers what the bidder
is willing to pay (it is not silently raised to 1.0); `0.0` means the bidder buys nothing
and the only hard limit is a floor of 1 copper. The same applies to the seller multiplier
on the fallback price.

```
.auctionator multiplier bidder epic 10
```

### auctionator status

Shows the status of the in memory configs as well as the current auction
houses. Useful for checking config values that may have changed or checking
on auction counts. Example output below.

```
.auctionator status
```

Output:
```
[Auctionator] Status:

 Enabled: 1

 CharacterGuid: 2
 Horde:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 9319
    Bidder Enabled: 1
        Cycle Time: 2
        Per Cycle: 20
 Alliance:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 8959
    Bidder Enabled: 1
        Cycle Time: 1
        Per Cycle: 20
 Neutral:
    Seller Enabled: 1
        Max Auctions: 20000
        Auctions: 9379
    Bidder Enabled: 1
        Cycle Time: 3
        Per Cycle: 30
 Seller Multipliers:
    Poor: 1.000000
    Normal: 1.000000
    Uncommon: 1.500000
    Rare: 2.000000
    Epic: 6.000000
    Legendary: 10.000000
 Bidder Multipliers:
    Poor: 1.000000
    Normal: 1.000000
    Uncommon: 1.500000
    Rare: 2.000000
    Epic: 6.000000
    Legendary: 10.000000
 Seller settings:
    Auctions per run: 100
    Default Price (no vendor/market price): 1000000
    Randomize Stack Size: 1
    Bid Start Modifier: 0.300000
    Market data max age (days, seller+bidder, 0 = never): 14
    Prefer market items: 1
    Exclude VerifiedBuild = 1 items: 0
    Min price modifier (x SellPrice): 1.000000
    Max price modifier (x market avg): 2.000000
```

## Shared faction houses (AllowTwoSide.Interaction.Auction = 1)

With this core option the three auction houses are one object, the core stores every auction
with `houseId = Neutral` (`WorldSession::HandleAuctionSellItem` forces it) and clients only
ever search the neutral house (`AuctionHouseSearcher` picks its partition from
`AuctionEntry::GetFactionId()`, i.e. from `houseId`). An Alliance or Horde entry would
therefore be an auction that **no client can see or bid on**, while still occupying that
house's quota and inflating `.auctionator status`.

The module refuses to create such an entry at every level:

* `.auctionator add 2 ...` / `add 6 ...` answers with an error, and `addlist` skips rows
  whose `house` column (or the `house` override) is 2 or 6;
* `Auctionator::CreateAuction()` rejects it as a second line of defence, so no caller can
  create an invisible auction;
* the `AllianceSeller`/`HordeSeller` and `AllianceBidder`/`HordeBidder` events log one error
  per cycle and do nothing (every *player* auction lives in the neutral house, so a
  non-neutral bidder would not find any of them anyway).

On such a realm enable `Auctionator.NeutralSeller` and `Auctionator.NeutralBidder` only.
`.auctionator status` prints a reminder, and `.auctionator expireall 2|6` still works, which
is what you want for rows left over from before the option was switched on.

## Market data import

The seller prices from `mod_auctionator_market_price` (characters database). The
table now keeps history: the primary key is `(entry, scan_datetime)`, so imports
upsert per item and scan, a partial export never touches the items it does not
mention, and you can prune old scans when you want. A `source` column records
which feed a row came from.

### Option A - the server imports a CSV on a timer (no external tools)

```
Auctionator.MarketData.ImportFile = "C:/acore/marketprice.csv"
Auctionator.MarketData.ImportSource = tsm
Auctionator.MarketData.ImportIntervalMinutes = 360
```

The worldserver reads that file about one minute after startup and then every
interval. An unchanged file (same size and mtime) is skipped, so the interval can
be short. Columns, with an optional header line:

```
scan_datetime,item_entry,avg_price,minimum_buyout,minimum_bid,item_count
```

### Option B - the helper script (needs Node 18+ and the mysql client)

```
cd apps/marketprice

# only need to do this the first time
npm install

node index.js mypricedata.csv --source=tsm | mysql -u <dbuser> -p <character_database_name>
```

The generated SQL is idempotent (batched `INSERT ... ON DUPLICATE KEY UPDATE`
inside a transaction), so re-importing the same export updates instead of failing.
Progress goes to stderr, the SQL to stdout.

`minimum_buyout` and `minimum_bid` are not used; `item_count` (volume) is used by
the seller's market weighting.

### Operating it

```
.auctionator market              # rows, distinct items, how many items have a usable scan
.auctionator marketimport        # import Auctionator.MarketData.ImportFile now
.auctionator marketimport force  # import even if the file did not change
.auctionator marketprune 30      # drop scans older than 30 days
```

`marketimport` reports "nothing was imported" and logs an error when every line of the file
was rejected (wrong date format, non-numeric or zero item entry/price): a broken export must
not look like an empty one. An empty file is still a successful no-op.

A scan older than `Auctionator.MarketData.MaxAgeDays` is ignored by both the
seller (falls back to `item_template.BuyPrice`) and the bidder, so a broken import
degrades to vendor pricing instead of listing nonsense. Run `marketprune`
regularly (or from your own cron) to keep the history table small.

`scan_datetime` is interpreted in the **database server's time zone**: the age of a
scan is computed by the database itself (`TIMESTAMPDIFF(..., NOW())`), so the importer
and the readers only agree when the CSV carries local time of the database host. An
export in UTC on a `+08:00` host looks eight hours older than it is.

Note: the timed import only runs while the module is enabled
(`Auctionator.Enabled = 1`). Use `.auctionator marketimport` if you need to import
while it is disabled.

## Pricing in one picture

For every listed item the seller picks one of three sources, in this order:

| Source | Used when | Scaled by |
|---|---|---|
| `mod_auctionator_market_price.average_price` | a scan exists and is newer than `MarketData.MaxAgeDays` | **nothing** (imported as-is) |
| `item_template.BuyPrice` | no usable market data | the per quality `Multipliers.Seller.*` |
| `Auctionator.Seller.DefaultPrice` (default 100 gold) | no market data **and** no vendor price | the per quality `Multipliers.Seller.*` |

The result is then clamped into `[SellPrice x MinPriceModifier, marketPrice x
MaxPriceModifier]` (the ceiling only applies when the price came from the market) and
multiplied by the stack size, capped at `MAX_MONEY_AMOUNT`. The ceiling is applied
**before** the floor, so the floor always wins: even a market average far below the vendor
sell price cannot make the bot list below what a vendor pays (which a player could
otherwise buy up and vendor at a profit). The starting bid is a random value between
`(1 - BidStartModifier) x buyout` and the buyout, and is never 0 - a start bid of 0 would
let a player take the auction for 1 copper.

## Item selection

Which items may be listed, and in what shape, is data driven (world database):

| Table | Columns | Meaning |
|---|---|---|
| `mod_auctionator_itemclass_config` | `class`, `subclass`, `bonding`, `max_count`, `stack_count` | one row per item class/subclass |
| `mod_auctionator_disabled_items` | `item` | flat blacklist of `item_template.entry` |

* `bonding` - minimum `item_template.bonding` for the row to match; `0` means "no
  additional constraint". Items with `bonding = 1` (bind on pickup) are always excluded.
  The shipped rows only use 0 and 1, so the column currently acts as a flag for
  "unbound items are allowed / not allowed" in that class.
* `max_count` - quota per item entry: the seller skips an item while it already has
  `max_count` auctions in that house. **`max_count = 0` means "never list this
  class/subclass"** (it is a quota, not "unlimited").
* `stack_count` - how many items go into one listing (weapons/armour `1`, trade
  goods and potions `20`, elixirs `10`, ...). It is clamped by the item's own max
  stack; a row with `0` falls back to that max stack (the column is `NOT NULL DEFAULT 1`,
  so `NULL` cannot occur). With
  `Auctionator.Seller.RandomizeStackSize = 1` the listing uses a random value
  between 1 and `stack_count` instead of the full value.

`item_template.VerifiedBuild = 1` rows are **not** filtered out by default: in most
world databases that flag means "not verified by the content team", and skipping those
rows removes a large part of the eligible catalogue (~30% on a stock WotLK world
database). Set `Auctionator.Seller.ExcludeUnverifiedItems = 1` if your data means
something else by it.

Both tables are read on every seller cycle, so edits apply without a restart.

## Economy and safety guarantees

These are invariants of the module, not side effects:

1. **Gold from bot and GM listings is recycled by the system.** When a bot/GM
   listing sells, the core mails the proceeds to the auctionator character, and
   the mail script deletes that mail (and any items attached to it) before it is
   sent. The gold leaves the economy. Passing an explicit `owner` to
   `.auctionator add` is the only way to send the money to a character instead;
   the command reply labels such a listing `GOLD SINK BYPASSED`.
2. **No deposit is ever paid out.** The module never charges a deposit, so
   mod-created auctions are created with `deposit = 0`; otherwise the core's
   "bid + deposit - cut" payout would mint gold.
3. **Player auctions are never damaged.**
   * `.auctionator expireall <house>` only expires auctions owned by the
     auctionator character; player auctions are untouched unless you pass `all`.
   * The item that the bot buys out is sunk, but the seller is paid in the same
     transaction, and the previous bidder is refunded through the core's
     `SendAuctionOutbiddedMail` before the bid is taken over, so no escrowed
     player gold is ever destroyed.
   * The bidder never places a *bid* on an auction that already has one (it skips it),
     so a player's bid is never overwritten. It can, however, **buy out** an auction a
     player has already bid on: the previous bidder is refunded through the core's
     `SendAuctionOutbiddedMail` first (see 6 for what that refund means), and the
     seller is paid in the same transaction.
4. **Mail recycling stops automatically if a human is playing the configured
   auctionator character.** The mail hook checks whether the receiver is online
   and, if so, leaves the mail alone (with a warning in the log). A dedicated,
   never-logged-in auctionator character keeps full recycling.
5. **The bidder is disabled by default** (`Auctionator.*Bidder.Enabled = 0`)
   because it buys player auctions without paying for them: the core pays the
   seller with newly created gold, which inflates the economy. Enable it only if
   that is what you want.
6. **Every bot purchase is unfunded, and the refund path depends on mail
   recycling.** The module holds no money by design, so its bids and buyouts are
   recorded without any escrow while `SendAuctionSuccessfulMail()` still pays the
   seller: each purchase grows the money supply (and the item the bot wins is sunk).
   When a *player* outbids the bot, the core refunds the bot's bid - money that was
   never escrowed - and that refund stays harmless only because the mail recycling
   destroys it (see 4: the hook deliberately steps aside while the auctionator
   character is online, at which point such a refund becomes real minted gold).
   Every purchase is logged with an `UNFUNDED` prefix, and a warning is printed once
   at startup whenever a bidder is enabled, so the faucet is never silent.

## Current limitations

1. ~~Stacks are acting weird, especially things like enchanter rods.~~ Fixed. The
   listed stack comes from `mod_auctionator_itemclass_config.stack_count` (clamped by
   the item's own max stack); with `Auctionator.Seller.RandomizeStackSize = 1` it is a
   random value between 1 and that size.
2. ~~No control on stack size, it is hard coded to 20.~~ Stack size is data driven per
   class/subclass (`stack_count`, see "Item selection"), and GM listings take an
   explicit `stack` argument.
3. Item selection is driven by `mod_auctionator_itemclass_config` and
   `mod_auctionator_disabled_items`; there is still no in-game editor for them
   (edit the tables and wait for the next seller cycle).
4. The import is synchronous, so a very large CSV stalls the world thread while it
   is written. Keep `Auctionator.MarketData.ImportMaxRows` sane and prefer
   incremental exports.

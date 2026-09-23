
#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <vector>
#include "ScriptMgr.h"
#include "Chat.h"
#include "Auctionator.h"
#include "AuctionatorConfig.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "DatabaseEnv.h"
#include "CharacterCache.h"
#include "AuctionatorMarketData.h"
#include "Optional.h"

using namespace Acore::ChatCommands;

namespace
{
    // Limits for the GM listing commands.
    uint32 const MaxItemsPerCommand = 200;
    uint32 const DefaultListingHours = 48;    // matches AuctionatorItem's default (172800 s)
    uint32 const MinListingHours = 1;
    uint32 const MaxListingHours = 720;       // 30 days
    uint32 const MaxListingStack = 1000;
    // Upper bound for ".auctionator multiplier": the value is multiplied into item prices
    // and std::llround() is undefined for out of range results.
    float const MaxPriceMultiplier = 1000.0f;

    bool TryParseUInt32(const std::string& value, uint32& result)
    {
        try {
            size_t idx = 0;
            unsigned long long parsed = std::stoull(value, &idx, 10);
            if (idx != value.size() || parsed > std::numeric_limits<uint32>::max()) {
                return false;
            }
            result = static_cast<uint32>(parsed);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool TryParseFloat(const std::string& value, float& result)
    {
        try {
            size_t idx = 0;
            float parsed = std::stof(value, &idx);
            if (idx != value.size()) {
                return false;
            }
            result = parsed;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // Split "5500, 5501,5502" into its non-empty tokens (spaces are ignored).
    std::vector<std::string> SplitList(std::string const& value, char separator)
    {
        std::vector<std::string> parts;
        std::string current;

        for (char ch : value)
        {
            if (ch == separator)
            {
                if (!current.empty())
                {
                    parts.push_back(current);
                    current.clear();
                }
            }
            else if (!std::isspace(static_cast<unsigned char>(ch)))
            {
                current.push_back(ch);
            }
        }

        if (!current.empty())
        {
            parts.push_back(current);
        }

        return parts;
    }

    // Things a GM should know about before listing an item: the bot fabricates a
    // brand new item from item_template, so uniqueness and quest checks that the
    // normal player trade path performs do not apply.
    std::string DescribeItemNotes(ItemTemplate const* proto)
    {
        std::string notes;

        if (proto->Bonding == BIND_QUEST_ITEM || proto->Bonding == BIND_QUEST_ITEM1 || proto->StartQuest != 0)
        {
            notes += "quest item; ";
        }

        if (proto->Bonding == BIND_WHEN_PICKED_UP)
        {
            notes += "binds on pickup; ";
        }

        if (proto->MaxCount > 0)
        {
            notes += "unique (max " + std::to_string(proto->MaxCount) + "); ";
        }

        return notes;
    }
}

class AuctionatorCommands : public CommandScript
{
    public:
        AuctionatorCommands() : CommandScript("AuctionatorCommandScript")
        {
        }

    private:
        // Handlers receive the raw parameter vector and check its size before
        // indexing it. The previous "std::vector<const char*>" marshalling handed
        // out a pointer that could be nullptr for an empty parameter list, so any
        // missing argument turned into a null dereference inside the handler.
        static bool DispatchCommand(ChatHandler* handler, Auctionator* auctionator, const std::string& command, const std::vector<std::string>& commandParams)
        {
            if (command == "add")
            {
                auctionator->logInfo("Adding new item(s) for GM");
                return CommandAdd(commandParams, handler, auctionator);
            }

            if (command == "addlist")
            {
                auctionator->logInfo("Adding GM list items");
                return CommandAddList(commandParams, handler, auctionator);
            }

            if (command == "market")
            {
                return CommandMarketStatus(commandParams, handler, auctionator);
            }

            if (command == "marketimport")
            {
                return CommandMarketImport(commandParams, handler, auctionator);
            }

            if (command == "marketprune")
            {
                return CommandMarketPrune(commandParams, handler, auctionator);
            }

            if (command == "auctionspercycle")
            {
                return CommandAuctionsPerCycle(commandParams, handler, auctionator);
            }

            if (command == "bidonown")
            {
                return CommandBidOnOwn(commandParams, handler, auctionator);
            }

            if (command == "bidspercycle")
            {
                return CommandBidsPerCycle(commandParams, handler, auctionator);
            }

            if (command == "disable")
            {
                return CommandDisableSeller(commandParams, handler, auctionator);
            }

            if (command == "enable")
            {
                return CommandEnableSeller(commandParams, handler, auctionator);
            }

            if (command == "expireall")
            {
                return CommandExpireAll(commandParams, handler, auctionator);
            }

            if (command == "multiplier")
            {
                return CommandSetMultiplier(commandParams, handler, auctionator);
            }

            if (command == "status")
            {
                ShowStatus(handler, auctionator);
                return true;
            }

            if (command == "help")
            {
                ShowHelp(handler);
                return true;
            }

            // Report unknown subcommands instead of silently doing nothing.
            handler->SendSysMessage("[Auctionator] unknown command \"" + command + "\"; see \".auctionator help\".");
            return true;
        }

        static bool HandleCommandOptionsNew(ChatHandler* handler, Optional<std::vector<std::string>> const& args)
        {
            // The parameter is optional on purpose: Acore::ChatCommands only invokes a
            // handler once it consumed every declared argument, and a plain
            // std::vector<std::string> requires at least one token. Without the
            // Optional wrapper a bare ".auctionator" never reached this function at all,
            // so the help fallback below was dead code.
            if (!args || args->empty())
            {
                ShowHelp(handler);
                return true;
            }

            std::vector<std::string> const& values = *args;
            std::string const command = values[0].empty() ? std::string("help") : values[0];

            std::vector<std::string> commandParams;
            commandParams.reserve(values.size() - 1);
            for (size_t i = 1; i < values.size(); ++i)
            {
                commandParams.push_back(values[i]);
            }

            Auctionator* auctionator = Auctionator::getInstance();
            auctionator->logDebug("Executing command: " + command);
            return DispatchCommand(handler, auctionator, command, commandParams);
        }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable commandTableBase =
            {
                { "auctionator", HandleCommandOptionsNew, SEC_GAMEMASTER, Console::Yes }
            };

            return commandTableBase;
        }

        static bool IsKnownAuctionHouse(uint32 auctionHouseId)
        {
            return auctionHouseId == (uint32)AuctionHouseId::Alliance
                || auctionHouseId == (uint32)AuctionHouseId::Horde
                || auctionHouseId == (uint32)AuctionHouseId::Neutral;
        }

        // Human readable owner for the chat replies. ownerGuid == 0 means the
        // configured Auctionator character, whose auction mail is deleted by the
        // mail script: the sale money is removed from the economy (gold sink).
        static std::string DescribeOwner(uint32 ownerGuid)
        {
            if (ownerGuid == 0)
            {
                return "the auctionator character (gold is recycled by the system)";
            }

            std::string name = "unknown";
            sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(ownerGuid), name);
            return name + " (guid " + std::to_string(ownerGuid) + ", GOLD SINK BYPASSED)";
        }

        // Creates one listing. Returns true only when the auction really exists.
        static bool AddSingleListing(uint32 auctionHouseId, uint32 itemId, uint32 unitPrice, uint32 stackSize,
            uint32 hours, uint32 ownerGuid, ChatHandler* handler, Auctionator* auctionator, bool report)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
            if (!proto)
            {
                if (report)
                {
                    handler->SendSysMessage("[Auctionator] add: skipped item " + std::to_string(itemId)
                        + " (no item_template entry).");
                }
                return false;
            }

            uint32 const maxStack = std::max<uint32>(1, proto->GetMaxStackSize());
            uint32 const finalStack = std::min<uint32>(std::max<uint32>(1, stackSize), maxStack);

            std::string const notes = DescribeItemNotes(proto);
            if (!notes.empty())
            {
                // The chat note is GM feedback for ".auctionator add"; during
                // ".auctionator addlist" it would repeat once per row and flood the chat.
                if (report)
                {
                    handler->SendSysMessage("[Auctionator] note for " + proto->Name1 + " ["
                        + std::to_string(itemId) + "]: " + notes
                        + "the bot creates a fresh item, so uniqueness and quest checks are bypassed.");
                }
                auctionator->logWarn("add: restricted item " + proto->Name1 + " ["
                    + std::to_string(itemId) + "]: " + notes);
            }

            uint64 const totalPrice = static_cast<uint64>(unitPrice) * static_cast<uint64>(finalStack);
            if (report && totalPrice > MAX_MONEY_AMOUNT)
            {
                handler->SendSysMessage("[Auctionator] add: " + proto->Name1 + " [" + std::to_string(itemId)
                    + "] stack total exceeds the maximum money amount, capping the listing price at "
                    + std::to_string(MAX_MONEY_AMOUNT) + " copper.");
            }

            uint32 const cappedTotal = static_cast<uint32>(std::min<uint64>(totalPrice, MAX_MONEY_AMOUNT));

            AuctionatorItem newItem;
            newItem.houseId = auctionHouseId;
            newItem.itemId = itemId;

            if (auctionator->config->sellerConfig.bidOnly)
            {
                //
                // Pure auction (Auctionator.Seller.BidOnly = 1): no buyout, so the whole
                // price becomes the start bid. The GM still passes a *unit* price; it is
                // multiplied by the stack exactly like the buyout would have been.
                //
                newItem.buyout = 0;
                newItem.bid = cappedTotal;
            }
            else
            {
                newItem.buyout = cappedTotal;

                //
                // A start bid of 0 means "any bid wins" to the core: HandleAuctionPlaceBid only
                // rejects a bid below auction->startbid, so a 0 would let a player take the stack
                // (including a 500000 copper epic) for 1 copper when the auction expires. Use the
                // same rule as the automatic seller: buyout * (1 - BidStartModifier), never below 1.
                //
                float const bidStartModifier = std::clamp(auctionator->config->sellerConfig.bidStartModifier, 0.0f, 1.0f);
                uint64 const startBid = static_cast<uint64>(std::llround(
                    static_cast<double>(newItem.buyout) * (1.0 - static_cast<double>(bidStartModifier))));
                newItem.bid = static_cast<uint32>(std::min<uint64>(std::max<uint64>(1, startBid), newItem.buyout));
            }

            newItem.stackSize = finalStack;
            newItem.time = hours * 60 * 60;
            newItem.ownerGuid = ownerGuid;

            bool const created = auctionator->CreateAuction(newItem);

            if (report && created)
            {
                handler->SendSysMessage("[Auctionator] add: listed " + proto->Name1 + " [" + std::to_string(itemId) + "]"
                    + " x" + std::to_string(finalStack)
                    + " in house " + std::to_string(auctionHouseId)
                    + (newItem.buyout != 0
                        ? " at " + std::to_string(newItem.buyout) + " copper buyout"
                        : std::string(" as a bid-only listing (no buyout)"))
                    + ", start bid " + std::to_string(newItem.bid) + " copper"
                    + " for " + std::to_string(hours) + "h"
                    + " | owner: " + DescribeOwner(ownerGuid) + ".");
            }
            else if (report)
            {
                handler->SendSysMessage("[Auctionator] add: item " + std::to_string(itemId)
                    + " was NOT listed; see the server log for the reason.");
            }

            return created;
        }

        // Resolves the optional owner argument: "bot" (default), "me" or a character guid.
        static bool ResolveOwnerArgument(std::string const& value, ChatHandler* handler, uint32& ownerGuid)
        {
            if (value == "bot")
            {
                ownerGuid = 0;
                return true;
            }

            if (value == "me")
            {
                Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
                if (!player)
                {
                    handler->SendSysMessage("[Auctionator] add: \"me\" is only available for an in-game GM.");
                    return false;
                }

                ownerGuid = player->GetGUID().GetCounter();
                return true;
            }

            if (!TryParseUInt32(value, ownerGuid))
            {
                handler->SendSysMessage("[Auctionator] add: owner must be \"bot\", \"me\" or a character guid.");
                return false;
            }

            if (ownerGuid != 0
                && !sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(ownerGuid)))
            {
                handler->SendSysMessage("[Auctionator] add: there is no character with guid "
                    + std::to_string(ownerGuid) + ".");
                return false;
            }

            return true;
        }

        // .auctionator add <house> <item[,item...]> <price> [stack] [hours] [owner]
        //
        // <price> is the unit price; the buyout becomes price * stack.
        // No owner (or "bot") means the sale money is recycled by the system.
        static bool CommandAdd(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() < 3 || params.size() > 6)
            {
                handler->SendSysMessage("[Auctionator] add: usage <house> <item[,item...]> <price> [stack] [hours] [owner]");
                return true;
            }

            uint32 auctionHouseId = 0;
            uint32 unitPrice = 0;

            if (!TryParseUInt32(params[0], auctionHouseId))
            {
                handler->SendSysMessage("[Auctionator] add: house must be a number (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            if (!IsKnownAuctionHouse(auctionHouseId))
            {
                handler->SendSysMessage("[Auctionator] add: invalid auction house "
                    + std::to_string(auctionHouseId) + " (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            //
            // With AllowTwoSide.Interaction.Auction the three houses share one object and the
            // core stores every auction with houseId = Neutral; clients only ever search the
            // neutral partition, so an Alliance/Horde entry would be invisible to everybody.
            // CreateAuction() refuses it too - this check only replaces its log line with a
            // GM-readable reason.
            //
            if (auctionHouseId != (uint32)AuctionHouseId::Neutral && Auctionator::UsesSharedNeutralAuctionHouse())
            {
                handler->SendSysMessage("[Auctionator] add: AllowTwoSide.Interaction.Auction is enabled on this realm, "
                    "so all players share the neutral house; house " + std::to_string(auctionHouseId)
                    + " would create an auction nobody can see. Use house 7 (neutral).");
                return true;
            }

            if (!auctionator->GetAuctionHouse(auctionHouseId))
            {
                handler->SendSysMessage("[Auctionator] add: auction house "
                    + std::to_string(auctionHouseId) + " is not initialized; check Auctionator.CharacterId / Auctionator.CharacterGuid.");
                return true;
            }

            if (!TryParseUInt32(params[2], unitPrice) || unitPrice == 0 || unitPrice > MAX_MONEY_AMOUNT)
            {
                handler->SendSysMessage("[Auctionator] add: price must be between 1 and "
                    + std::to_string(MAX_MONEY_AMOUNT) + " copper per item.");
                return true;
            }

            std::vector<std::string> const itemTokens = SplitList(params[1], ',');
            if (itemTokens.empty())
            {
                handler->SendSysMessage("[Auctionator] add: no item id given.");
                return true;
            }

            if (itemTokens.size() > MaxItemsPerCommand)
            {
                handler->SendSysMessage("[Auctionator] add: at most " + std::to_string(MaxItemsPerCommand)
                    + " items per command (" + std::to_string(itemTokens.size()) + " given).");
                return true;
            }

            std::vector<uint32> itemIds;
            itemIds.reserve(itemTokens.size());
            for (std::string const& token : itemTokens)
            {
                uint32 itemId = 0;
                if (!TryParseUInt32(token, itemId) || itemId == 0)
                {
                    handler->SendSysMessage("[Auctionator] add: \"" + token + "\" is not a valid item id; nothing was listed.");
                    return true;
                }

                itemIds.push_back(itemId);
            }

            uint32 stackSize = 1;
            if (params.size() >= 4 && !TryParseUInt32(params[3], stackSize))
            {
                handler->SendSysMessage("[Auctionator] add: stack must be a number.");
                return true;
            }

            stackSize = std::clamp<uint32>(stackSize, 1, MaxListingStack);

            uint32 hours = DefaultListingHours;
            if (params.size() >= 5 && !TryParseUInt32(params[4], hours))
            {
                handler->SendSysMessage("[Auctionator] add: hours must be a number.");
                return true;
            }

            if (hours < MinListingHours || hours > MaxListingHours)
            {
                handler->SendSysMessage("[Auctionator] add: hours must be between "
                    + std::to_string(MinListingHours) + " and " + std::to_string(MaxListingHours) + ".");
                return true;
            }

            uint32 ownerGuid = 0;
            if (params.size() >= 6 && !ResolveOwnerArgument(params[5], handler, ownerGuid))
            {
                return true;
            }

            uint32 listed = 0;
            for (uint32 itemId : itemIds)
            {
                if (AddSingleListing(auctionHouseId, itemId, unitPrice, stackSize, hours, ownerGuid, handler, auctionator, true))
                {
                    listed++;
                }
            }

            if (itemIds.size() > 1 || listed != itemIds.size())
            {
                handler->SendSysMessage("[Auctionator] add: listed " + std::to_string(listed)
                    + " of " + std::to_string(itemIds.size()) + " item(s).");
            }

            return true;
        }

        // .auctionator addlist [house] [owner]
        //
        // Lists every enabled row of mod_auctionator_gm_list (world database), so a
        // curated set of special items can be (re)stocked with one command.
        static bool CommandAddList(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() > 2)
            {
                handler->SendSysMessage("[Auctionator] addlist: usage [house] [owner]");
                return true;
            }

            uint32 overrideHouse = 0;
            if (!params.empty() && !TryParseUInt32(params[0], overrideHouse))
            {
                handler->SendSysMessage("[Auctionator] addlist: house must be a number (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            if (!params.empty() && !IsKnownAuctionHouse(overrideHouse))
            {
                handler->SendSysMessage("[Auctionator] addlist: invalid auction house "
                    + std::to_string(overrideHouse) + " (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            uint32 ownerGuid = 0;
            if (params.size() >= 2 && !ResolveOwnerArgument(params[1], handler, ownerGuid))
            {
                return true;
            }

            QueryResult result = WorldDatabase.Query(R"(
                SELECT item, price, stack, hours, house, owner
                FROM mod_auctionator_gm_list
                WHERE enabled = 1
                ORDER BY house, item
            )");

            if (!result)
            {
                handler->SendSysMessage("[Auctionator] addlist: no enabled rows in mod_auctionator_gm_list (world database).");
                return true;
            }

            uint32 processed = 0;
            uint32 listed = 0;
            uint32 skipped = 0;

            do
            {
                Field* fields = result->Fetch();

                uint32 const itemId = fields[0].Get<uint32>();
                uint32 const unitPrice = fields[1].Get<uint32>();
                uint32 const stackSize = fields[2].Get<uint32>();
                uint32 const hours = fields[3].Get<uint32>();
                uint32 const rowHouse = fields[4].Get<uint32>();
                uint32 const rowOwner = fields[5].Get<uint32>();

                processed++;

                uint32 const house = overrideHouse != 0 ? overrideHouse : rowHouse;
                uint32 const owner = params.size() >= 2 ? ownerGuid : rowOwner;

                if (!IsKnownAuctionHouse(house))
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (invalid house " + std::to_string(house) + ").");
                    skipped++;
                    continue;
                }

                // See CommandAdd: on a shared-house realm only the neutral house is visible.
                if (house != (uint32)AuctionHouseId::Neutral && Auctionator::UsesSharedNeutralAuctionHouse())
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (house " + std::to_string(house) + ", but AllowTwoSide.Interaction.Auction is enabled: "
                          "only house 7 exists for players).");
                    skipped++;
                    continue;
                }

                if (!auctionator->GetAuctionHouse(house))
                {
                    handler->SendSysMessage("[Auctionator] addlist: auction house " + std::to_string(house)
                        + " is not initialized; check Auctionator.CharacterId / Auctionator.CharacterGuid.");
                    skipped++;
                    continue;
                }

                if (unitPrice == 0 || unitPrice > MAX_MONEY_AMOUNT)
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (price must be between 1 and " + std::to_string(MAX_MONEY_AMOUNT) + " copper).");
                    skipped++;
                    continue;
                }

                if (hours < MinListingHours || hours > MaxListingHours)
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (hours must be between " + std::to_string(MinListingHours)
                        + " and " + std::to_string(MaxListingHours) + ").");
                    skipped++;
                    continue;
                }

                if (owner != 0
                    && !sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(owner)))
                {
                    handler->SendSysMessage("[Auctionator] addlist: skipping item " + std::to_string(itemId)
                        + " (there is no character with guid " + std::to_string(owner) + ").");
                    skipped++;
                    continue;
                }

                if (AddSingleListing(house, itemId, unitPrice, stackSize, hours, owner, handler, auctionator, false))
                {
                    listed++;
                }
                else
                {
                    skipped++;
                }
            } while (result->NextRow());

            std::string const ownerNote = params.size() >= 2
                ? "owner override: " + DescribeOwner(ownerGuid)
                : "owner from each row (0 = gold recycled by the system)";

            handler->SendSysMessage("[Auctionator] addlist: listed " + std::to_string(listed)
                + " of " + std::to_string(processed) + " row(s), skipped " + std::to_string(skipped)
                + " | " + ownerNote + ".");

            return true;
        }

        // .auctionator market
        static bool CommandMarketStatus(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params.empty())
            {
                handler->SendSysMessage("[Auctionator] market: usage (no arguments)");
                return true;
            }

            AuctionatorConfig* config = auctionator->config;

            std::string message = "[Auctionator] Market data:\n";
            message += " Config: max age " + std::to_string(config->marketDataMaxAgeDays) + " day(s)"
                + ", retention " + std::to_string(config->marketDataRetentionDays) + " day(s)\n";
            message += " Config: import file: "
                + (config->marketDataImportFile.empty() ? std::string("(disabled)") : config->marketDataImportFile) + "\n";

            if (!config->marketDataImportFile.empty())
            {
                message += " Config: every " + std::to_string(config->marketDataImportIntervalMinutes) + " minute(s)"
                    + ", source '" + config->marketDataImportSource + "'"
                    + ", max " + std::to_string(config->marketDataImportMaxRows) + " row(s) per import\n";
            }

            AuctionatorMarketData marketData;
            AuctionatorMarketData::Stats stats;

            if (!marketData.GetStats(config->marketDataMaxAgeDays, stats))
            {
                message += " Table: not readable, see the server log.";
                handler->SendSysMessage(message);
                return true;
            }

            message += " Table: " + std::to_string(stats.totalRows) + " row(s)"
                + ", " + std::to_string(stats.distinctItems) + " distinct item(s)\n";
            message += " Items with a usable scan: " + std::to_string(stats.freshItems)
                + " (age limit " + std::to_string(config->marketDataMaxAgeDays) + " day(s), 0 = no limit)\n";
            message += " Newest scan: " + stats.newestScan + " | oldest scan: " + stats.oldestScan;

            handler->SendSysMessage(message);
            return true;
        }

        // .auctionator marketimport [force]
        static bool CommandMarketImport(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            bool force = false;
            if (!params.empty())
            {
                if (params.size() > 1 || params[0] != "force")
                {
                    handler->SendSysMessage("[Auctionator] marketimport: usage [force]");
                    return true;
                }

                force = true;
            }

            std::string const file = auctionator->config->marketDataImportFile;
            if (file.empty())
            {
                handler->SendSysMessage("[Auctionator] marketimport: no Auctionator.MarketData.ImportFile configured.");
                return true;
            }

            AuctionatorMarketData marketData;
            AuctionatorMarketData::ImportResult const result = marketData.ImportFromFile(
                file,
                auctionator->config->marketDataImportSource,
                auctionator->config->marketDataImportMaxRows,
                force
            );

            if (!result.ok)
            {
                handler->SendSysMessage("[Auctionator] marketimport: nothing was imported ("
                    + std::to_string(result.read) + " line(s) read, "
                    + std::to_string(result.invalid) + " rejected); see the server log and \".auctionator market\".");
                return true;
            }

            if (result.unchanged)
            {
                handler->SendSysMessage("[Auctionator] marketimport: " + file
                    + " is unchanged since the last import (use \"marketimport force\" to import anyway).");
                return true;
            }

            handler->SendSysMessage("[Auctionator] marketimport: wrote " + std::to_string(result.imported)
                + " row(s), skipped " + std::to_string(result.invalid)
                + " line(s) of " + std::to_string(result.read)
                + (result.truncated ? " (TRUNCATED at Auctionator.MarketData.ImportMaxRows)" : "")
                + ". Check \".auctionator market\".");

            return true;
        }

        // .auctionator marketprune [days]
        static bool CommandMarketPrune(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            uint32 days = auctionator->config->marketDataRetentionDays;

            if (!params.empty())
            {
                if (params.size() > 1 || !TryParseUInt32(params[0], days))
                {
                    handler->SendSysMessage("[Auctionator] marketprune: usage [days]");
                    return true;
                }
            }

            if (days < 1)
            {
                handler->SendSysMessage("[Auctionator] marketprune: the retention must be at least 1 day.");
                return true;
            }

            AuctionatorMarketData marketData;
            uint32 const removed = marketData.PruneOlderThan(days);

            handler->SendSysMessage("[Auctionator] marketprune: removed " + std::to_string(removed)
                + " row(s) older than " + std::to_string(days) + " day(s).");

            return true;
        }

        static void ShowHelp(ChatHandler* handler)
        {
            std::string helpString(R"(
Auctionator Help:
add <house> <item[,item...]> <price> [stack] [hours] [owner]
     house: 2 = alliance, 6 = horde, 7 = neutral
            (with AllowTwoSide.Interaction.Auction only house 7 is visible to
             players; houses 2 and 6 are refused instead of creating an
             auction nobody can see)
     price: unit price in copper (buyout = price * stack)
     start bid: buyout * (1 - Auctionator.Seller.BidStartModifier), at least 1
            (with Auctionator.Seller.BidOnly = 1 there is no buyout at all: the
             listing can only be won by bidding and "price * stack" is the start bid)
     stack: default 1, capped by the item's max stack
     hours: default 48, range 1..720
     owner: "bot" (default) recycles the sale money, "me" or a character guid
addlist [house] [owner]
     lists every enabled row of mod_auctionator_gm_list
market
     market price table: rows, distinct items, usable scans, import config
marketimport [force]
     import the CSV configured in Auctionator.MarketData.ImportFile now
marketprune [days]
     delete market scans older than the retention (default from the config)
auctionspercycle <value>
     set how many auctions each seller run may add (shared by all houses)
bidspercycle <value>
     set how many auctions each bidder run may buy (all three houses)
bidonown <0|1>
     allow the bidder to bid on the auctionator's own auctions (testing)
disable <hordeseller|allianceseller|neutralseller|hordebidder|alliancebidder|neutralbidder|all>
enable <same targets as disable>
expireall <house> [all]
     without "all" only the auctionator's own auctions are expired; listings
     created with an explicit owner need "all"
multiplier <seller|bidder> <poor|normal|uncommon|rare|epic|legendary> <value>
status
help
            )");
            handler->SendSysMessage(helpString);
        }

        static std::string DescribeHouseAuctionCount(Auctionator* auctionator, AuctionHouseId houseId)
        {
            // CountAuctions() counts by AuctionEntry::houseId, which is the only correct
            // answer while CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION makes all houses
            // share one object.
            if (!auctionator->GetAuctionHouse((uint32)houseId))
            {
                return "n/a (auction houses not initialized)";
            }

            return std::to_string(auctionator->CountAuctions((uint32)houseId));
        }

        static void ShowStatus(ChatHandler* handler, Auctionator* auctionator)
        {
            std::string statusString = "[Auctionator] Status:\n\n";

            statusString += " Enabled: " + std::to_string(auctionator->config->isEnabled) + "\n\n";
            statusString += " Bid on Own: " + std::to_string(auctionator->config->bidOnOwn) + "\n";
            statusString += " CharacterGuid: " + std::to_string(auctionator->config->characterGuid) + "\n";

            if (Auctionator::UsesSharedNeutralAuctionHouse())
            {
                statusString += " NOTE: AllowTwoSide.Interaction.Auction is enabled, so the three houses share one\n";
                statusString += "       object and only the neutral house (7) is visible to players. Houses 2 and 6\n";
                statusString += "       are refused by the add/addlist commands and their seller/bidder events do\n";
                statusString += "       nothing but log an error; use Auctionator.NeutralSeller / NeutralBidder.\n";
            }

            statusString += " Horde:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->hordeSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->hordeSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + DescribeHouseAuctionCount(auctionator, AuctionHouseId::Horde) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->hordeBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->hordeBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->hordeBidder.maxPerCycle) + "\n";

            statusString += " Alliance:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->allianceSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->allianceSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + DescribeHouseAuctionCount(auctionator, AuctionHouseId::Alliance) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->allianceBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->allianceBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->allianceBidder.maxPerCycle) + "\n";

            statusString += " Neutral:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->neutralSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->neutralSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + DescribeHouseAuctionCount(auctionator, AuctionHouseId::Neutral) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->neutralBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->neutralBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->neutralBidder.maxPerCycle) + "\n";

            statusString += " Seller Multipliers:\n";
            statusString += "    Poor: " + std::to_string(auctionator->config->sellerMultipliers.poor) + "\n";
            statusString += "    Normal: " + std::to_string(auctionator->config->sellerMultipliers.normal) + "\n";
            statusString += "    Uncommon: " + std::to_string(auctionator->config->sellerMultipliers.uncommon) + "\n";
            statusString += "    Rare: " + std::to_string(auctionator->config->sellerMultipliers.rare) + "\n";
            statusString += "    Epic: " + std::to_string(auctionator->config->sellerMultipliers.epic) + "\n";
            statusString += "    Legendary: " + std::to_string(auctionator->config->sellerMultipliers.legendary) + "\n";

            statusString += " Bidder Multipliers:\n";
            statusString += "    Poor: " + std::to_string(auctionator->config->bidderMultipliers.poor) + "\n";
            statusString += "    Normal: " + std::to_string(auctionator->config->bidderMultipliers.normal) + "\n";
            statusString += "    Uncommon: " + std::to_string(auctionator->config->bidderMultipliers.uncommon) + "\n";
            statusString += "    Rare: " + std::to_string(auctionator->config->bidderMultipliers.rare) + "\n";
            statusString += "    Epic: " + std::to_string(auctionator->config->bidderMultipliers.epic) + "\n";
            statusString += "    Legendary: " + std::to_string(auctionator->config->bidderMultipliers.legendary) + "\n";

            statusString += " Seller settings:\n";
            statusString += "    Auctions per run: " + std::to_string(auctionator->config->sellerConfig.auctionsPerRun) + "\n";
            statusString += "    Default Price (no vendor/market price): " + std::to_string(auctionator->config->sellerConfig.defaultPrice) + "\n";
            statusString += "    Randomize Stack Size: " + std::to_string(auctionator->config->sellerConfig.randomizeStackSize) + "\n";
            statusString += "    Bid Start Modifier: " + std::to_string(auctionator->config->sellerConfig.bidStartModifier) + "\n";
            statusString += "    Market data max age (days, seller+bidder, 0 = never): " + std::to_string(auctionator->config->marketDataMaxAgeDays) + "\n";
            statusString += "    Prefer market items: " + std::to_string(auctionator->config->sellerConfig.preferMarketItems) + "\n";
            statusString += "    Exclude VerifiedBuild = 1 items: " + std::to_string(auctionator->config->sellerConfig.excludeUnverifiedItems) + "\n";
            statusString += "    Min price modifier (x SellPrice): " + std::to_string(auctionator->config->sellerConfig.minPriceModifier) + "\n";
            statusString += "    Max price modifier (x market avg): " + std::to_string(auctionator->config->sellerConfig.maxPriceModifier) + "\n";

            handler->SendSysMessage(statusString);
        }

        // .auctionator expireall <house> [all]
        //
        // Without "all" only the auctionator's own auctions are expired, so a real
        // player's auction is never cancelled by this command.
        static bool CommandExpireAll(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] expireall: No Auction House Specified! [2, 6, 7]");
                auctionator->logInfo("expireall: No Auction House Specified!");
                return true;
            }

            if (params.size() > 2) {
                handler->SendSysMessage("[Auctionator] expireall: usage <house> [all]");
                return true;
            }

            uint32 houseId = 0;
            if (!TryParseUInt32(params[0], houseId)) {
                handler->SendSysMessage("[Auctionator] expireall: House id must be a number.");
                return true;
            }

            // Validate here as well: ExpireAllAuctions() only rejects an unknown house in
            // the debug log, so without this check the GM saw the "warning" and the "Done"
            // lines for a house that was never touched.
            if (!IsKnownAuctionHouse(houseId))
            {
                handler->SendSysMessage("[Auctionator] expireall: invalid auction house "
                    + std::to_string(houseId) + " (2 = alliance, 6 = horde, 7 = neutral).");
                return true;
            }

            bool includePlayerAuctions = false;
            if (params.size() >= 2)
            {
                if (params[1] != "all")
                {
                    handler->SendSysMessage("[Auctionator] expireall: usage <house> [all]");
                    return true;
                }

                includePlayerAuctions = true;
            }

            if (includePlayerAuctions)
            {
                handler->SendSysMessage("[Auctionator] expireall: WARNING - \"all\" also cancels player auctions in house "
                    + std::to_string(houseId) + " (their items and bids are returned by the core, nothing is destroyed).");
            }
            else
            {
                handler->SendSysMessage("[Auctionator] Expiring auctionator auctions for house: "
                    + std::to_string(houseId)
                    + " (listings created with an explicit owner via \".auctionator add ... <owner>\" are not"
                      " touched by this; pass \"all\" to include them).");
            }

            auctionator->logInfo("expireall: Expiring auctions for house: "
                + std::to_string(houseId)
                + (includePlayerAuctions ? " (including player auctions)" : " (auctionator owned only)"));

            auctionator->ExpireAllAuctions(houseId, includePlayerAuctions);

            auctionator->logInfo("expireall: Done for house: " + std::to_string(houseId));

            return true;
        }

        static void WarnIfModuleDisabled(ChatHandler* handler, Auctionator* auctionator)
        {
            if (!auctionator->config->isEnabled)
            {
                handler->SendSysMessage("[Auctionator] note: the module is disabled in the configuration "
                    "(Auctionator.Enabled = 0); toggle it there and restart the server for these flags to take effect.");
            }
        }

        // Shared by enable/disable: returns false when the target is unknown.
        static bool ApplySellerToggle(std::string const& target, bool enable, Auctionator* auctionator)
        {
            AuctionatorConfig* config = auctionator->config;
            uint32 const value = enable ? 1 : 0;

            if (target == "hordeseller")          { config->hordeSeller.enabled = value; }
            else if (target == "allianceseller")  { config->allianceSeller.enabled = value; }
            else if (target == "neutralseller")   { config->neutralSeller.enabled = value; }
            else if (target == "hordebidder")     { config->hordeBidder.enabled = value; }
            else if (target == "alliancebidder")  { config->allianceBidder.enabled = value; }
            else if (target == "neutralbidder")   { config->neutralBidder.enabled = value; }
            else if (target == "all")
            {
                config->hordeSeller.enabled = value;
                config->allianceSeller.enabled = value;
                config->neutralSeller.enabled = value;
                config->hordeBidder.enabled = value;
                config->allianceBidder.enabled = value;
                config->neutralBidder.enabled = value;
            }
            else
            {
                return false;
            }

            // The flags are only read when the events are scheduled, so apply the
            // change to the running schedule right away (no restart needed).
            auctionator->ResyncEventSchedule();

            return true;
        }

        static bool CommandEnableSeller(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] enable: No target specified! [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                auctionator->logInfo("enable: No target specified!");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] enable: usage <target> (exactly one target).");
                return true;
            }

            WarnIfModuleDisabled(handler, auctionator);

            std::string const target = params[0];
            if (!ApplySellerToggle(target, true, auctionator))
            {
                handler->SendSysMessage("[Auctionator] enable: unknown target \"" + target
                    + "\" [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                return true;
            }

            auctionator->logInfo("enable: " + target);
            handler->SendSysMessage("[Auctionator] enable: " + target + " enabled, schedule updated.");
            return true;
        }

        static bool CommandDisableSeller(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] disable: No target specified! [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                auctionator->logInfo("disable: No target specified!");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] disable: usage <target> (exactly one target).");
                return true;
            }

            WarnIfModuleDisabled(handler, auctionator);

            std::string const target = params[0];
            if (!ApplySellerToggle(target, false, auctionator))
            {
                handler->SendSysMessage("[Auctionator] disable: unknown target \"" + target
                    + "\" [hordeseller, allianceseller, neutralseller, hordebidder, alliancebidder, neutralbidder, all]");
                return true;
            }

            auctionator->logInfo("disable: " + target);
            handler->SendSysMessage("[Auctionator] disable: " + target + " disabled, schedule updated.");
            return true;
        }

        static bool CommandSetMultiplier(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.size() < 1) {
                handler->SendSysMessage("[Auctionator] multiplier: No type specified! [seller, bidder]");
                auctionator->logInfo("multiplier: No type specified");
                return true;
            }

            if (params.size() < 2) {
                handler->SendSysMessage("[Auctionator] multiplier: No quality specified! [poor, normal, uncommon, rare, epic, legendary]");
                auctionator->logInfo("multiplier: No quality specified");
                return true;
            }

            if (params.size() < 3) {
                handler->SendSysMessage("[Auctionator] multiplier: No multiplier specified!");
                auctionator->logInfo("multiplier: No multiplier specified!");
                return true;
            }

            if (params.size() > 3) {
                handler->SendSysMessage("[Auctionator] multiplier: usage <seller|bidder> <quality> <value>");
                return true;
            }

            std::string type(params[0]);
            std::string quality(params[1]);
            float newMultiplier = 0.0f;
            if (!TryParseFloat(params[2], newMultiplier)) {
                handler->SendSysMessage("[Auctionator] multiplier: Number must be a valid float.");
                return true;
            }

            if (!std::isfinite(newMultiplier) || newMultiplier < 0.0f) {
                handler->SendSysMessage("[Auctionator] multiplier: Value must be a finite non-negative float.");
                return true;
            }

            if (newMultiplier > MaxPriceMultiplier)
            {
                handler->SendSysMessage("[Auctionator] multiplier: capped at "
                    + std::to_string(MaxPriceMultiplier) + ".");
                newMultiplier = MaxPriceMultiplier;
            }

            AuctionatorPriceMultiplierConfig* multipliers;

            if (type == "seller") {
                multipliers = &auctionator->config->sellerMultipliers;
            } else if (type == "bidder") {
                multipliers = &auctionator->config->bidderMultipliers;
            } else {
                handler->SendSysMessage("[Auctionator] multiplier: Invalid type! [seller, bidder]");
                return true;
            }

            bool success = false;

            if (quality == "poor") {
                multipliers->poor = newMultiplier;
                success = true;
            } else if (quality == "normal") {
                multipliers->normal = newMultiplier;
                success = true;
            } else if (quality == "uncommon") {
                multipliers->uncommon = newMultiplier;
                success = true;
            } else if (quality == "rare") {
                multipliers->rare = newMultiplier;
                success = true;
            } else if (quality == "epic") {
                multipliers->epic = newMultiplier;
                success = true;
            } else if (quality == "legendary") {
                multipliers->legendary = newMultiplier;
                success = true;
            }

            if (success) {
                handler->SendSysMessage("[Auctionator] " + type +
                    " multiplier: " + quality + " quality multiplier set to "
                    + std::to_string(newMultiplier));
            } else {
                handler->SendSysMessage("[Auctionator] unable to set multiplier");
            }

            return true;
        }

        static bool CommandBidOnOwn(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] bidonown: Need to specify 1 (on) or 0 (off).");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] bidonown: usage <0|1>");
                return true;
            }

            uint32 bidOnOwn = 0;
            if (!TryParseUInt32(params[0], bidOnOwn)) {
                handler->SendSysMessage("[Auctionator] bidonown: Need to specify 1 (on) or 0 (off).");
                return true;
            }

            if (bidOnOwn == 1) {
                auctionator->config->bidOnOwn = 1;
                handler->SendSysMessage("[Auctionator] bidonown: Bid on own enabled.");
            } else if (bidOnOwn == 0) {
                auctionator->config->bidOnOwn = 0;
                handler->SendSysMessage("[Auctionator] bidonown: Bid on own disabled.");
            } else {
                // Anything else used to be accepted silently, leaving the flag untouched.
                handler->SendSysMessage("[Auctionator] bidonown: only 1 (on) or 0 (off) are valid; nothing changed.");
            }

            return true;
        }

        static bool CommandBidsPerCycle(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] bidspercycle: Need to specify number of items to bid on.");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] bidspercycle: usage <value>");
                return true;
            }

            uint32 bidsPerCycle = 0;
            if (!TryParseUInt32(params[0], bidsPerCycle)) {
                handler->SendSysMessage("[Auctionator] bidspercycle: Need to specify a number.");
                return true;
            }

            if (bidsPerCycle > MaxBidderPurchasesPerCycle)
            {
                handler->SendSysMessage("[Auctionator] bidspercycle: capped at "
                    + std::to_string(MaxBidderPurchasesPerCycle) + " purchases per cycle.");
                bidsPerCycle = MaxBidderPurchasesPerCycle;
            }

            auctionator->config->allianceBidder.maxPerCycle = bidsPerCycle;
            auctionator->config->hordeBidder.maxPerCycle = bidsPerCycle;
            auctionator->config->neutralBidder.maxPerCycle = bidsPerCycle;

            handler->SendSysMessage("[Auctionator] bidspercycle: Set bids per cycle to " + std::to_string(bidsPerCycle));

            return true;
        }

        static bool CommandAuctionsPerCycle(std::vector<std::string> const& params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (params.empty()) {
                handler->SendSysMessage("[Auctionator] auctionspercycle: Need to specify a number.");
                return true;
            }

            if (params.size() > 1) {
                handler->SendSysMessage("[Auctionator] auctionspercycle: usage <value>");
                return true;
            }

            uint32 auctionsPerCycle = 0;
            if (!TryParseUInt32(params[0], auctionsPerCycle)) {
                handler->SendSysMessage("[Auctionator] auctionspercycle: Need to specify a number.");
                return true;
            }
            auctionator->config->sellerConfig.auctionsPerRun = auctionsPerCycle;
            handler->SendSysMessage("[Auctionator] auctionspercycle: Set auctions per cycle to "
                + std::to_string(auctionsPerCycle));

            return true;
        }
};

void AddAuctionatorCommands()
{
    new AuctionatorCommands();
}

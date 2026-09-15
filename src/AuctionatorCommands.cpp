
#include <iostream>
#include <string>
#include <limits>
#include "ScriptMgr.h"
#include "Chat.h"
#include "Auctionator.h"
#include "AuctionatorConfig.h"

using namespace Acore::ChatCommands;

namespace
{
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
}

class AuctionatorCommands : public CommandScript
{
    public:
        AuctionatorCommands() : CommandScript("AuctionatorCommandScript")
        {
        }

    private:
        static std::vector<const char*> BuildCommandArgs(const std::vector<std::string>& commandParams)
        {
            std::vector<const char*> commandParamsArray;
            commandParamsArray.reserve(commandParams.size());
            for (const std::string& param : commandParams)
            {
                commandParamsArray.push_back(param.c_str());
            }
            return commandParamsArray;
        }

        static bool DispatchCommand(ChatHandler* handler, Auctionator* auctionator, const std::string& command, const std::vector<std::string>& commandParams)
        {
            if (command == "add")
            {
                auctionator->logInfo("Adding new Item for GM");
                if (commandParams.size() >= 3)
                {
                    uint32 auctionHouseId = 0;
                    uint32 itemId = 0;
                    uint32 price = 0;

                    if (!TryParseUInt32(commandParams[0], auctionHouseId) ||
                        !TryParseUInt32(commandParams[1], itemId) ||
                        !TryParseUInt32(commandParams[2], price))
                    {
                        handler->SendSysMessage("[Auctionator] add: Invalid numeric arguments.");
                        return true;
                    }

                    AddItemForBuyout(auctionHouseId, itemId, price, auctionator);
                    return true;
                }

                handler->SendSysMessage("[Auctionator] add: Expected <houseid> <itemid> <price>");
                return true;
            }

            if (command == "auctionspercycle")
            {
                auto commandParamsArray = BuildCommandArgs(commandParams);
                return CommandAuctionsPerCycle(commandParamsArray.data(), handler, auctionator);
            }

            if (command == "bidonown")
            {
                auto commandParamsArray = BuildCommandArgs(commandParams);
                return CommandBidOnOwn(commandParamsArray.data(), handler, auctionator);
            }

            if (command == "bidspercycle")
            {
                auto commandParamsArray = BuildCommandArgs(commandParams);
                return CommandBidsPerCycle(commandParamsArray.data(), handler, auctionator);
            }

            if (command == "disable")
            {
                auto commandParamsArray = BuildCommandArgs(commandParams);
                return CommandDisableSeller(commandParamsArray.data(), handler, auctionator);
            }

            if (command == "enable")
            {
                auto commandParamsArray = BuildCommandArgs(commandParams);
                return CommandEnableSeller(commandParamsArray.data(), handler, auctionator);
            }

            if (command == "expireall")
            {
                auto commandParamsArray = BuildCommandArgs(commandParams);
                return CommandExpireAll(commandParamsArray.data(), handler, auctionator);
            }

            if (command == "multiplier")
            {
                auto commandParamsArray = BuildCommandArgs(commandParams);
                return CommandSetMultiplier(commandParamsArray.data(), handler, auctionator);
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

            return true;
        }

        static bool HandleCommandOptionsNew(ChatHandler* handler, const std::vector<std::string>& args)
        {
            std::string command = args.empty() ? "help" : args[0];
            if (command.empty())
            {
                return true;
            }

            std::vector<std::string> commandParams;
            commandParams.reserve(args.size() - 1);
            for (size_t i = 1; i < args.size(); ++i)
            {
                commandParams.push_back(args[i]);
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

        static void AddItemForBuyout(uint32 auctionHouseId, uint32 itemId, uint32 price, Auctionator* auctionator)
        {
            AuctionatorItem newItem;
            newItem.houseId = auctionHouseId;
            newItem.itemId = itemId;
            newItem.buyout = price;
            newItem.bid = 0;
            newItem.stackSize = 1;

            auctionator->CreateAuction(newItem);
        }

        static void ShowHelp(ChatHandler* handler)
        {
            std::string helpString(R"(
Auctionator Help:
add ...
auctionspercycle ...
bidonown ...
bidspercycle ...
disable ...
enable ...
expireall ...
multiplier ...
status
help
            )");
            handler->SendSysMessage(helpString);
        }

        static void ShowStatus(ChatHandler* handler, Auctionator* auctionator)
        {
            std::string statusString = "[Auctionator] Status:\n\n";

            statusString += " Enabled: " + std::to_string(auctionator->config->isEnabled) + "\n\n";
            statusString += " Bid on Own: " + std::to_string(auctionator->config->bidOnOwn) + "\n";
            statusString += " CharacterGuid: " + std::to_string(auctionator->config->characterGuid) + "\n";

            statusString += " Horde:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->hordeSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->hordeSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + std::to_string(auctionator->GetAuctionHouse((uint32)AuctionHouseId::Horde)->Getcount()) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->hordeBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->hordeBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->hordeBidder.maxPerCycle) + "\n";

            statusString += " Alliance:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->allianceSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->allianceSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + std::to_string(auctionator->GetAuctionHouse((uint32)AuctionHouseId::Alliance)->Getcount()) + "\n";
            statusString += "    Bidder Enabled: " + std::to_string(auctionator->config->allianceBidder.enabled) + "\n";
            statusString += "        Cycle Time: " + std::to_string(auctionator->config->allianceBidder.cycleMinutes) + "\n";
            statusString += "        Per Cycle: " + std::to_string(auctionator->config->allianceBidder.maxPerCycle) + "\n";

            statusString += " Neutral:\n";
            statusString += "    Seller Enabled: " + std::to_string(auctionator->config->neutralSeller.enabled) + "\n";
            statusString += "        Max Auctions: " + std::to_string(auctionator->config->neutralSeller.maxAuctions) + "\n";
            statusString += "        Auctions: " + std::to_string(auctionator->GetAuctionHouse((uint32)AuctionHouseId::Neutral)->Getcount()) + "\n";
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
            statusString += "    Query Limit: " + std::to_string(auctionator->config->sellerConfig.queryLimit) + "\n";
            statusString += "    Default Price: " + std::to_string(auctionator->config->sellerConfig.defaultPrice) + "\n";
            statusString += "    Randomize Stack Size: " + std::to_string(auctionator->config->sellerConfig.randomizeStackSize) + "\n";
            statusString += "    Bid Start Modifier: " + std::to_string(auctionator->config->sellerConfig.bidStartModifier) + "\n";

            handler->SendSysMessage(statusString);
        }

        static bool CommandExpireAll(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] expireall: No Auction House Specified!");
                auctionator->logInfo("expireall: No Auction House Specified!");
                return true;
            }
            uint32 houseId = 0;
            if (!TryParseUInt32(params[0], houseId)) {
                handler->SendSysMessage("[Auctionator] expireall: House id must be a number.");
                return true;
            }

            handler->SendSysMessage("[Auctionator] Expiring all Auctions for house: "
                + std::to_string(houseId));
            auctionator->logInfo("expireall: Expiring all auctions for house: "
                + std::to_string(houseId));

            auctionator->ExpireAllAuctions(houseId);

            auctionator->logInfo("expireall: All auctions expired for house: "
                + std::to_string(houseId));

            return true;
        }

        static bool CommandEnableSeller(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] enable: No Auction House Specified! [hordeseller, allianceseller, neutralseller, all]");
                auctionator->logInfo("enable: No Auction House Specified!");
                return true;
            }

            std::string toEnable(params[0]);
            if (toEnable == "hordeseller") {
                auctionator->config->hordeSeller.enabled = 1;
                auctionator->logInfo("Horde seller enabled");
                return true;
            } else if (toEnable == "allianceseller") {
                auctionator->config->allianceSeller.enabled = 1;
                auctionator->logInfo("Alliance seller enabled");
                return true;
            } else if (toEnable == "neutralseller") {
                auctionator->config->neutralSeller.enabled = 1;
                auctionator->logInfo("Neutral seller enabled");
                return true;
            } else if (toEnable == "all") {
                auctionator->config->hordeSeller.enabled = 1;
                auctionator->config->allianceSeller.enabled = 1;
                auctionator->config->neutralSeller.enabled = 1;
                auctionator->config->hordeBidder.enabled = 1;
                auctionator->config->allianceBidder.enabled = 1;
                auctionator->config->neutralBidder.enabled = 1;
                auctionator->logInfo("All sellers enabled");
                return true;
            } else if (toEnable == "hordebidder") {
                auctionator->config->hordeBidder.enabled = 1;
                auctionator->logInfo("Horde bidder enabled");
                return true;
            } else if (toEnable == "alliancebidder") {
                auctionator->config->allianceBidder.enabled = 1;
                auctionator->logInfo("Alliance bidder enabled");
                return true;
            } else if (toEnable == "neutralbidder") {
                auctionator->config->neutralBidder.enabled = 1;
                auctionator->logInfo("Neutral bidder enabled");
                return true;
            }

            return true;
        }

        static bool CommandDisableSeller(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] disable: No Auction House Specified! [hordeseller, allianceseller, neutralseller, all]");
                auctionator->logInfo("disable: No Auction House Specified!");
                return true;
            }

            std::string toDisable(params[0]);
            if (toDisable == "hordeseller") {
                auctionator->config->hordeSeller.enabled = 0;
                auctionator->logInfo("Horde seller disabled");
                return true;
            } else if (toDisable == "allianceseller") {
                auctionator->config->allianceSeller.enabled = 0;
                auctionator->logInfo("Alliance seller disabled");
                return true;
            } else if (toDisable == "neutralseller") {
                auctionator->config->neutralSeller.enabled = 0;
                auctionator->logInfo("Neutral seller disabled");
                return true;
            } else if (toDisable == "all") {
                auctionator->config->hordeSeller.enabled = 0;
                auctionator->config->allianceSeller.enabled = 0;
                auctionator->config->neutralSeller.enabled = 0;
                auctionator->config->hordeBidder.enabled = 0;
                auctionator->config->allianceBidder.enabled = 0;
                auctionator->config->neutralBidder.enabled = 0;
                auctionator->logInfo("All sellers disabled");
                return true;
            } else if (toDisable == "hordebidder") {
                auctionator->config->hordeBidder.enabled = 0;
                auctionator->logInfo("Horde bidder disabled");
                return true;
            } else if (toDisable == "alliancebidder") {
                auctionator->config->allianceBidder.enabled = 0;
                auctionator->logInfo("Alliance bidder disabled");
                return true;
            } else if (toDisable == "neutralbidder") {
                auctionator->config->neutralBidder.enabled = 0;
                auctionator->logInfo("Neutral bidder disabled");
                return true;
            }

            return true;
        }

        static bool CommandSetMultiplier(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] multiplier: No type specified! [seller, bidder]");
                auctionator->logInfo("multiplier: No type specified");
                return true;
            }

            if (!params[1]) {
                handler->SendSysMessage("[Auctionator] multiplier: No quality specified! [poor, normal, uncommon, rare, epic, legendary]");
                auctionator->logInfo("multiplier: No quality specified");
                return true;
            }

            if (!params[2]) {
                handler->SendSysMessage("[Auctionator] multiplier: No multiplier specified!");
                auctionator->logInfo("multiplier: No multiplier specified Specified!");
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

        static bool CommandBidOnOwn(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] bitonown: Need to specify 1 (on) or 0 (off).");
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
            }

            return true;
        }

        static bool CommandBidsPerCycle(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] bidspercycle: Need to specify number of items to bid on.");
                return true;
            }

            uint32 bidsPerCycle = 0;
            if (!TryParseUInt32(params[0], bidsPerCycle)) {
                handler->SendSysMessage("[Auctionator] bidspercycle: Need to specify a number.");
                return true;
            }

            auctionator->config->allianceBidder.maxPerCycle = bidsPerCycle;
            auctionator->config->hordeBidder.maxPerCycle = bidsPerCycle;
            auctionator->config->neutralBidder.maxPerCycle = bidsPerCycle;

            handler->SendSysMessage("[Auctionator] bidspercycle: Set bids per cycle to " + std::to_string(bidsPerCycle));

            return true;
        }

        static bool CommandAuctionsPerCycle(const char** params, ChatHandler* handler, Auctionator* auctionator)
        {
            if (!params[0]) {
                handler->SendSysMessage("[Auctionator] auctionspercycle: Need to specify a number.");
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

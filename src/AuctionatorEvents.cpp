
#include "AuctionatorEvents.h"
#include "AuctionatorBidder.h"
#include "AuctionatorSeller.h"
#include "Log.h"
#include <functional>
#include <chrono>

AuctionatorEvents::AuctionatorEvents(AuctionatorConfig* auctionatorConfig)
{
    SetLogPrefix("[AuctionatorEvents] ");
    events = EventMap();
    config = auctionatorConfig;
    InitializeEvents();
}

AuctionatorEvents::~AuctionatorEvents()
{}

void AuctionatorEvents::InitializeEvents()
{
    if (!config) {
        logError("AuctionatorEvents::InitializeEvents skipped: config is null.");
        return;
    }

    logInfo("Initializing events");

    eventToFunction = {
            {1, "AllianceBidder"},
            {2, "HordeBidder"},
            {3, "NeutralBidder"},
            {4, "AllianceSeller"},
            {5, "HordeSeller"},
            {6, "NeutralSeller"}
        };

    eventHandlers = {
            {1, &AuctionatorEvents::EventAllianceBidder},
            {2, &AuctionatorEvents::EventHordeBidder},
            {3, &AuctionatorEvents::EventNeutralBidder},
            {4, &AuctionatorEvents::EventAllianceSeller},
            {5, &AuctionatorEvents::EventHordeSeller},
            {6, &AuctionatorEvents::EventNeutralSeller}
        };

    if (config->allianceBidder.enabled) {
        events.ScheduleEvent(1, std::chrono::minutes(config->allianceBidder.cycleMinutes));
    }
    if (config->hordeBidder.enabled) {
        events.ScheduleEvent(2, std::chrono::minutes(config->hordeBidder.cycleMinutes));
    }
    if (config->neutralBidder.enabled) {
        events.ScheduleEvent(3, std::chrono::minutes(config->neutralBidder.cycleMinutes));
    }
    if (config->allianceSeller.enabled) {
        events.ScheduleEvent(4, std::chrono::minutes(config->allianceSeller.cycleMinutes));
    }
    if (config->hordeSeller.enabled) {
        events.ScheduleEvent(5, std::chrono::minutes(config->hordeSeller.cycleMinutes));
    }
    if (config->neutralSeller.enabled) {
        events.ScheduleEvent(6, std::chrono::minutes(config->neutralSeller.cycleMinutes));
    }
}

std::chrono::minutes AuctionatorEvents::GetEventInterval(uint32 currentEvent) const
{
    switch (currentEvent) {
        case 1:
            return std::chrono::minutes(config->allianceBidder.cycleMinutes);
        case 2:
            return std::chrono::minutes(config->hordeBidder.cycleMinutes);
        case 3:
            return std::chrono::minutes(config->neutralBidder.cycleMinutes);
        case 4:
            return std::chrono::minutes(config->allianceSeller.cycleMinutes);
        case 5:
            return std::chrono::minutes(config->hordeSeller.cycleMinutes);
        case 6:
            return std::chrono::minutes(config->neutralSeller.cycleMinutes);
        default:
            return std::chrono::minutes(1);
    }
}

void AuctionatorEvents::RescheduleEvent(uint32 currentEvent)
{
    if (!config) {
        return;
    }

    switch (currentEvent) {
        case 1:
            if (config->allianceBidder.enabled) {
                events.ScheduleEvent(currentEvent, GetEventInterval(currentEvent));
            }
            break;
        case 2:
            if (config->hordeBidder.enabled) {
                events.ScheduleEvent(currentEvent, GetEventInterval(currentEvent));
            }
            break;
        case 3:
            if (config->neutralBidder.enabled) {
                events.ScheduleEvent(currentEvent, GetEventInterval(currentEvent));
            }
            break;
        case 4:
            if (config->allianceSeller.enabled) {
                events.ScheduleEvent(currentEvent, GetEventInterval(currentEvent));
            }
            break;
        case 5:
            if (config->hordeSeller.enabled) {
                events.ScheduleEvent(currentEvent, GetEventInterval(currentEvent));
            }
            break;
        case 6:
            if (config->neutralSeller.enabled) {
                events.ScheduleEvent(currentEvent, GetEventInterval(currentEvent));
            }
            break;
        default:
            break;
    }
}

void AuctionatorEvents::DispatchEvent(uint32 currentEvent)
{
    if (!config) {
        logError("AuctionatorEvents::DispatchEvent skipped: config is null.");
        return;
    }

    auto eventIt = eventToFunction.find(currentEvent);
    if (eventIt == eventToFunction.end()) {
        logError("Unknown event id: " + std::to_string(currentEvent));
        return;
    }

    auto handlerIt = eventHandlers.find(currentEvent);
    if (handlerIt == eventHandlers.end() || !handlerIt->second) {
        logError("No handler registered for event id: " + std::to_string(currentEvent));
        return;
    }

    logInfo("Executing event: " + eventIt->second);

    try {
        (this->*(handlerIt->second))();
    } catch (const std::exception& e) {
        logError("Issue calling handler for event id " + std::to_string(currentEvent));
        logError(e.what());
        return;
    }

    RescheduleEvent(currentEvent);
}

void AuctionatorEvents::ExecuteEvents()
{
    logInfo("Executing events");
    uint32 currentEvent = events.ExecuteEvent();
    while (currentEvent != 0) {
        DispatchEvent(currentEvent);
        currentEvent = events.ExecuteEvent();
    }
}

void AuctionatorEvents::Update(uint32 deltaMinutes)
{
    events.Update(deltaMinutes);
    ExecuteEvents();
}

void AuctionatorEvents::SetPlayerGuid(ObjectGuid playerGuid)
{
    auctionatorGuid = playerGuid;
}

void AuctionatorEvents::SetHouses(AuctionatorHouses* auctionatorHouses)
{
    houses = auctionatorHouses;
}

void AuctionatorEvents::EventAllianceBidder()
{
    if (!config) {
        logError("Alliance bidder skipped: config is null.");
        return;
    }

    logInfo("Starting Alliance Bidder");
    AuctionatorBidder bidder = AuctionatorBidder((uint32)AuctionHouseId::Alliance, auctionatorGuid, config);
    bidder.SpendSomeCash();
}

void AuctionatorEvents::EventHordeBidder()
{
    if (!config) {
        logError("Horde bidder skipped: config is null.");
        return;
    }

    logInfo("Starting Horde Bidder");
    AuctionatorBidder bidder = AuctionatorBidder((uint32)AuctionHouseId::Horde, auctionatorGuid, config);
    bidder.SpendSomeCash();
}

void AuctionatorEvents::EventNeutralBidder()
{
    if (!config) {
        logError("Neutral bidder skipped: config is null.");
        return;
    }

    logInfo("Starting Neutral Bidder");
    AuctionatorBidder bidder = AuctionatorBidder((uint32)AuctionHouseId::Neutral, auctionatorGuid, config);
    bidder.SpendSomeCash();
}

void AuctionatorEvents::EventAllianceSeller()
{
    if (!houses || !Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Alliance)) {
        logError("Alliance auction house is not initialized.");
        return;
    }

    AuctionatorSeller sellerAlliance =
        AuctionatorSeller(Auctionator::getInstance(), static_cast<uint32>(AuctionHouseId::Alliance));

    uint32 auctionCountAlliance = Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Alliance)->Getcount();

    if (auctionCountAlliance < config->allianceSeller.maxAuctions) {
        logInfo(
            "Alliance count is good, here we go: "
            + std::to_string(auctionCountAlliance)
            + " of " + std::to_string(config->allianceSeller.maxAuctions)
        );

        sellerAlliance.LetsGetToIt(
            config->sellerConfig.auctionsPerRun,
            (uint32)AuctionHouseId::Alliance
        );
    } else {
        logInfo("Alliance count over max: " + std::to_string(auctionCountAlliance));
    }
    
}

void AuctionatorEvents::EventHordeSeller()
{
    if (!houses || !Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Horde)) {
        logError("Horde auction house is not initialized.");
        return;
    }

    AuctionatorSeller sellerHorde =
        AuctionatorSeller(Auctionator::getInstance(), static_cast<uint32>(AuctionHouseId::Horde));

    uint32 auctionCountHorde = Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Horde)->Getcount();

    if (auctionCountHorde < config->hordeSeller.maxAuctions) {
        logInfo(
            "Horde count is good, here we go: "
            + std::to_string(auctionCountHorde)
            + " of " + std::to_string(config->hordeSeller.maxAuctions)
        );

        sellerHorde.LetsGetToIt(
            config->sellerConfig.auctionsPerRun,
            (uint32)AuctionHouseId::Horde
        );
    } else {
        logInfo("Horde count over max: " + std::to_string(auctionCountHorde));
    }
}

void AuctionatorEvents::EventNeutralSeller()
{
    if (!houses || !Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Neutral)) {
        logError("Neutral auction house is not initialized.");
        return;
    }

    AuctionatorSeller sellerNeutral =
        AuctionatorSeller(Auctionator::getInstance(), static_cast<uint32>(AuctionHouseId::Neutral));

    uint32 auctionCountNeutral = Auctionator::getInstance()->GetAuctionHouse((uint32)AuctionHouseId::Neutral)->Getcount();

    if (auctionCountNeutral < config->neutralSeller.maxAuctions) {
        logInfo(
            "Neutral count is good, here we go: "
            + std::to_string(auctionCountNeutral)
            + " of " + std::to_string(config->neutralSeller.maxAuctions)
        );

        sellerNeutral.LetsGetToIt(
            config->sellerConfig.auctionsPerRun,
            (uint32)AuctionHouseId::Neutral
        );

    } else {
        logInfo("Neutral count over max: " + std::to_string(auctionCountNeutral));
    }
}

EventMap AuctionatorEvents::GetEvents()
{
    return events;
}

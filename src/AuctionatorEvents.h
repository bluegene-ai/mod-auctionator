
#ifndef AUCTIONATOR_EVENTS_H
#define AUCTIONATOR_EVENTS_H

#include "AuctionatorBase.h"
#include "AuctionatorConfig.h"
#include "AuctionatorStructs.h"
#include "EventMap.h"
#include "ObjectMgr.h"
#include <unordered_map>
#include <functional>

using FunctionType = void (*)();

class AuctionatorEvents : public AuctionatorBase
{
    private:
        using EventHandler = void (AuctionatorEvents::*)();

        ObjectGuid auctionatorGuid;
        EventMap events;
        std::unordered_map<uint32, std::string> eventToFunction;
        std::unordered_map<uint32, EventHandler> eventHandlers;
        AuctionatorConfig* config;
        AuctionatorHouses* houses;

        void DispatchEvent(uint32 currentEvent);
        void RescheduleEvent(uint32 currentEvent);
        std::chrono::minutes GetEventInterval(uint32 currentEvent) const;

    public:
        AuctionatorEvents() {};
        AuctionatorEvents(AuctionatorConfig* auctionatorConfig);
        ~AuctionatorEvents();
        void InitializeEvents();
        void EventAllianceBidder();
        void EventHordeBidder();
        void EventNeutralBidder();
        void EventAllianceSeller();
        void EventHordeSeller();
        void EventNeutralSeller();
        void ExecuteEvents();
        void Update(uint32 deltaMinutes = 1);
        void SetHouses(AuctionatorHouses* auctionatorHouses);
        void SetPlayerGuid(ObjectGuid playerGuid);
        EventMap GetEvents();
};

#endif

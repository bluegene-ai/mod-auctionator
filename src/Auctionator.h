
#ifndef AUCTIONATOR_H
#define AUCTIONATOR_H

#include "Common.h"
#include "ObjectGuid.h"
#include "ItemTemplate.h"
#include "AuctionatorConfig.h"
#include "AuctionHouseMgr.h"
#include "Config.h"
#include "EventMap.h"
#include "AuctionatorEvents.h"
#include "AuctionatorBase.h"

struct AuctionatorItem
{
    uint32 itemId;
    uint32 houseId;
    uint32 bid;
    uint32 buyout;
    uint32 time;
    uint32 quantity;
    uint32 stackSize;

    AuctionatorItem() :
        itemId(0),
        houseId((uint32)AuctionHouseId::Neutral),
        bid(0),
        buyout(0),
        time(172800),
        quantity(1),
        stackSize(1) {}
};

class Auctionator : public AuctionatorBase
{
    private:
        Auctionator();
        bool initialized = false;
        AuctionHouseObject* HordeAh;
        AuctionHouseObject* AllianceAh;
        AuctionHouseObject* NeutralAh;
        AuctionHouseEntry const* HordeAhEntry;
        AuctionHouseEntry const* AllianceAhEntry;
        AuctionHouseEntry const* NeutralAhEntry;
        WorldSession *session;
        AuctionatorHouses* houses;
        AuctionatorEvents events;

    public:
        ~Auctionator();
        void CreateAuction(AuctionatorItem newItem);
        void ExpireAllAuctions(uint32 houseId);
        AuctionHouseEntry const *GetAuctionHouseEntry(uint32 houseId);
        AuctionHouseObject *GetAuctionHouse(uint32 houseId);
        bool ValidateHouseState(uint32 houseId, AuctionHouseObject*& house, AuctionHouseEntry const*& entry) const;
        void Initialize();
        void InitializeConfig(ConfigMgr* configMgr);
        AuctionatorConfig *config;
        void Update();
        bool IsReady() const
        {
            return initialized && config && session && sAuctionMgr && sWorld;
        }

        AuctionHouseObject *GetAuctionMgr(uint32 auctionHouseId);
        static float GetQualityMultiplier(AuctionatorPriceMultiplierConfig config, uint32 quality);

        static Auctionator *getInstance()
        {
            static Auctionator instance;
            return &instance;
        }
};

// Legacy compatibility alias kept only for old call sites during migration.
// New code should use Auctionator::getInstance() explicitly.
#define gAuctionator Auctionator::getInstance()

#endif

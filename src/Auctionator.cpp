
#include "Log.h"
#include "Auctionator.h"
#include "Config.h"
#include "WorldSession.h"
#include "AuctionHouseMgr.h"
#include "GameTime.h"
#include "ObjectMgr.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "AuctionatorConfig.h"
#include "AuctionatorSeller.h"
#include "AuctionatorBidder.h"
#include "AuctionatorEvents.h"
#include "AuctionatorStructs.h"
#include "EventMap.h"
#include <vector>

Auctionator::Auctionator()
{
    SetLogPrefix("[Auctionator] ");
    config = nullptr;
    houses = nullptr;
    initialized = false;
    InitializeConfig(sConfigMgr);
    Initialize();

    logInfo("Event init");

    if (!houses) {
        houses = new AuctionatorHouses();
    }

    houses->HordeAh = HordeAh;
    houses->AllianceAh = AllianceAh;
    houses->NeutralAh = NeutralAh;

    ObjectGuid buyerGuid = ObjectGuid::Create<HighGuid::Player>(config->characterGuid);
    events = AuctionatorEvents(config);
    events.SetPlayerGuid(buyerGuid);
    events.SetHouses(houses);

    if (!IsReady()) {
        logError("[Auctionator] initialization incomplete after setup.");
    }
};

Auctionator::~Auctionator()
{
    if (session) {
        delete session;
        session = nullptr;
    }

    if (houses) {
        delete houses;
        houses = nullptr;
    }

    if (config) {
        delete config;
        config = nullptr;
    }
}

void Auctionator::CreateAuction(AuctionatorItem newItem)
{
    if (!config || !sAuctionMgr || !sWorld) {
        logError("Auctionator CreateAuction failed: config/world/auction manager is not ready.");
        return;
    }

    if (!session) {
        logError("Auctionator CreateAuction failed: session is not initialized.");
        return;
    }

    if (config->characterGuid == 0) {
        logError("Auctionator CreateAuction failed: characterGuid is zero; system auction owner is not configured.");
        return;
    }

    if (newItem.itemId == 0) {
        logError("Auctionator CreateAuction failed: itemId is zero.");
        return;
    }

    if (newItem.stackSize == 0) {
        newItem.stackSize = 1;
    }

    if (newItem.stackSize > 20) {
        newItem.stackSize = 20;
    }

    if (newItem.houseId != (uint32)AuctionHouseId::Alliance &&
        newItem.houseId != (uint32)AuctionHouseId::Horde &&
        newItem.houseId != (uint32)AuctionHouseId::Neutral) {
        logError("Auctionator CreateAuction failed: invalid houseId " + std::to_string(newItem.houseId));
        return;
    }

    AuctionHouseObject* house = nullptr;
    AuctionHouseEntry const* entry = nullptr;
    if (!ValidateHouseState(newItem.houseId, house, entry)) {
        logError("Auctionator CreateAuction failed: auction house data is not initialized for houseId " + std::to_string(newItem.houseId));
        return;
    }

    if (newItem.buyout == 0 && newItem.bid == 0) {
        logError("Auctionator CreateAuction failed: item " + std::to_string(newItem.itemId) + " has zero buyout and bid prices.");
        return;
    }

    // will need this when we want to know details of the item for filtering
    // ItemTemplate const* prototype = sObjectMgr->GetItemTemplate(itemId);


    Player player(session);
    player.Initialize(config->characterGuid);
    ObjectAccessor::AddObject(&player);
    uint32 houseId = newItem.houseId;

    logDebug("Creating Auction for item: " + std::to_string(newItem.itemId));
    // Create the item (and add it to the update queue for the player ")
    Item* item = Item::CreateItem(newItem.itemId, 1, &player);
    if (!item) {
        logError("Auctionator CreateAuction failed: unable to create item " + std::to_string(newItem.itemId));
        ObjectAccessor::RemoveObject(&player);
        return;
    }

    logTrace("adding item to player queue");
    item->AddToUpdateQueueOf(&player);
    uint32 randomPropertyId = Item::GenerateItemRandomPropertyId(newItem.itemId);
    if (randomPropertyId != 0) {
        logDebug("adding random properties");
        item->SetItemRandomProperties(randomPropertyId);
    }

    // set our quantity. If this is a stack it needs to get set here
    // on the item instance and not on the auction item.
    item->SetCount(1);
    if (newItem.stackSize > 1) {
        item->SetCount(newItem.stackSize);
    }

    logTrace("starting character transaction for AH item");
    auto trans = CharacterDatabase.BeginTransaction();

    logTrace("creating auction entry");
    AuctionEntry* auctionEntry = new AuctionEntry();
    auctionEntry->Id = sObjectMgr->GenerateAuctionID();
    auctionEntry->houseId = (AuctionHouseId)houseId;
    auctionEntry->item_guid = item->GetGUID();
    auctionEntry->item_template = item->GetEntry();
    auctionEntry->itemCount = newItem.stackSize;
    auctionEntry->owner = ObjectGuid::Create<HighGuid::Player>(config->characterGuid);
    auctionEntry->startbid = newItem.bid;
    auctionEntry->buyout = newItem.buyout;
    auctionEntry->bid = 0;
    auctionEntry->bidder = ObjectGuid::Empty;
    auctionEntry->deposit = std::max<uint32>(100, newItem.buyout / 10);
    auctionEntry->expire_time = (time_t)newItem.time + time(nullptr);
    auctionEntry->auctionHouseEntry = sAuctionMgr->GetAuctionHouseEntryFromHouse((AuctionHouseId)houseId);

    if (!auctionEntry->auctionHouseEntry) {
        logError("Auctionator CreateAuction failed: unable to resolve auction house entry for houseId " + std::to_string(houseId));
        delete auctionEntry;
        ObjectAccessor::RemoveObject(&player);
        return;
    }

    logTrace("save item to db");
    item->SaveToDB(trans);

    logTrace("removed from character queue");
    item->RemoveFromUpdateQueueOf(&player);

    if (item->GetGUID().IsEmpty()) {
        logError("Auctionator CreateAuction failed: item GUID was invalid after save for item " + std::to_string(newItem.itemId));
        item->RemoveFromWorld();
        delete item;
        ObjectAccessor::RemoveObject(&player);
        return;
    }

    //
    // Add the Item we are auctioning to the managers item list.
    // This is NOT faction specific, it's a global list shared by
    // all factions.
    //
    logTrace("add item to auction mgr");
    sAuctionMgr->AddAItem(item);

    //
    // Add the AuctionEntry to the correct auction house. This IS
    // faction specific and you must use the correct house for the
    // faction you want the item to show up in or it will show up
    // ... somewhere else.
    //
    logTrace("add item entry to auction house: " + std::to_string(houseId));
    GetAuctionHouse(houseId)->AddAuction(auctionEntry);

    //
    // Save your AuctionHouseEntry object to the
    // `acore_characters`.`auctionhouse` table.
    //
    logTrace("save auction entry");
    auctionEntry->SaveToDB(trans);

    logTrace("commit character transaction");
    CharacterDatabase.CommitTransaction(trans);

    ObjectAccessor::RemoveObject(&player);
}

/**
 * Use this to get access to the AuctionHouseEntry object pointer
 * for a specific auction house.
 *
 * Ultimately this is just a global singleton.
*/
AuctionHouseEntry const *Auctionator::GetAuctionHouseEntry(uint32 houseId)
{
    switch(houseId) {
        case((uint32)AuctionHouseId::Alliance):
            return AllianceAhEntry;
            break;
        case((uint32)AuctionHouseId::Horde):
            return HordeAhEntry;
            break;
        default:
            return NeutralAhEntry;
    }
}

/**
 * Use this to get access to the AuctionHouseObject pointer for a
 * specific auction house.
 *
 * Ultimately this is just a global singleton.
*/
AuctionHouseObject *Auctionator::GetAuctionHouse(uint32 houseId) {
    switch(houseId) {
        case((uint32)AuctionHouseId::Alliance):
            return AllianceAh;
            break;
        case((uint32)AuctionHouseId::Horde):
            return HordeAh;
            break;
        default:
            return NeutralAh;
    }
}

bool Auctionator::ValidateHouseState(uint32 houseId, AuctionHouseObject*& house, AuctionHouseEntry const*& entry) const
{
    switch (houseId)
    {
        case (uint32)AuctionHouseId::Alliance:
            house = AllianceAh;
            entry = AllianceAhEntry;
            break;
        case (uint32)AuctionHouseId::Horde:
            house = HordeAh;
            entry = HordeAhEntry;
            break;
        case (uint32)AuctionHouseId::Neutral:
        default:
            house = NeutralAh;
            entry = NeutralAhEntry;
            break;
    }

    return house != nullptr && entry != nullptr;
}

void Auctionator::Initialize()
{
    if (!config) {
        logError("Auctionator config is null, cannot initialize.");
        initialized = false;
        return;
    }

    if (!sAuctionMgr || !sWorld) {
        logError("Auctionator initialization failed: world or auction manager is unavailable.");
        initialized = false;
        return;
    }

    if (config->characterId == 0 || config->characterGuid == 0) {
        logWarn("Auctionator requires valid CharacterId and CharacterGuid before creating session; seller and bidder features stay disabled.");
        initialized = false;
        return;
    }

    if (!houses) {
        houses = new AuctionatorHouses();
    }

    std::string accountName = "Auctionator";

    HordeAh = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Horde);
    HordeAhEntry = sAuctionHouseStore.LookupEntry((uint32)AuctionHouseId::Horde);

    AllianceAh = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Alliance);
    AllianceAhEntry = sAuctionHouseStore.LookupEntry((uint32)AuctionHouseId::Alliance);

    NeutralAh = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Neutral);
    NeutralAhEntry = sAuctionHouseStore.LookupEntry((uint32)AuctionHouseId::Neutral);

    if (session) {
        delete session;
        session = nullptr;
    }

    session = new WorldSession(
        config->characterId,
        std::move(accountName),
        0,
        nullptr,
        SEC_GAMEMASTER,
        sWorld->getIntConfig(CONFIG_EXPANSION),
        0,
        LOCALE_enUS,
        0,
        false,
        false,
        0
    );

    initialized = session != nullptr && sAuctionMgr != nullptr && sWorld != nullptr &&
        HordeAh != nullptr && AllianceAh != nullptr && NeutralAh != nullptr &&
        HordeAhEntry != nullptr && AllianceAhEntry != nullptr && NeutralAhEntry != nullptr;
}

void Auctionator::InitializeConfig(ConfigMgr* configMgr)
{
    logInfo("Initializing Auctionator Config");

    if (!config) {
        config = new AuctionatorConfig();
    }
    config->isEnabled = configMgr->GetOption<bool>("Auctionator.Enabled", false);
    logInfo("config->isEnabled: "
        + std::to_string(config->isEnabled));

    config->characterId = configMgr->GetOption<uint32>("Auctionator.CharacterId", 0);
    config->characterGuid = configMgr->GetOption<uint32>("Auctionator.CharacterGuid", 0);
    logInfo("CharacterIds: "
        + std::to_string(config->characterId)
        + "::"
        + std::to_string(config->characterGuid)
    );

    if (config->characterId == 0 || config->characterGuid == 0) {
        logError("Auctionator requires a valid CharacterId and CharacterGuid. Seller and bidder features may be disabled until configured.");
    }

    config->hordeSeller.enabled = configMgr->GetOption<uint32>("Auctionator.HordeSeller.Enabled", 0);
    config->allianceSeller.enabled = configMgr->GetOption<uint32>("Auctionator.AllianceSeller.Enabled", 0);
    config->neutralSeller.enabled = configMgr->GetOption<uint32>("Auctionator.NeutralSeller.Enabled", 0);

    config->hordeSeller.maxAuctions = configMgr->GetOption<uint32>("Auctionator.HordeSeller.MaxAuctions", 50);
    config->allianceSeller.maxAuctions = configMgr->GetOption<uint32>("Auctionator.AllianceSeller.MaxAuctions", 50);
    config->neutralSeller.maxAuctions = configMgr->GetOption<uint32>("Auctionator.NeutralSeller.MaxAuctions", 50);

    config->hordeSeller.cycleMinutes = std::max<uint32>(1, configMgr->GetOption<uint32>("Auctionator.HordeSeller.CycleMinutes", 1));
    config->allianceSeller.cycleMinutes = std::max<uint32>(1, configMgr->GetOption<uint32>("Auctionator.AllianceSeller.CycleMinutes", 1));
    config->neutralSeller.cycleMinutes = std::max<uint32>(1, configMgr->GetOption<uint32>("Auctionator.NeutralSeller.CycleMinutes", 1));

    // Load our seller configurations
    config->sellerConfig.auctionsPerRun = configMgr->GetOption<uint32>("Auctionator.Seller.AuctionsPerRun", 100);
    config->sellerConfig.defaultPrice = configMgr->GetOption<uint32>("Auctionator.Seller.DefaultPrice", 10000000);
    config->sellerConfig.queryLimit = std::max<uint32>(
        1,
        configMgr->GetOption<uint32>("Auctionator.Seller.QueryLimit", config->sellerConfig.auctionsPerRun)
    );
    config->sellerConfig.randomizeStackSize = configMgr->GetOption<uint32>("Auctionator.Seller.RandomizeStackSize", 1);
    config->sellerConfig.bidStartModifier = std::clamp(
        configMgr->GetOption<float>("Auctionator.Seller.BidStartModifier", 1.0f),
        0.0f,
        1.0f
    );

    // Load our bidder configurations
    config->allianceBidder.enabled = configMgr->GetOption<uint32>("Auctionator.AllianceBidder.Enabled", 0);
    config->allianceBidder.cycleMinutes = configMgr->GetOption<uint32>("Auctionator.AllianceBidder.CycleMinutes", 30);
    config->allianceBidder.maxPerCycle = configMgr->GetOption<uint32>("Auctionator.AllianceBidder.MaxPerCycle", 1);

    config->hordeBidder.enabled = configMgr->GetOption<uint32>("Auctionator.HordeBidder.Enabled", 0);
    config->hordeBidder.cycleMinutes = configMgr->GetOption<uint32>("Auctionator.HordeBidder.CycleMinutes", 30);
    config->hordeBidder.maxPerCycle = configMgr->GetOption<uint32>("Auctionator.HordeBidder.MaxPerCycle", 1);

    config->neutralBidder.enabled = configMgr->GetOption<uint32>("Auctionator.NeutralBidder.Enabled", 0);
    config->neutralBidder.cycleMinutes = configMgr->GetOption<uint32>("Auctionator.NeutralBidder.CycleMinutes", 30);
    config->neutralBidder.maxPerCycle = configMgr->GetOption<uint32>("Auctionator.NeutralBidder.MaxPerCycle", 1);

    config->bidOnOwn = configMgr->GetOption<uint32>("Auctionator.Bidder.BidOnOwn", 0);

    // load out multipliers for seller prices
    config->sellerMultipliers.poor
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Seller.Poor", 1.0f));
    config->sellerMultipliers.normal
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Seller.Normal", 1.0f));
    config->sellerMultipliers.uncommon
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Seller.Uncommon", 1.5f));
    config->sellerMultipliers.rare
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Seller.Rare", 2.0f));
    config->sellerMultipliers.epic
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Seller.Epic", 6.0f));
    config->sellerMultipliers.legendary
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Seller.Legendary", 10.0f));

    // load out multipliers for bidder prices
    config->bidderMultipliers.poor
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Bidder.Poor", 1.0f));
    config->bidderMultipliers.normal
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Bidder.Normal", 1.0f));
    config->bidderMultipliers.uncommon
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Bidder.Uncommon", 1.5f));
    config->bidderMultipliers.rare
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Bidder.Rare", 2.0f));
    config->bidderMultipliers.epic
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Bidder.Epic", 6.0f));
    config->bidderMultipliers.legendary
        = std::max(0.0f, configMgr->GetOption<float>("Auctionator.Multipliers.Bidder.Legendary", 10.0f));

    logInfo("Auctionator config initialized");
}

/**
 * Update gets called on each "tick" of the global sAuctionHouseManager.
*/
void Auctionator::Update()
{
    if (!IsReady()) {
        logWarn("Auctionator update skipped: module is not ready.");
        return;
    }

    if (!NeutralAh || !AllianceAh || !HordeAh) {
        logWarn("Auctionator update skipped: one or more auction house maps are null.");
        return;
    }

    logDebug("Neutral count: " + std::to_string(NeutralAh->Getcount()));
    logDebug("Alliance count: " + std::to_string(AllianceAh->Getcount()));
    logDebug("Horde count: " + std::to_string(HordeAh->Getcount()));

    logDebug("UpdatingEvents");
    events.Update(60000);
}

AuctionHouseObject* Auctionator::GetAuctionMgr(uint32 auctionHouseId)
{
    switch(auctionHouseId) {
        case (uint32)AuctionHouseId::Alliance:
            return AllianceAh;
            break;
        case (uint32)AuctionHouseId::Horde:
            return HordeAh;
            break;
        default:
            return NeutralAh;
            break;
    }
}

void Auctionator::ExpireAllAuctions(uint32 houseId)
{
    if (houseId != (uint32)AuctionHouseId::Alliance &&
        houseId != (uint32)AuctionHouseId::Horde &&
        houseId != (uint32)AuctionHouseId::Neutral
    ) {
        logDebug("Invalid houseId: " + std::to_string(houseId));
        return;
    }

    logDebug("Clearing auctions for houseId: " + std::to_string(houseId));
    //
    // we are going to grab our auctions map from the matching house
    // and iterate over it setting the expire_time for each auction to
    // 0. This will force the AHmgr to delete all of these auctions BUT
    // it won't happen till after the next tick of Auctionator passes
    // because our update happens before the server gets a chance to update.
    //
    AuctionHouseObject* ah = GetAuctionHouse(houseId);
    if (!ah) {
        logDebug("Unable to expire auctions for invalid houseId: " + std::to_string(houseId));
        return;
    }

    for (
        AuctionHouseObject::AuctionEntryMap::iterator itr,
        iter = ah->GetAuctionsBegin();
        iter != ah->GetAuctionsEnd();
        )
    {
        itr = iter++;
        AuctionEntry* auction = (*itr).second;
        if (!auction) {
            continue;
        }

        if (auction->houseId != AuctionHouseId::Alliance &&
            auction->houseId != AuctionHouseId::Horde &&
            auction->houseId != AuctionHouseId::Neutral) {
            continue;
        }

        logTrace("Expiring auction " + std::to_string(auction->Id) +
            " for house " + std::to_string((uint32)auction->houseId));
        auction->expire_time = 0;
    }

    logDebug("House auctions expired: " + std::to_string(houseId));
}

float Auctionator::GetQualityMultiplier(AuctionatorPriceMultiplierConfig config, uint32 quality)
{
    switch (quality) {
        case ITEM_QUALITY_POOR:
            return config.poor;
        case ITEM_QUALITY_NORMAL:
            return config.normal;
        case ITEM_QUALITY_UNCOMMON:
            return config.uncommon;
        case ITEM_QUALITY_RARE:
            return config.rare;
        case ITEM_QUALITY_EPIC:
            return config.epic;
        case ITEM_QUALITY_LEGENDARY:
            return config.legendary;
        case ITEM_QUALITY_ARTIFACT:
            return config.legendary;
        case ITEM_QUALITY_HEIRLOOM:
            return config.legendary;
        default:
            return config.normal > 0.0f ? config.normal : 1.0f;
    }
}

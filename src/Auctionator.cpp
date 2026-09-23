
#include "Log.h"
#include "Auctionator.h"
#include "Config.h"
#include "World.h"
#include "WorldSession.h"
#include "AuctionHouseMgr.h"
#include "DBCStores.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "DatabaseEnv.h"
#include "CharacterCache.h"
#include "Timer.h"
#include "AuctionatorConfig.h"
#include "AuctionatorSeller.h"
#include "AuctionatorBidder.h"
#include "AuctionatorEvents.h"
#include <algorithm>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

Auctionator::Auctionator()
{
    SetLogPrefix("[Auctionator] ");
    InitializeConfig(sConfigMgr);
    Initialize();

    ObjectGuid buyerGuid = ObjectGuid::Create<HighGuid::Player>(config->characterGuid);
    events = AuctionatorEvents(config);
    events.SetPlayerGuid(buyerGuid);

    if (!IsReady())
    {
        logError("initialization incomplete after setup; seller and bidder are disabled.");
    }
}

Auctionator::~Auctionator()
{
    //
    // The dummy session is deliberately NOT deleted here. This object is a function
    // local static, so its destructor runs after main() returned - i.e. after
    // StopDB() closed the pools - while ~WorldSession() writes to the login database
    // (account.totaltime / account.online). Shutdown() releases it while the pools are
    // still alive.
    //
    if (session)
    {
        logWarn("the dummy session was not released before shutdown; leaving it to the kernel.");
        session = nullptr;
    }

    delete config;
    config = nullptr;
}

void Auctionator::Shutdown()
{
    if (session)
    {
        delete session;
        session = nullptr;
    }

    initialized = false;
    logInfo("shutdown: dummy session released");
}

bool Auctionator::CreateAuction(AuctionatorItem newItem, CharacterDatabaseTransaction trans)
{
    if (!config || !sAuctionMgr || !sWorld)
    {
        logError("Auctionator CreateAuction failed: config/world/auction manager is not ready.");
        return false;
    }

    if (!session)
    {
        logError("Auctionator CreateAuction failed: session is not initialized.");
        return false;
    }

    if (config->characterGuid == 0)
    {
        logError("Auctionator CreateAuction failed: characterGuid is zero; system auction owner is not configured.");
        return false;
    }

    if (newItem.itemId == 0)
    {
        logError("Auctionator CreateAuction failed: itemId is zero.");
        return false;
    }

    // Item::CreateItem() calls ABORT() when the template does not exist, so an invalid
    // item id (e.g. a typo in ".auctionator add") would kill the whole worldserver.
    ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(newItem.itemId);
    if (!itemTemplate)
    {
        logError("Auctionator CreateAuction failed: no item_template entry for item " + std::to_string(newItem.itemId) + ".");
        return false;
    }

    if (newItem.stackSize == 0)
    {
        newItem.stackSize = 1;
    }

    // Never exceed what the item can actually stack to.
    uint32 const maxStackSize = std::max<uint32>(1, itemTemplate->GetMaxStackSize());
    if (newItem.stackSize > maxStackSize)
    {
        newItem.stackSize = maxStackSize;
    }

    if (newItem.houseId != (uint32)AuctionHouseId::Alliance &&
        newItem.houseId != (uint32)AuctionHouseId::Horde &&
        newItem.houseId != (uint32)AuctionHouseId::Neutral)
    {
        logError("Auctionator CreateAuction failed: invalid houseId " + std::to_string(newItem.houseId));
        return false;
    }

    //
    // Enforce the invariant the core itself enforces for player listings: on a realm with
    // shared faction houses, only Neutral is reachable by clients. An Alliance/Horde entry
    // created here would be indexed in a search partition that no client query selects
    // (AuctionHouseSearcher uses AuctionEntry::GetFactionId(), i.e. houseId, while every
    // two-side client searches Neutral), so it would be an auction nobody can see or bid on
    // while still occupying the house quota. Refusing is the only safe answer.
    //
    if (UsesSharedNeutralAuctionHouse() && newItem.houseId != (uint32)AuctionHouseId::Neutral)
    {
        logError("Auctionator CreateAuction refused: AllowTwoSide.Interaction.Auction is enabled, so houses "
            "2 (alliance) and 6 (horde) are not visible to clients; use house 7 (neutral) instead.");
        return false;
    }

    AuctionHouseObject* house = nullptr;
    AuctionHouseEntry const* entry = nullptr;
    if (!ValidateHouseState(newItem.houseId, house, entry))
    {
        logError("Auctionator CreateAuction failed: auction house data is not initialized for houseId " + std::to_string(newItem.houseId));
        return false;
    }

    if (newItem.buyout == 0 && newItem.bid == 0)
    {
        logError("Auctionator CreateAuction failed: item " + std::to_string(newItem.itemId) + " has zero buyout and bid prices.");
        return false;
    }

    // 0 means "the configured Auctionator character", whose auction mail is deleted by
    // the mail script (gold sink). Any other value is a real character that receives the
    // sale money through the normal mail flow.
    ObjectGuid const ownerGuid = ObjectGuid::Create<HighGuid::Player>(
        newItem.ownerGuid != 0 ? newItem.ownerGuid : config->characterGuid
    );

    uint32 const houseId = newItem.houseId;

    //
    // Resolve the AuctionHouse.dbc entry NOW, before anything is written.
    //
    // It is the last thing that can fail, and it used to be resolved after
    // item->SaveToDB(trans), where a failure had to delete the Item object while its INSERT
    // row was already queued on the transaction. With a caller supplied transaction (the
    // seller batches a whole run into one) that row was then committed by the caller, so
    // every such failure left behind an orphan item_instance row with no auction, no mail
    // and no owner. Resolving it here means every failure path below runs before the first
    // write, so the transaction can never be polluted.
    //
    AuctionHouseEntry const* auctionHouseEntry = sAuctionMgr->GetAuctionHouseEntryFromHouse((AuctionHouseId)houseId);
    if (!auctionHouseEntry)
    {
        logError("Auctionator CreateAuction failed: unable to resolve auction house entry for houseId " + std::to_string(houseId));
        return false;
    }

    logDebug("Creating Auction for item: " + std::to_string(newItem.itemId)
        + " owned by " + std::to_string(ownerGuid.GetCounter()));

    //
    // The item is created WITHOUT an owner object (nullptr) and then stamped with the owner
    // guid by hand. That is deliberate:
    //
    //   * A temporary Player used to be the container. Player's constructor and destructor
    //     call Increase/DecreasePlayerCount(), and the increase also raises _maxPlayerCount,
    //     which the core publishes as the realm's "max players" - i.e. stocking the auction
    //     house inflated a server statistic. ~Player() also fired the OnDestructPlayer()
    //     hook for a player that never existed.
    //   * With a container, Item::CreateItem() ends in SetItemRandomProperties() ->
    //     SetState(ITEM_CHANGED, GetOwner()); for an owner that is currently online,
    //     GetOwner() finds that real player and queues the brand new item in *their* item
    //     update queue, which had to be undone again (RemoveFromUpdateQueueOf).
    //   * With no owner object, GetOwner() is null while the random properties are applied,
    //     so nothing is ever queued and no queue surgery is needed. The two fields the core
    //     fills from the owner are set below instead - exactly the values the container
    //     Player produced (ObjectAccessor::FindPlayer(ObjectGuid::Empty) is a plain map
    //     lookup and returns nullptr, so the empty guid is safe here).
    //
    Item* item = Item::CreateItem(newItem.itemId, 1, nullptr);
    if (!item)
    {
        logError("Auctionator CreateAuction failed: unable to create item " + std::to_string(newItem.itemId));
        return false;
    }

    item->SetGuidValue(ITEM_FIELD_OWNER, ownerGuid);
    item->SetGuidValue(ITEM_FIELD_CONTAINED, ownerGuid);

    // set our quantity. If this is a stack it needs to get set here
    // on the item instance and not on the auction item.
    item->SetCount(newItem.stackSize > 1 ? newItem.stackSize : 1);

    // The item GUID comes from the generator during creation.
    if (item->GetGUID().IsEmpty())
    {
        logError("Auctionator CreateAuction failed: item GUID was invalid for item " + std::to_string(newItem.itemId));
        item->RemoveFromWorld();
        delete item;
        return false;
    }

    // Either the caller's transaction (batched seller run) or one of our own.
    bool const ownTransaction = !trans;
    if (ownTransaction) {
        logTrace("starting character transaction for AH item");
        trans = CharacterDatabase.BeginTransaction();
    }

    AuctionEntry* auctionEntry = new AuctionEntry();
    auctionEntry->Id = sObjectMgr->GenerateAuctionID();
    auctionEntry->houseId = (AuctionHouseId)houseId;
    auctionEntry->item_guid = item->GetGUID();
    auctionEntry->item_template = item->GetEntry();
    auctionEntry->itemCount = newItem.stackSize;
    auctionEntry->owner = ownerGuid;
    auctionEntry->startbid = newItem.bid;
    auctionEntry->buyout = newItem.buyout;
    auctionEntry->bid = 0;
    auctionEntry->bidder = ObjectGuid::Empty;
    // No deposit: the mod never charges one, and SendAuctionSuccessfulMail() pays
    // "bid + deposit - cut" to the owner, so a non-zero deposit would mint gold.
    auctionEntry->deposit = 0;
    auctionEntry->expire_time = (time_t)newItem.time + time(nullptr);
    auctionEntry->auctionHouseEntry = auctionHouseEntry;

    item->SaveToDB(trans);

    //
    // Add the Item we are auctioning to the managers item list. This is NOT faction
    // specific, it's a global list shared by all factions.
    //
    sAuctionMgr->AddAItem(item);

    //
    // Add the AuctionEntry to the correct auction house. AddAuction() also feeds the
    // search cache, which is why the item has to be in the manager's list first (the
    // searcher silently drops an auction whose item it cannot resolve).
    //
    GetAuctionHouse(houseId)->AddAuction(auctionEntry);

    auctionEntry->SaveToDB(trans);

    // Only close a transaction we opened ourselves; a batched caller commits the whole run.
    if (ownTransaction) {
        CharacterDatabase.CommitTransaction(trans);
    }

    return true;
}

/**
 * Use this to get access to the AuctionHouseObject pointer for a
 * specific auction house. Ultimately this is just a global singleton.
 */
AuctionHouseObject* Auctionator::GetAuctionHouse(uint32 houseId) {
    switch(houseId) {
        case((uint32)AuctionHouseId::Alliance):
            return AllianceAh;
        case((uint32)AuctionHouseId::Horde):
            return HordeAh;
        default:
            return NeutralAh;
    }
}

uint32 Auctionator::CountAuctions(uint32 houseId)
{
    AuctionHouseObject* house = nullptr;
    AuctionHouseEntry const* entry = nullptr;
    if (!ValidateHouseState(houseId, house, entry))
    {
        return 0;
    }

    // Counted by AuctionEntry::houseId on purpose: with
    // CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION the core returns the same object for all
    // three houses, so Getcount() would report a combined total for every house.
    uint32 count = 0;
    for (auto const& itr : house->GetAuctions())
    {
        if (itr.second && (uint32)itr.second->houseId == houseId)
        {
            ++count;
        }
    }

    return count;
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

bool Auctionator::UsesSharedNeutralAuctionHouse()
{
    return sWorld != nullptr && sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION);
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

    //
    // The account name handed to the dummy session must be the real one. The core builds
    // its own sessions with the authenticated account name (WorldSocket.cpp) and only ever
    // stores this value, but a made up name makes every log line and any future check that
    // reads it wrong, so it is read from the login database instead of hardcoded.
    //
    std::string accountName = "Auctionator";
    if (QueryResult accountResult = LoginDatabase.Query("SELECT username FROM account WHERE id = {}", config->characterId))
    {
        accountName = accountResult->Fetch()[0].Get<std::string>();
    }
    else
    {
        logWarn("no `account` row with id " + std::to_string(config->characterId)
            + "; the dummy session is created with a placeholder account name.");
    }

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
        static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION)),
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

    ValidateConfiguredCharacter();
}

void Auctionator::ValidateConfiguredCharacter()
{
    if (!config) {
        return;
    }

    //
    // Both configured identities fail silently at runtime when they are wrong, so they are
    // checked once here (this runs from the world startup hook, after the character cache
    // and both database pools are up):
    //
    //   * CharacterGuid - the owner of every bot listing. The core's
    //     SendAuctionSuccessfulMail()/SendAuctionExpiredMail() only send anything when the
    //     owner exists in the character cache, so a wrong guid means sold auctions pay
    //     nobody and expired listings drop their item without a word.
    //   * CharacterId - the account id handed to the dummy WorldSession (RBAC permissions,
    //     and written back as totaltime/online when that session is destroyed), so it has
    //     to be a real account row.
    //
    if (config->characterGuid != 0
        && !sCharacterCache->GetCharacterCacheByGuid(ObjectGuid::Create<HighGuid::Player>(config->characterGuid)))
    {
        logError("Auctionator.CharacterGuid = " + std::to_string(config->characterGuid)
            + " does not exist in the characters database. Auction sale/expiry mail for that owner is silently "
              "dropped by the core (no gold, and the items are lost), so create the character or fix the setting.");
    }

    if (config->characterId != 0)
    {
        QueryResult result = LoginDatabase.Query("SELECT 1 FROM account WHERE id = {}", config->characterId);
        if (!result)
        {
            logWarn("Auctionator.CharacterId = " + std::to_string(config->characterId)
                + " is not a row in the login database `account` table. It is used as the dummy WorldSession's "
                  "account id, so it should be the account that owns Auctionator.CharacterGuid.");
        }
    }
}

void Auctionator::InitializeConfig(ConfigMgr* configMgr)
{
    logInfo("Initializing Auctionator Config");

    if (!config) {
        config = new AuctionatorConfig();
    }

    config->isEnabled = configMgr->GetOption<bool>("Auctionator.Enabled", false);
    config->characterId = configMgr->GetOption<uint32>("Auctionator.CharacterId", 0);
    config->characterGuid = configMgr->GetOption<uint32>("Auctionator.CharacterGuid", 0);

    logInfo("config->isEnabled: " + std::to_string(config->isEnabled)
        + ", CharacterIds: " + std::to_string(config->characterId)
        + "::" + std::to_string(config->characterGuid));

    if (config->characterId == 0 || config->characterGuid == 0) {
        logError("Auctionator requires a valid CharacterId and CharacterGuid. Seller and bidder features stay disabled until configured.");
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

    config->sellerConfig.auctionsPerRun = configMgr->GetOption<uint32>("Auctionator.Seller.AuctionsPerRun", 100);
    config->sellerConfig.defaultPrice = configMgr->GetOption<uint32>("Auctionator.Seller.DefaultPrice", 1000000);
    config->sellerConfig.randomizeStackSize = configMgr->GetOption<uint32>("Auctionator.Seller.RandomizeStackSize", 1);
    // The fallback default, the struct default and conf/mod_auctionator.conf.dist must all
    // agree: a missing key used to fall back to 1.0, which let the start bid be drawn as
    // 0 copper (a start bid of 0 lets any player take the item for 1 copper, because the
    // core only rejects a bid below auction->startbid).
    config->sellerConfig.bidStartModifier = std::clamp(
        configMgr->GetOption<float>("Auctionator.Seller.BidStartModifier", 0.3f),
        0.0f,
        1.0f
    );

    config->marketDataMaxAgeDays = configMgr->GetOption<uint32>("Auctionator.MarketData.MaxAgeDays", 14);
    config->marketDataImportFile = configMgr->GetOption<std::string>("Auctionator.MarketData.ImportFile", "");
    config->marketDataImportSource = configMgr->GetOption<std::string>("Auctionator.MarketData.ImportSource", "file");
    config->marketDataImportIntervalMinutes = std::max<uint32>(
        5,
        configMgr->GetOption<uint32>("Auctionator.MarketData.ImportIntervalMinutes", 360)
    );
    config->marketDataImportMaxRows = std::max<uint32>(
        1,
        configMgr->GetOption<uint32>("Auctionator.MarketData.ImportMaxRows", 50000)
    );
    config->marketDataRetentionDays = configMgr->GetOption<uint32>("Auctionator.MarketData.RetentionDays", 30);
    config->sellerConfig.preferMarketItems = configMgr->GetOption<uint32>("Auctionator.Seller.PreferMarketItems", 1);
    config->sellerConfig.excludeUnverifiedItems = configMgr->GetOption<uint32>("Auctionator.Seller.ExcludeUnverifiedItems", 0);
    config->sellerConfig.minPriceModifier = std::max(
        0.0f,
        configMgr->GetOption<float>("Auctionator.Seller.MinPriceModifier", 1.0f)
    );
    config->sellerConfig.maxPriceModifier = std::max(
        0.0f,
        configMgr->GetOption<float>("Auctionator.Seller.MaxPriceModifier", 2.0f)
    );

    //
    // Every cycle length is clamped to at least one minute: EventMap hands an event back
    // for as long as its due time is <= the map's clock, so a zero interval would make
    // AuctionatorEvents::ExecuteEvents() re-dispatch the same event forever and hang the
    // world thread.
    //
    config->allianceBidder.enabled = configMgr->GetOption<uint32>("Auctionator.AllianceBidder.Enabled", 0);
    config->allianceBidder.cycleMinutes = std::max<uint32>(1, configMgr->GetOption<uint32>("Auctionator.AllianceBidder.CycleMinutes", 30));
    config->allianceBidder.maxPerCycle = std::min<uint32>(configMgr->GetOption<uint32>("Auctionator.AllianceBidder.MaxPerCycle", 1), MaxBidderPurchasesPerCycle);

    config->hordeBidder.enabled = configMgr->GetOption<uint32>("Auctionator.HordeBidder.Enabled", 0);
    config->hordeBidder.cycleMinutes = std::max<uint32>(1, configMgr->GetOption<uint32>("Auctionator.HordeBidder.CycleMinutes", 30));
    config->hordeBidder.maxPerCycle = std::min<uint32>(configMgr->GetOption<uint32>("Auctionator.HordeBidder.MaxPerCycle", 1), MaxBidderPurchasesPerCycle);

    config->neutralBidder.enabled = configMgr->GetOption<uint32>("Auctionator.NeutralBidder.Enabled", 0);
    config->neutralBidder.cycleMinutes = std::max<uint32>(1, configMgr->GetOption<uint32>("Auctionator.NeutralBidder.CycleMinutes", 30));
    config->neutralBidder.maxPerCycle = std::min<uint32>(configMgr->GetOption<uint32>("Auctionator.NeutralBidder.MaxPerCycle", 1), MaxBidderPurchasesPerCycle);

    config->bidOnOwn = configMgr->GetOption<uint32>("Auctionator.Bidder.BidOnOwn", 0);

    // Load our multipliers for seller prices
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

    // Load our multipliers for bidder prices
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
    //
    // Advance the event clock by the time that really elapsed instead of assuming the
    // caller's cadence. The hook fires once per minute (AuctionHouseMgr's
    // _updateIntervalTimer), so the nominal interval is used for the very first call;
    // after that the measured delta keeps every cycle honest even if the core changes the
    // interval or a tick arrives late. A stall is capped at an hour, and the timestamp is
    // refreshed on every call - including the early returns below - so re-enabling the
    // module does not replay a burst of events for the time it was switched off.
    //
    uint32 const nowMs = getMSTime();
    uint32 const deltaMs = lastUpdateMs == 0 ? MINUTE * IN_MILLISECONDS : getMSTimeDiff(lastUpdateMs, nowMs);
    lastUpdateMs = nowMs;

    if (!config || !config->isEnabled) {
        logDebug("Auctionator update skipped: module disabled by Auctionator.Enabled.");
        return;
    }

    if (!IsReady()) {
        logWarn("Auctionator update skipped: module is not ready.");
        return;
    }

    logDebug("UpdatingEvents");
    events.Update(std::clamp<uint32>(deltaMs, 1, HOUR * IN_MILLISECONDS));
}

void Auctionator::ResyncEventSchedule()
{
    events.ResyncSchedule();
}

void Auctionator::ExpireAllAuctions(uint32 houseId, bool includePlayerAuctions)
{
    if (houseId != (uint32)AuctionHouseId::Alliance &&
        houseId != (uint32)AuctionHouseId::Horde &&
        houseId != (uint32)AuctionHouseId::Neutral
    ) {
        logDebug("Invalid houseId: " + std::to_string(houseId));
        return;
    }

    logDebug("Clearing auctions for houseId: " + std::to_string(houseId)
        + (includePlayerAuctions ? " (including player auctions)" : " (auctionator owned only)"));

    AuctionHouseObject* ah = GetAuctionHouse(houseId);
    if (!ah)
    {
        logDebug("Unable to expire auctions for invalid houseId: " + std::to_string(houseId));
        return;
    }

    //
    // Setting expire_time to 0 forces the AH manager to delete the auction on its next
    // tick, which happens after this update. By default only auctions owned by the
    // configured auctionator character are expired: cancelling a real player's auction
    // (even though the core returns their item and refunds bidders) is not this
    // command's business. The GM can still ask for every auction with the explicit "all"
    // argument.
    //
    ObjectGuid const auctionatorOwner = ObjectGuid::Create<HighGuid::Player>(config ? config->characterGuid : 0);
    uint32 expiredCount = 0;

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

        // With CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION all houses share one object, so
        // the house filter has to be explicit here.
        if (auction->houseId != (AuctionHouseId)houseId) {
            continue;
        }

        if (!includePlayerAuctions && auction->owner != auctionatorOwner) {
            continue;
        }

        logTrace("Expiring auction " + std::to_string(auction->Id) +
            " for house " + std::to_string((uint32)auction->houseId));
        auction->expire_time = 0;
        expiredCount++;
    }

    logDebug("House auctions expired: " + std::to_string(houseId)
        + " count: " + std::to_string(expiredCount));
}

float Auctionator::GetQualityMultiplier(AuctionatorPriceMultiplierConfig const& config, uint32 quality)
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

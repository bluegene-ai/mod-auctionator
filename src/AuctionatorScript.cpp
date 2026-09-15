
#include "ScriptMgr.h"
#include "WorldSession.h"
#include "Config.h"
#include "Chat.h"
#include "Auctionator.h"
#include "Player.h"

class AuctionatorWorldScript : public WorldScript
{
public:
    AuctionatorWorldScript() : WorldScript("Auctionator_WorldScript") { }

    void OnStartup() override
    {
        Auctionator* auctionator = Auctionator::getInstance();
        LOG_INFO("server.loading", "[Auctionator]: Auctionator initializing...");
        if (auctionator->config->isEnabled) {
            LOG_INFO("server.loading", "[Auctionator]: Auctionator enabled.");
        } else {
            LOG_INFO("server.loading", "[Auctionator]: Auctionator disabled.");
        }
    }
};

class AuctionatorHouseScript : public AuctionHouseScript
{
    public:
        AuctionatorHouseScript() : AuctionHouseScript("AuctionatorHouseScript") {}

        void OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(
                AuctionHouseMgr*,
                AuctionEntry*,
                Player* owner,
                uint32& /*owner_accId*/,
                uint32& /*profit*/,
                bool& sendNotification,
                bool& updateAchievementCriteria,
                bool& /*sendMail*/
            ) override
        {
            Auctionator* auctionator = Auctionator::getInstance();
            if (!auctionator || !auctionator->config) {
                return;
            }

            if (owner && owner->GetGUID().GetCounter() == auctionator->config->characterGuid)
            {
                sendNotification = false;
                updateAchievementCriteria = false;
            }
        }

        void OnBeforeAuctionHouseMgrSendAuctionExpiredMail(
                AuctionHouseMgr* ,
                AuctionEntry*,
                Player* owner,
                uint32& /*owner_accId*/,
                bool& sendNotification,
                bool& /*sendMail*/
            ) override
        {
            Auctionator* auctionator = Auctionator::getInstance();
            if (!auctionator || !auctionator->config) {
                return;
            }

            if (owner && owner->GetGUID().GetCounter() == auctionator->config->characterGuid)
                sendNotification = false;
        }

        void OnBeforeAuctionHouseMgrSendAuctionOutbiddedMail(
                AuctionHouseMgr* /*auctionHouseMgr*/,
                AuctionEntry* auction,
                Player* oldBidder,
                uint32& /*oldBidder_accId*/,
                Player* newBidder,
                uint32& newPrice,
                bool& /*sendNotification*/,
                bool& /*sendMail*/
            ) override
        {
            Auctionator* auctionator = Auctionator::getInstance();
            if (!auctionator || !auctionator->config || !auction || !oldBidder || newBidder) {
                return;
            }

            if (oldBidder->GetSession())
                oldBidder->GetSession()->SendAuctionBidderNotification(
                    (uint32)auction->GetHouseId(),
                    auction->Id,
                    ObjectGuid::Create<HighGuid::Player>(auctionator->config->characterGuid),
                    newPrice,
                    auction->GetAuctionOutBid(),
                    auction->item_template);
        }

        void OnBeforeAuctionHouseMgrUpdate() override
        {
            Auctionator::getInstance()->Update();
        }

};


class AuctionatorMailScript : public MailScript
{
public:
    AuctionatorMailScript() : MailScript("AuctionatorMailScript") { }

    void OnBeforeMailDraftSendMailTo(MailDraft* /*mailDraft*/, MailReceiver const& receiver, MailSender const& sender, MailCheckMask& /*checked*/, uint32& /*deliver_delay*/, uint32& /*custom_expiration*/, bool& deleteMailItemsFromDB, bool& sendMail) override
    {
        Auctionator* auctionator = Auctionator::getInstance();
        if (!auctionator || !auctionator->config) {
            return;
        }

        if (receiver.GetPlayerGUIDLow() != auctionator->config->characterGuid) {
            return;
        }

        if (sender.GetMailMessageType() == MAIL_AUCTION) {
            deleteMailItemsFromDB = true;
            sendMail = false;
            return;
        }
    }
};

void AddAuctionatorScripts()
{
    new AuctionatorWorldScript();
    new AuctionatorHouseScript();
    new AuctionatorMailScript();
};

/*
 * mod-loot-postmaster
 *
 * Recovers valuable loot from expiring creature corpses through in-game mail.
 */

#include "MailMgr.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "test_harness.h"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace
{
    constexpr uint32 POSTMASTER_ENTRY = 34337;
    constexpr char MAIL_SUBJECT[] = "Recovered Loot";

    std::atomic_bool _enable{true};

    struct RecoveredLoot
    {
        uint32 money = 0;
        std::vector<LootItem const*> items;
    };

    using RecoveryMap = std::map<ObjectGuid::LowType, RecoveredLoot>;

    bool CanReceiveItem(ObjectGuid recipient, LootItem const& item)
    {
        if (!recipient.IsPlayer())
            return false;

        AllowedLooterSet const& allowedLooters = item.GetAllowedLooters();
        return allowedLooters.empty() || allowedLooters.find(recipient) != allowedLooters.end();
    }

    ObjectGuid SelectItemRecipient(Creature* creature, LootItem const& item)
    {
        if (item.rollWinnerGUID.IsPlayer())
            return item.rollWinnerGUID;

        Loot const& loot = creature->loot;
        if (item.is_underthreshold && CanReceiveItem(loot.roundRobinPlayer, item))
            return loot.roundRobinPlayer;

        if (Group* group = creature->GetLootRecipientGroup())
        {
            if (!item.is_underthreshold && group->GetLootMethod() == MASTER_LOOT)
            {
                ObjectGuid masterLooter = group->GetMasterLooterGuid();
                if (CanReceiveItem(masterLooter, item))
                    return masterLooter;
            }
        }

        if (CanReceiveItem(loot.lootOwnerGUID, item))
            return loot.lootOwnerGUID;

        ObjectGuid creatureRecipient = creature->GetLootRecipientGUID();
        if (CanReceiveItem(creatureRecipient, item))
            return creatureRecipient;

        AllowedLooterSet const& allowedLooters = item.GetAllowedLooters();
        return allowedLooters.empty() ? ObjectGuid::Empty : *allowedLooters.begin();
    }

    void QueueItem(RecoveryMap& recoveries, ObjectGuid recipient, LootItem const& item)
    {
        if (!recipient.IsPlayer() || item.is_looted || !item.itemid || !item.count || item.needs_quest)
            return;

        ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(item.itemid);
        if (!itemTemplate || itemTemplate->Class == ITEM_CLASS_QUEST)
            return;

        recoveries[recipient.GetCounter()].items.push_back(&item);
    }

    void QueuePerPlayerItems(RecoveryMap& recoveries, QuestItemMap const& playerItems,
        std::vector<LootItem> const& lootItems)
    {
        for (auto const& [recipient, entries] : playerItems)
        {
            if (!entries)
                continue;

            for (QuestItem const& entry : *entries)
            {
                if (entry.is_looted || entry.index >= lootItems.size())
                    continue;

                LootItem const& item = lootItems[entry.index];
                if (item.freeforall)
                    QueueItem(recoveries, recipient, item);
            }
        }
    }

    void QueueMoney(RecoveryMap& recoveries, Creature* creature)
    {
        Loot const& loot = creature->loot;
        if (!loot.gold)
            return;

        ObjectGuid ownerGuid = loot.lootOwnerGUID;
        if (!ownerGuid.IsPlayer())
            ownerGuid = creature->GetLootRecipientGUID();
        if (!ownerGuid.IsPlayer())
            return;

        std::vector<ObjectGuid> recipients;
        if (Group* group = creature->GetLootRecipientGroup())
        {
            for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
            {
                Player* member = reference->GetSource();
                if (member && member->IsAtLootRewardDistance(creature))
                    recipients.push_back(member->GetGUID());
            }
        }

        if (recipients.empty())
            recipients.push_back(ownerGuid);

        uint32 share = loot.gold / recipients.size();
        uint32 remainder = loot.gold % recipients.size();
        for (ObjectGuid recipient : recipients)
        {
            recoveries[recipient.GetCounter()].money += share;
            if (remainder)
            {
                ++recoveries[recipient.GetCounter()].money;
                --remainder;
            }
        }
    }

    void QueueItems(RecoveryMap& recoveries, Creature* creature)
    {
        Loot const& loot = creature->loot;

        for (LootItem const& item : loot.items)
        {
            if (!item.freeforall)
                QueueItem(recoveries, SelectItemRecipient(creature, item), item);
        }

        QueuePerPlayerItems(recoveries, loot.GetPlayerFFAItems(), loot.items);
    }

    void SendRecoveries(Creature* creature, RecoveryMap const& recoveries)
    {
        if (recoveries.empty())
            return;

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        uint32 sentMailCount = 0;
        uint32 sentItemCount = 0;
        uint64 sentMoney = 0;

        for (auto const& [recipientLow, recovery] : recoveries)
        {
            ObjectGuid recipientGuid(HighGuid::Player, recipientLow);
            Player* recipient = ObjectAccessor::FindConnectedPlayer(recipientGuid);
            std::size_t itemOffset = 0;
            bool moneyPending = recovery.money != 0;

            while (moneyPending || itemOffset < recovery.items.size())
            {
                MailDraft draft(MAIL_SUBJECT, "");
                bool hasContents = false;
                if (moneyPending)
                {
                    draft.AddMoney(recovery.money);
                    sentMoney += recovery.money;
                    moneyPending = false;
                    hasContents = true;
                }

                uint8 attachmentCount = 0;
                while (itemOffset < recovery.items.size() && attachmentCount < MAX_MAIL_ITEMS)
                {
                    LootItem const& lootItem = *recovery.items[itemOffset++];
                    Item* item = Item::CreateItem(lootItem.itemid, lootItem.count, recipient,
                        false, lootItem.randomPropertyId, false, lootItem.randomSuffix);
                    if (!item)
                    {
                        LOG_ERROR("module.loot-postmaster",
                            "Failed to recover item {} x{} for player {} from creature {}",
                            lootItem.itemid, lootItem.count, recipientLow, creature->GetEntry());
                        continue;
                    }

                    item->SaveToDB(transaction);
                    draft.AddItem(item);
                    ++attachmentCount;
                    ++sentItemCount;
                    hasContents = true;
                }

                if (hasContents)
                {
                    draft.SendMailTo(transaction, MailReceiver(recipient, recipientLow),
                        MailSender(MAIL_CREATURE, POSTMASTER_ENTRY), MAIL_CHECK_MASK_COPIED);
                    ++sentMailCount;
                }
            }
        }

        if (!sentMailCount)
            return;

        CharacterDatabase.CommitTransaction(transaction);
        LOG_INFO("module.loot-postmaster",
            "Recovered {} item stacks and {} copper from creature {} into {} mail messages",
            sentItemCount, sentMoney, creature->GetEntry(), sentMailCount);
    }

    class LootPostmasterTestSuite final : public TestHarness::Suite
    {
    public:
        void Start(TestHarness::Context& context) override
        {
            Player* actor = context.GetActor();
            context.Expect(actor != nullptr, "headless actor available");
            if (!actor)
            {
                context.Finish();
                return;
            }

            _previousEnable = _enable.exchange(true, std::memory_order_relaxed);
            _initialMailCount = actor->GetMailSize();
            if (QueryResult result = CharacterDatabase.Query("SELECT COALESCE(MAX(id), 0) FROM mail"))
                _latestMailId = (*result)[0].Get<uint32>();

            Creature* corpse = context.SpawnDummy();
            context.Expect(corpse != nullptr, "postmaster test creature spawns");
            if (!corpse)
            {
                Cleanup(context, true);
                return;
            }

            corpse->SetLootRecipient(actor, false);
            context.Expect(context.Damage(corpse->GetGUID(), corpse->GetHealth()),
                "postmaster test creature dies");
            context.Expect(corpse->getDeathState() == DeathState::Corpse,
                "postmaster test creature becomes a corpse");
            if (corpse->getDeathState() != DeathState::Corpse)
            {
                Cleanup(context, true);
                return;
            }

            corpse->loot.clear();
            corpse->loot.lootOwnerGUID = actor->GetGUID();
            corpse->loot.gold = 37;
            AddLootItem(corpse->loot.items, 1376, 1);
            AddLootItem(corpse->loot.items, 2770, 2);
            AddLootItem(corpse->loot.items, 182, 1);
            AddLootItem(corpse->loot.quest_items, 2447, 1, true);
            corpse->RemoveCorpse(false);

            _elapsed = 0;
            _waitingForMail = true;
        }

        void Update(TestHarness::Context& context, uint32 diff) override
        {
            if (!_waitingForMail)
                return;

            _elapsed += diff;
            if (_elapsed < 100)
                return;

            Player* actor = context.GetActor();
            if (!actor)
            {
                context.Fail("headless actor remains available");
                Cleanup(context, true);
                return;
            }

            QueryResult result = CharacterDatabase.Query(
                "SELECT m.id, m.money, m.messageType, m.checked, m.body, "
                "COUNT(mi.item_guid), "
                "SUM(ii.itemEntry = 1376 AND ii.count = 1), "
                "SUM(ii.itemEntry = 2770 AND ii.count = 2), "
                "SUM(ii.itemEntry IN (182, 2447)) "
                "FROM mail m "
                "JOIN mail_items mi ON mi.mail_id = m.id "
                "JOIN item_instance ii ON ii.guid = mi.item_guid "
                "WHERE m.receiver = {} AND m.id > {} AND m.sender = {} "
                "AND m.subject = '{}' GROUP BY m.id, m.money, m.messageType, "
                "m.checked, m.body ORDER BY m.id DESC LIMIT 1",
                actor->GetGUID().GetCounter(), _latestMailId, POSTMASTER_ENTRY, MAIL_SUBJECT);
            if (!result)
            {
                if (_elapsed < 5000)
                    return;
                context.Fail("recovered-loot mail reaches the character database");
                Cleanup(context, true);
                return;
            }

            Field* fields = result->Fetch();
            _mailId = fields[0].Get<uint32>();
            uint32 money = fields[1].Get<uint32>();
            uint8 messageType = fields[2].Get<uint8>();
            uint8 checked = fields[3].Get<uint8>();
            std::string body = fields[4].Get<std::string>();
            uint32 itemCount = fields[5].Get<uint32>();
            bool poorItemRecovered = fields[6].Get<uint64>() == 1;
            bool commonItemRecovered = fields[7].Get<uint64>() == 1;
            bool questItemRecovered = fields[8].Get<uint64>() != 0;
            Mail* mail = actor->GetMail(_mailId);
            context.Expect(actor->GetMailSize() == _initialMailCount + 1
                    && mail && mail->money == 37 && mail->items.size() == 2,
                "corpse removal delivers one recovered-loot mail");
            context.Expect(money == 37 && itemCount == 2
                    && poorItemRecovered && commonItemRecovered,
                "postmaster preserves money, item quality, and stack count",
                "money=" + std::to_string(money)
                    + ",items=" + std::to_string(itemCount)
                    + ",poor=" + std::to_string(poorItemRecovered)
                    + ",common=" + std::to_string(commonItemRecovered));
            context.Expect(!questItemRecovered,
                "postmaster excludes quest-bound and quest-class items");
            context.Expect(messageType == MAIL_CREATURE && checked == MAIL_CHECK_MASK_COPIED
                    && body.empty() && mail && mail->body.empty()
                    && mail->checked == MAIL_CHECK_MASK_COPIED,
                "postmaster mail has auction-style attachment-only presentation");
            Cleanup(context, true);
        }

        void Cancel(TestHarness::Context& context) override
        {
            Cleanup(context, false);
        }

    private:
        static void AddLootItem(std::vector<LootItem>& items, uint32 itemId, uint8 count,
            bool needsQuest = false)
        {
            LootItem item{};
            item.itemid = itemId;
            item.itemIndex = items.size();
            item.count = count;
            item.is_underthreshold = true;
            item.needs_quest = needsQuest;
            items.push_back(item);
        }

        void Cleanup(TestHarness::Context& context, bool finish)
        {
            _enable.store(_previousEnable, std::memory_order_relaxed);
            _waitingForMail = false;

            if (Player* actor = context.GetActor())
            {
                if (_mailId)
                {
                    Mail* mail = actor->GetMail(_mailId);
                    std::vector<MailItemInfo> attachments = mail ? mail->items
                        : std::vector<MailItemInfo>{};
                    CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
                    for (MailItemInfo const& attachment : attachments)
                    {
                        Item::DeleteFromDB(transaction, attachment.item_guid);
                        CharacterDatabasePreparedStatement* itemStatement =
                            CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM);
                        itemStatement->SetData(0, attachment.item_guid);
                        transaction->Append(itemStatement);
                        if (Item* item = actor->GetMItem(attachment.item_guid))
                        {
                            actor->RemoveMItem(attachment.item_guid);
                            delete item;
                        }
                    }

                    CharacterDatabasePreparedStatement* mailStatement =
                        CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
                    mailStatement->SetData(0, _mailId);
                    transaction->Append(mailStatement);
                    CharacterDatabase.CommitTransaction(transaction);
                    actor->RemoveMail(_mailId);
                    sMailMgr->OnMailDeleted(actor->GetGUID().GetCounter());
                    actor->UpdateNextMailTimeAndUnreads();
                }
            }

            context.DespawnAllDummies();
            if (finish)
                context.Finish();
        }

        uint32 _latestMailId = 0;
        uint32 _mailId = 0;
        uint32 _initialMailCount = 0;
        uint32 _elapsed = 0;
        bool _previousEnable = true;
        bool _waitingForMail = false;
    };
}

class LootPostmasterWorld final : public WorldScript
{
public:
    LootPostmasterWorld() : WorldScript("LootPostmasterWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        _enable.store(sConfigMgr->GetOption<bool>("LootPostmaster.Enable", true),
            std::memory_order_relaxed);
    }
};

class LootPostmasterCreature final : public AllCreatureScript
{
public:
    LootPostmasterCreature() : AllCreatureScript("LootPostmasterCreature") { }

    void OnBeforeCreatureRemoveCorpse(Creature* creature) override
    {
        if (!_enable.load(std::memory_order_relaxed) || !creature)
            return;

        RecoveryMap recoveries;
        QueueMoney(recoveries, creature);
        QueueItems(recoveries, creature);
        SendRecoveries(creature, recoveries);
    }
};

void AddSC_loot_postmaster()
{
    TestHarness::RegisterSuite("loot-postmaster",
        [] { return std::make_unique<LootPostmasterTestSuite>(); });
    new LootPostmasterWorld();
    new LootPostmasterCreature();
}

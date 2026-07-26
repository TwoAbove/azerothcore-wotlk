/*
 * mod-easy-gathering — Earthcaller gathering guardians.
 */

#include "Creature.h"
#include "Config.h"
#include "CreatureAI.h"
#include "DataMap.h"
#include "GameObject.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "TemporarySummon.h"
#include "test_harness.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>

namespace
{
constexpr char EARTHCALLER_NODE_KEY[] = "mod-easy-gathering-earthcaller";
constexpr uint32 EARTHCALLER_GUARDIAN_ENTRY_BASE = 900000;
constexpr uint32 EARTHCALLER_LIFETIME_MS = 5 * MINUTE * IN_MILLISECONDS;
constexpr uint32 EARTHCALLER_TEST_ITEM = 2770; // Copper Ore
constexpr uint32 EARTHCALLER_TEST_LOOT_ID = 1502; // Copper Vein
constexpr uint32 EARTHCALLER_TEST_QUEST = 255; // Mercenaries
constexpr uint32 EARTHCALLER_TEST_QUEST_TARGET = 1178; // Mo'grosh Ogre

struct EarthcallerSettings
{
    bool enable = true;
    uint32 chance = 10;
};

EarthcallerSettings _earthcallerSettings;

struct EarthcallerNode : public DataMap::Base
{
    std::unordered_set<ObjectGuid> attemptedBy;
};

bool IsGatheringNode(GameObject const* gameObject)
{
    if (!gameObject || gameObject->GetGoType() != GAMEOBJECT_TYPE_CHEST)
        return false;

    LockEntry const* lock = sLockStore.LookupEntry(gameObject->GetGOInfo()->GetLockId());
    if (!lock)
        return false;

    for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
        if (lock->Type[i] == LOCK_KEY_SKILL
            && (lock->Index[i] == LOCKTYPE_HERBALISM || lock->Index[i] == LOCKTYPE_MINING))
            return true;

    return false;
}

bool IsNodeMaterial(ItemTemplate const* itemTemplate)
{
    return itemTemplate && itemTemplate->Class == ITEM_CLASS_TRADE_GOODS
        && (itemTemplate->SubClass == ITEM_SUBCLASS_METAL_STONE
            || itemTemplate->SubClass == ITEM_SUBCLASS_HERB);
}

TempSummon* TriggerEarthcaller(Player* player, uint32 nodeLootId, bool forced = false)
{
    if (!_earthcallerSettings.enable || !player || !player->IsInWorld() || !player->GetMap()
        || !nodeLootId || (!forced && !roll_chance_i(_earthcallerSettings.chance)))
        return nullptr;

    uint32 const guardianEntry = EARTHCALLER_GUARDIAN_ENTRY_BASE + nodeLootId;
    if (!sObjectMgr->GetCreatureTemplate(guardianEntry))
    {
        LOG_ERROR("module", "EasyGathering: no Earthcaller guardian template for node loot {}", nodeLootId);
        return nullptr;
    }

    Position position = player->GetNearPosition(3.0f, 0.0f);
    position.SetOrientation(position.GetAbsoluteAngle(player));
    TempSummon* guardian = player->SummonCreature(guardianEntry, position,
        TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, EARTHCALLER_LIFETIME_MS);
    if (!guardian)
        return nullptr;

    guardian->SelectLevel(true, player->GetLevel());
    guardian->SetFaction(14);
    guardian->SetPhaseMask(player->GetPhaseMask(), true);
    guardian->SetReactState(REACT_AGGRESSIVE);
    guardian->SetReputationRewardDisabled(true);

    // Use level-matched combat stats, then replace only the ordinary health
    // pool with a short solo encounter. Keep the reward threshold in sync.
    uint32 const health = std::max<uint32>(50, player->GetMaxHealth() / 3);
    guardian->SetMaxHealth(health);
    guardian->SetHealth(health);
    guardian->ResetPlayerDamageReq();

    guardian->SetInCombatWith(player);
    player->SetInCombatWith(guardian);
    guardian->GetThreatMgr().AddThreat(player, 1.0f, nullptr, true, true);
    if (guardian->IsAIEnabled)
        guardian->AI()->AttackStart(player);

    return guardian;
}

class EarthcallerWorld final : public WorldScript
{
public:
    EarthcallerWorld() : WorldScript("EarthcallerWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        _earthcallerSettings.enable = sConfigMgr->GetOption<bool>("EasyGathering.Earthcaller.Enable", true);
        _earthcallerSettings.chance = std::min<uint32>(100,
            sConfigMgr->GetOption<uint32>("EasyGathering.Earthcaller.Chance", 10));
    }
};

class EarthcallerPlayer final : public PlayerScript
{
public:
    EarthcallerPlayer() : PlayerScript("EarthcallerPlayer", { PLAYERHOOK_ON_LOOT_ITEM }) { }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootGuid) override
    {
        if (!_earthcallerSettings.enable || !player || !item || !count || !IsNodeMaterial(item->GetTemplate()))
            return;

        GameObject* source = ObjectAccessor::GetGameObject(*player, lootGuid);
        if (!IsGatheringNode(source))
            return;

        EarthcallerNode* node = source->CustomData.GetDefault<EarthcallerNode>(EARTHCALLER_NODE_KEY);
        if (!node->attemptedBy.insert(player->GetGUID()).second)
            return;

        TriggerEarthcaller(player, source->GetGOInfo()->GetLootId());
    }
};

class EarthcallerGameObject final : public AllGameObjectScript
{
public:
    EarthcallerGameObject() : AllGameObjectScript("EarthcallerGameObject") { }

    void OnGameObjectLootStateChanged(GameObject* gameObject, uint32 state, Unit* /*unit*/) override
    {
        // Multi-tap returns an active node to READY in its earlier callback.
        if (state == GO_JUST_DEACTIVATED && gameObject->getLootState() == GO_JUST_DEACTIVATED)
            gameObject->CustomData.Erase(EARTHCALLER_NODE_KEY);
    }
};

class EarthcallerTestSuite final : public TestHarness::Suite
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

        _stage = Stage::KillGuardian;

        actor->RemoveRewardedQuest(EARTHCALLER_TEST_QUEST, false);
        actor->RemoveActiveQuest(EARTHCALLER_TEST_QUEST, false);
        if (uint16 slot = actor->FindQuestSlot(EARTHCALLER_TEST_QUEST); slot < MAX_QUEST_LOG_SIZE)
            actor->SetQuestSlot(slot, 0);

        Quest const* quest = sObjectMgr->GetQuestTemplate(EARTHCALLER_TEST_QUEST);
        context.Expect(quest != nullptr, "quest-credit probe resolves its quest");
        if (quest)
        {
            actor->AddQuestAndCheckCompletion(quest, nullptr);
            Creature* questTarget = context.SpawnDummy(1.0f, 0.0f, EARTHCALLER_TEST_QUEST_TARGET);
            context.Expect(questTarget != nullptr, "quest-credit probe spawns its target");
            if (questTarget)
            {
                questTarget->SetMaxHealth(1);
                questTarget->SetHealth(1);
                questTarget->ResetPlayerDamageReq();
                int32 damage = 1;
                actor->CastCustomSpell(questTarget, 82006, &damage, nullptr, nullptr, true);
                context.Expect(!questTarget->IsAlive(),
                    "ordinary spell damage kills the live quest target");
                context.Expect(actor->GetReqKillOrCastCurrentCount(
                        EARTHCALLER_TEST_QUEST, EARTHCALLER_TEST_QUEST_TARGET) == 1,
                    "ordinary spell kill advances the live quest credit");
            }
        }

        actor->CombatStop(true);

        _initialMoney = actor->GetMoney();
        TempSummon* guardian = TriggerEarthcaller(actor, EARTHCALLER_TEST_LOOT_ID, true);
        context.Expect(guardian != nullptr, "forced gather wakes an Earthcaller guardian");
        if (!guardian)
        {
            Cleanup(context, true);
            return;
        }

        _guardianGuid = guardian->GetGUID();
        context.Expect(guardian->IsHostileTo(actor) && actor->IsHostileTo(guardian),
            "guardian is hostile to the gatherer");
        context.Expect(guardian->IsInCombatWith(actor) && actor->IsInCombatWith(guardian)
                && guardian->GetVictim() == actor,
            "guardian immediately attacks the gatherer");
        context.Expect(guardian->GetLevel() == actor->GetLevel(),
            "guardian matches the gatherer's level");
        float guardianMaxDamage = guardian->GetFloatValue(UNIT_FIELD_MAXDAMAGE);
        context.Expect(guardianMaxDamage * 2.0f < float(actor->GetMaxHealth()),
            "guardian cannot one-shot the level-matched gatherer",
            "maxMelee=" + std::to_string(guardianMaxDamage)
                + ",gathererHealth=" + std::to_string(actor->GetMaxHealth()));
        context.Expect(guardian->GetPlayerDamageReq() == guardian->GetHealth() / 2,
            "guardian reward threshold matches its custom health",
            "required=" + std::to_string(guardian->GetPlayerDamageReq())
                + ",health=" + std::to_string(guardian->GetHealth()));
        context.Expect(guardian->GetEntry() == EARTHCALLER_GUARDIAN_ENTRY_BASE + EARTHCALLER_TEST_LOOT_ID
                && guardian->GetLootMode() == LOOT_MODE_DEFAULT
                && !guardian->IsLootRewardDisabled(),
            "guardian uses its node-specific enriched reward path");

        _elapsed = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
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

        if (_stage == Stage::KillGuardian)
        {
            Creature* guardian = context.GetCreature(_guardianGuid);
            context.Expect(guardian && guardian->IsAlive(), "guardian remains available for combat");
            if (!guardian)
            {
                Cleanup(context, true);
                return;
            }

            _killMoney = actor->GetMoney();
            context.Expect(context.Damage(_guardianGuid, guardian->GetHealth()),
                "gatherer kills the guardian");
            _stage = Stage::CheckReward;
            _elapsed = 0;
            return;
        }

        if (_stage == Stage::CheckReward)
        {
            Creature* corpse = context.GetCreature(_guardianGuid);
            bool nativeLoot = corpse && corpse->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE)
                && corpse->loot.gold == 0 && corpse->loot.items.size() == 1
                && corpse->loot.items.front().itemid != EARTHCALLER_TEST_ITEM
                && corpse->loot.items.front().count >= 1;
            context.Expect(nativeLoot, "guardian corpse contains one enriched node side-drop",
                "lootable=" + std::to_string(corpse && corpse->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
                    + ",items=" + std::to_string(corpse ? corpse->loot.items.size() : 0)
                    + ",item=" + std::to_string(corpse && !corpse->loot.items.empty()
                        ? corpse->loot.items.front().itemid : 0));

            if (nativeLoot)
            {
                LootItem const& lootItem = corpse->loot.items.front();
                _lootedItemId = lootItem.itemid;
                _initialLootedItemCount = actor->GetItemCount(_lootedItemId, false);
                uint8 const count = lootItem.count;
                uint8 const slot = uint8(lootItem.itemIndex);
                InventoryResult result = EQUIP_ERR_OK;
                LootItem* stored = actor->StoreLootItem(slot, &corpse->loot, result);
                context.Expect(stored && result == EQUIP_ERR_OK
                        && actor->GetItemCount(_lootedItemId, false) == _initialLootedItemCount + count,
                    "gatherer loots the enriched reward through AzerothCore's corpse-loot path");
            }

            context.Expect(actor->GetMoney() == _killMoney && actor->GetMoney() == _initialMoney,
                "guardian awards no money");
            Cleanup(context, true);
        }
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context, false);
    }

private:
    enum class Stage
    {
        KillGuardian,
        CheckReward,
        Done
    };

    void Cleanup(TestHarness::Context& context, bool finish)
    {
        if (_stage == Stage::Done)
            return;

        if (Player* actor = context.GetActor())
        {
            if (_guardianGuid)
                if (Creature* guardian = ObjectAccessor::GetCreature(*actor, _guardianGuid))
                    guardian->DespawnOrUnsummon();

            if (_lootedItemId)
            {
                uint32 const currentCount = actor->GetItemCount(_lootedItemId, false);
                if (currentCount > _initialLootedItemCount)
                    actor->DestroyItemCount(_lootedItemId, currentCount - _initialLootedItemCount, true);
            }

            actor->AbandonQuest(EARTHCALLER_TEST_QUEST);
            actor->RemoveActiveQuest(EARTHCALLER_TEST_QUEST, false);
            actor->RemoveRewardedQuest(EARTHCALLER_TEST_QUEST, false);
            if (uint16 slot = actor->FindQuestSlot(EARTHCALLER_TEST_QUEST); slot < MAX_QUEST_LOG_SIZE)
                actor->SetQuestSlot(slot, 0);
            actor->CombatStop(true);
        }

        _guardianGuid.Clear();
        _stage = Stage::Done;
        context.DespawnAllDummies();
        if (finish)
            context.Finish();
    }

    Stage _stage = Stage::Done;
    ObjectGuid _guardianGuid;
    uint32 _lootedItemId = 0;
    uint32 _initialLootedItemCount = 0;
    uint32 _elapsed = 0;
    uint32 _initialMoney = 0;
    uint32 _killMoney = 0;
};
} // namespace

void AddSC_easy_gathering_earthcaller()
{
    TestHarness::RegisterSuite("earthcaller",
        [] { return std::make_unique<EarthcallerTestSuite>(); });
    new EarthcallerWorld();
    new EarthcallerPlayer();
    new EarthcallerGameObject();
}

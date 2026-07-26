/*
 * mod-open-tap
 *
 * Every player who deals damage to a creature receives quest kill credit when
 * it dies, independent of the creature's first-tap owner. Loot ownership and
 * XP/reputation rewards remain unchanged.
 */

#include "Config.h"
#include "Creature.h"
#include "DataMap.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <atomic>
#include <unordered_set>

namespace
{
    constexpr char PARTICIPANTS_KEY[] = "mod-open-tap-participants";

    std::atomic_bool _enable{true};

    struct OpenTapParticipants : public DataMap::Base
    {
        std::unordered_set<ObjectGuid> players;
    };
}

class OpenTapWorld : public WorldScript
{
public:
    OpenTapWorld() : WorldScript("OpenTapWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        _enable.store(sConfigMgr->GetOption<bool>("OpenTap.Enable", true), std::memory_order_relaxed);
    }
};

class OpenTapUnit : public UnitScript
{
public:
    OpenTapUnit() : UnitScript("OpenTapUnit", true,
        { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_UNIT_ENTER_EVADE_MODE, UNITHOOK_ON_UNIT_DEATH }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!_enable.load(std::memory_order_relaxed) || !damage || !attacker || !victim)
            return;

        Creature* creature = victim->ToCreature();
        Player* player = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!creature || !player || creature->IsControlledByPlayer())
            return;

        creature->CustomData.GetDefault<OpenTapParticipants>(PARTICIPANTS_KEY)->players.insert(player->GetGUID());
    }

    void OnUnitEnterEvadeMode(Unit* unit, uint8 /*evadeReason*/) override
    {
        if (Creature* creature = unit ? unit->ToCreature() : nullptr)
            creature->CustomData.Erase(PARTICIPANTS_KEY);
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!_enable.load(std::memory_order_relaxed))
            return;

        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || creature->IsControlledByPlayer()
            || !creature->IsDamageEnoughForLootingAndReward() || creature->IsLootRewardDisabled())
            return;

        OpenTapParticipants* data = creature->CustomData.Get<OpenTapParticipants>(PARTICIPANTS_KEY);
        if (!data)
            return;

        Player* recipient = creature->GetLootRecipient();
        Group* recipientGroup = recipient ? recipient->GetGroup() : creature->GetLootRecipientGroup();

        for (ObjectGuid const& guid : data->players)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);
            if (!player || !player->IsInMap(creature) || !player->IsAtGroupRewardDistance(creature))
                continue;

            // KillRewarder credits the first tapper and nearby members of
            // that tapper's current group.
            if (player == recipient
                || (recipientGroup && player->GetGroup() == recipientGroup && player->IsAtGroupRewardDistance(creature)))
                continue;

            // Match KillRewarder's rule for released group members.
            if (!player->IsAlive() && player->GetCorpse())
                continue;

            player->KilledMonster(creature->GetCreatureTemplate(), creature->GetGUID());
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_KILL_CREATURE_TYPE,
                creature->GetCreatureType(), 1, creature);
        }

        creature->CustomData.Erase(PARTICIPANTS_KEY);
    }
};

void AddSC_open_tap()
{
    new OpenTapWorld();
    new OpenTapUnit();
}

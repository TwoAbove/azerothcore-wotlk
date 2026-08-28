/*
 * mod-tertiary-stats: fabled effect "Cavalier".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "GameObject.h"
#include "SharedDefines.h"
#include "Creature.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellMgr.h"
#include "test_harness.h"

namespace Fabled
{
namespace
{
constexpr uint32 SPELL_BROWN_HORSE = 458;
constexpr uint32 SPELL_FIREBALL_RANK_1 = 133;
constexpr uint32 TEST_GATHERING_SPELL = 8613; // Skinning.
constexpr uint32 TEST_CRAFTING_SPELL = 2963; // Bolt of Linen Cloth.
constexpr uint32 TEST_POTION_ITEM = 3928; // Superior Healing Potion.
constexpr uint32 TEST_FOOD_ITEM = 117; // Tough Jerky.

SpellInfo const* OnUseSpellInfo(uint32 itemId)
{
    ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemId);
    if (!itemTemplate)
        return nullptr;

    for (_Spell const& itemSpell : itemTemplate->Spells)
        if (itemSpell.SpellId && itemSpell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            return sSpellMgr->GetSpellInfo(itemSpell.SpellId);
    return nullptr;
}

bool IsEligibleGroundMount(bool mounted, bool outdoors, bool inFlight, bool hasMountedFlightSpeed)
{
    return mounted && outdoors && !inFlight && !hasMountedFlightSpeed;
}


class CavalierScript final : public Script
{
public:
    CavalierScript() : Script(Effect::Cavalier) { }

    bool CanCastWhileMounted(Player* player, Runtime& /*runtime*/, Spell const* /*spell*/) override
    {
        return IsEligible(player);
    }

    bool CanAttackWhileMounted(Player* player, Runtime& /*runtime*/, Unit* /*victim*/,
        bool /*meleeAttack*/) override
    {
        return IsEligible(player);
    }

    bool CanUseGameObjectWhileMounted(Player* player, Runtime& /*runtime*/,
        GameObject* gameObject) override
    {
        return gameObject && IsEligible(player);
    }

private:
    static bool IsEligible(Player* player)
    {
        // Mounted flight-speed auras are the core's capability marker for a flying mount.
        return player && IsEligibleGroundMount(player->IsMounted(), player->IsOutdoors(),
            player->IsInFlight(), player->HasIncreaseMountedFlightSpeedAura());
    }
};

class CavalierTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {

        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled spell catalog is ready");
        if (!actor || !IsReady())
        {
            _stage = Stage::Cleanup;
            return;
        }

        actor->RemoveAurasByType(SPELL_AURA_MOUNTED);
        actor->Dismount();

        Creature* dummy = context.SpawnDummy(20.0f);
        context.Expect(dummy != nullptr, "cast-time spell target spawned");
        if (dummy)
            _dummyGuid = dummy->GetGUID();

        context.Expect(Test::EquipFabled(actor, Effect::Cavalier) != nullptr,
            "Cavalier trinket equipped");

        SpellInfo const* mountInfo = sSpellMgr->GetSpellInfo(SPELL_BROWN_HORSE);
        SpellInfo const* damageInfo = sSpellMgr->GetSpellInfo(SPELL_FIREBALL_RANK_1);
        context.Expect(mountInfo && damageInfo,
            "Brown Horse and Fireball spell data resolved");

        if (!dummy || !mountInfo || !damageInfo || !Has(GetRuntime(actor), Effect::Cavalier))
        {
            _stage = Stage::Cleanup;
            return;
        }

        actor->CastSpell(actor, SPELL_BROWN_HORSE, true);
        _stage = Stage::VerifyMounted;
    }

    void Update(TestHarness::Context& context, uint32 /*diff*/) override
    {
        Player* actor = context.GetActor();
        if (_stage == Stage::VerifyMounted)
        {
            Creature* dummy = context.GetCreature(_dummyGuid);
            SpellInfo const* damageInfo = sSpellMgr->GetSpellInfo(SPELL_FIREBALL_RANK_1);
            SpellInfo const* gatheringInfo = sSpellMgr->GetSpellInfo(TEST_GATHERING_SPELL);
            SpellInfo const* craftingInfo = sSpellMgr->GetSpellInfo(TEST_CRAFTING_SPELL);
            SpellInfo const* potionInfo = OnUseSpellInfo(TEST_POTION_ITEM);
            SpellInfo const* foodInfo = OnUseSpellInfo(TEST_FOOD_ITEM);
            if (!actor || !dummy || !damageInfo || !gatheringInfo || !craftingInfo
                || !potionInfo || !foodInfo)
            {
                context.Expect(false, "mounted activity spell metadata remained available");
                _stage = Stage::Cleanup;
                return;
            }

            context.Expect(actor->IsMounted() && actor->IsOutdoors()
                    && !actor->IsInFlight() && !actor->HasIncreaseMountedFlightSpeedAura(),
                "Brown Horse is an outdoor ground mount");

            ExpectMountedCast(context, actor, dummy, damageInfo,
                "ordinary combat spell");
            context.Expect(gatheringInfo->HasEffect(SPELL_EFFECT_SKINNING)
                    || gatheringInfo->HasEffect(SPELL_EFFECT_OPEN_LOCK),
                "gathering fixture is classified by spell effects");
            ExpectMountedCast(context, actor, dummy, gatheringInfo,
                "gathering spell");
            context.Expect(craftingInfo->HasAttribute(SPELL_ATTR0_IS_TRADESKILL),
                "crafting fixture is classified as a trade skill");
            ExpectMountedCast(context, actor, actor, craftingInfo,
                "crafting spell");

            ItemTemplate const* potionTemplate = sObjectMgr->GetItemTemplate(TEST_POTION_ITEM);
            ItemTemplate const* foodTemplate = sObjectMgr->GetItemTemplate(TEST_FOOD_ITEM);
            context.Expect(potionTemplate && potionTemplate->IsPotion(),
                "potion fixture is classified by item metadata");
            context.Expect(foodTemplate && foodTemplate->Class == ITEM_CLASS_CONSUMABLE
                    && foodTemplate->SubClass == ITEM_SUBCLASS_FOOD,
                "food fixture is classified by item metadata");
            ExpectMountedCast(context, actor, actor, potionInfo,
                "potion use");
            ExpectMountedCast(context, actor, actor, foodInfo,
                "food use");
            context.Expect(actor->Attack(dummy, true),
                "Cavalier permits mounted melee attacks");
            actor->AttackStop();
            context.Expect(HandleCanAttackWhileMounted(actor, dummy, false),
                "Cavalier permits mounted ranged attacks");

            context.Expect(!IsEligibleGroundMount(true, false, false, false),
                "Cavalier rejects an indoor mounted cast at predicate level");
            context.Expect(!IsEligibleGroundMount(true, true, false, true),
                "Cavalier rejects a flight-capable mount at predicate level");
            context.Expect(!IsEligibleGroundMount(true, true, true, false),
                "Cavalier rejects taxi flight at predicate level");

            Test::UnequipFabled(actor, Effect::Cavalier);

            Spell unequippedCast(actor, damageInfo, TRIGGERED_NONE);
            unequippedCast.m_targets.SetUnitTarget(dummy);
            SpellCastResult unequippedResult = unequippedCast.CheckCast(false);
            context.Expect(unequippedResult == SPELL_FAILED_NOT_MOUNTED,
                "mounted casting is rejected after Cavalier is unequipped",
                "result=" + std::to_string(uint32(unequippedResult)));

            _stage = Stage::Cleanup;
            return;
        }

        if (_stage == Stage::Cleanup)
        {
            if (actor)
            {
                Test::UnequipFabled(actor, Effect::Cavalier);
                actor->RemoveAurasByType(SPELL_AURA_MOUNTED);
                actor->Dismount();
            }
            context.DespawnAllDummies();
            _stage = Stage::Done;
            context.Finish();
        }
    }

private:
    enum class Stage : uint8
    {
        VerifyMounted,
        Cleanup,
        Done
    };

    static void ExpectMountedCast(TestHarness::Context& context, Player* actor,
        Unit* target, SpellInfo const* spellInfo, char const* activity)
    {
        Spell spell(actor, spellInfo, TRIGGERED_NONE);
        spell.m_targets.SetUnitTarget(target);
        SpellCastResult result = spell.CheckCast(false);
        context.Expect(result != SPELL_FAILED_NOT_MOUNTED,
            std::string("Cavalier permits mounted ") + activity,
            "spell=" + std::to_string(spellInfo->Id)
                + " result=" + std::to_string(uint32(result)));
    }

    Stage _stage = Stage::Cleanup;
    ObjectGuid _dummyGuid;
};
} // namespace

std::unique_ptr<Script> MakeCavalier()
{
    return std::make_unique<CavalierScript>();
}

void RegisterCavalierTests()
{
    TestHarness::RegisterSuite("fabled-cavalier",
        [] { return std::make_unique<CavalierTestSuite>(); });
}
} // namespace Fabled

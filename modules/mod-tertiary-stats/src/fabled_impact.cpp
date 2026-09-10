/*
 * mod-tertiary-stats: fabled effect "Impact".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GridNotifiers.h"
#include "Opcodes.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "test_harness.h"

#include <list>
#include <memory>
#include <string>

namespace Fabled
{
namespace
{
class ImpactScript final : public Script
{
public:
    ImpactScript() : Script(Effect::Impact) { }

    void OnRefresh(Player* /*player*/, Runtime& runtime, bool active) override
    {
        if (!active)
            runtime.impact.pendingFallDamage = 0;
    }

    void OnDeath(Player* /*player*/, Runtime& runtime) override
    {
        runtime.impact.pendingFallDamage = 0;
    }

    void OnEnvironmentalDamage(Player* /*player*/, Runtime& runtime, EnviromentalDamage type, uint32& damage) override
    {
        if (type != DAMAGE_FALL || !damage)
            return;

        runtime.impact.pendingFallDamage = damage;
        damage = 0;
    }

    void OnAfterMove(Player* player, Runtime& runtime, uint32 opcode) override
    {
        uint32 preventedDamage = runtime.impact.pendingFallDamage;
        runtime.impact.pendingFallDamage = 0;
        if (opcode != MSG_MOVE_FALL_LAND || !preventedDamage || !player->IsAlive())
            return;

        float radius = GetSettings(player).impactRadiusYd;
        std::list<Unit*> nearby;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(player, player, radius);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, nearby, check);
        Cell::VisitObjects(player, searcher, radius);

        for (Unit* target : nearby)
        {
            if (player->IsFriendlyTo(target))
                continue;

            CastEffectDamage(player, target, preventedDamage, SPELL_SCHOOL_MASK_NORMAL,
                SPELL_IMPACT_DAMAGE);
        }
    }
};

class ImpactTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled custom spell gate is ready");
        if (!actor || !IsReady())
        {
            Finish(context);
            return;
        }

        _settingsSaved = true;
        TestSettings(actor).impactRadiusYd = 20.0f;

        _equipped = Test::EquipFabled(context, actor, Effect::Impact) != nullptr;
        context.Expect(_equipped, "Impact fabled trinket equipped");
        if (!_equipped)
        {
            Finish(context);
            return;
        }

        Creature* first = context.SpawnDummy(3.0f, 0.0f);
        Creature* second = context.SpawnDummy(6.0f, 1.5f);
        bool firstEngaged = first && context.Engage(first->GetGUID());
        bool secondEngaged = second && context.Engage(second->GetGUID());
        context.Expect(firstEngaged && secondEngaged,
            "two hostile targets are engaged inside the Impact radius");
        if (!firstEngaged || !secondEngaged)
        {
            Finish(context);
            return;
        }

        _firstGuid = first->GetGUID();
        _secondGuid = second->GetGUID();
        _stage = Stage::Fall;
        _elapsedMs = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        if (_stage == Stage::Done)
            return;

        _elapsedMs += diff;

        Player* actor = context.GetActor();
        Creature* first = context.GetCreature(_firstGuid);
        Creature* second = context.GetCreature(_secondGuid);
        if (!actor || !first || !second)
        {
            context.Fail("Impact test objects remain available");
            Finish(context);
            return;
        }

        if (_stage == Stage::Fall)
        {
            if (_elapsedMs < 100)
                return;
            _elapsedMs = 0;
            Position landing = actor->GetPosition();
            // The last airborne position is outside both targets' radius.
            actor->UpdatePosition(landing.GetPositionX() - 40.0f, landing.GetPositionY(),
                landing.GetPositionZ() + 40.0f, landing.GetOrientation());
            actor->SetFallInformation(0, landing.GetPositionZ() + 40.0f);
            actor->SetHealth(actor->GetMaxHealth());
            uint32 actorHealthBefore = actor->GetHealth();
            _firstHealthBefore = first->GetHealth();
            _secondHealthBefore = second->GetHealth();

            MovementInfo movement = actor->m_movementInfo;
            movement.pos = landing;
            movement.flags = MOVEMENTFLAG_NONE;
            movement.fallTime = 2000;
            WorldPacket packet(MSG_MOVE_FALL_LAND);
            bool moved = actor->GetSession()->ProcessMovementInfo(movement, actor, actor, packet);

            context.Expect(moved && actor->GetExactDist(&landing) < 0.1f,
                "validated landing packet relocates the airborne actor");
            context.Expect(actor->GetHealth() == actorHealthBefore,
                "Impact prevents fall damage during movement processing");
            // Resolve now, not at the next update's (possibly different) position.
            context.Expect(first->GetHealth() < _firstHealthBefore
                    && second->GetHealth() < _secondHealthBefore,
                "landing AoE resolves at the relocated position before movement returns");
            _stage = Stage::AwaitImpact;
            _elapsedMs = 0;
            return;
        }

        if (_stage == Stage::AwaitImpact)
        {
            if ((first->GetHealth() >= _firstHealthBefore
                    || second->GetHealth() >= _secondHealthBefore)
                && _elapsedMs < 2000)
                return;
            context.Expect(first->GetHealth() < _firstHealthBefore,
                "Impact sends the first target through native physical damage resolution",
                "before=" + std::to_string(_firstHealthBefore)
                    + ",after=" + std::to_string(first->GetHealth()));
            context.Expect(second->GetHealth() < _secondHealthBefore,
                "Impact sends the second target through native physical damage resolution",
                "before=" + std::to_string(_secondHealthBefore)
                    + ",after=" + std::to_string(second->GetHealth()));
            _stage = Stage::Lava;
            return;
        }

        uint32 firstHealthBefore = first->GetHealth();
        uint32 secondHealthBefore = second->GetHealth();
        actor->SetHealth(actor->GetMaxHealth());
        actor->EnvironmentalDamage(DAMAGE_LAVA, 321);
        context.Expect(first->GetHealth() == firstHealthBefore && second->GetHealth() == secondHealthBefore,
            "Impact does not redirect non-fall environmental damage",
            "firstBefore=" + std::to_string(firstHealthBefore)
                + ",firstAfter=" + std::to_string(first->GetHealth())
                + ",secondBefore=" + std::to_string(secondHealthBefore)
                + ",secondAfter=" + std::to_string(second->GetHealth()));
        Finish(context);
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context);
    }

private:
    enum class Stage
    {
        Fall,
        AwaitImpact,
        Lava,
        Done
    };

    void Cleanup(TestHarness::Context& context)
    {
        if (Player* actor = context.GetActor())
        {
            actor->SetHealth(actor->GetMaxHealth());
            if (_equipped)
                Test::UnequipFabled(context, actor, Effect::Impact);
        }
        _equipped = false;

        context.DespawnAllDummies();
        if (_settingsSaved)
        {
            ClearTestSettings(context.GetActor());
            _settingsSaved = false;
        }
    }

    void Finish(TestHarness::Context& context)
    {
        Cleanup(context);
        _stage = Stage::Done;
        context.Finish();
    }

    ObjectGuid _firstGuid;
    ObjectGuid _secondGuid;
    Stage _stage = Stage::Done;
    uint32 _elapsedMs = 0;
    bool _settingsSaved = false;
    uint32 _firstHealthBefore = 0;
    uint32 _secondHealthBefore = 0;
    bool _equipped = false;
};
} // namespace

std::unique_ptr<Script> MakeImpact()
{
    return std::make_unique<ImpactScript>();
}

void RegisterImpactTests()
{
    TestHarness::RegisterSuite("fabled-impact",
        [] { return std::make_unique<ImpactTestSuite>(); });
}
} // namespace Fabled

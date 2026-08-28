/*
 * mod-tertiary-stats: fabled effect "Crossfire".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "ScriptMgr.h"
#include "SpellDefines.h"
#include "SpellMgr.h"
#include "Unit.h"
#include "test_harness.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>

namespace Fabled
{
namespace
{
constexpr uint32 SPELL_FIREBALL_R1 = 133;
constexpr uint32 TEST_DAMAGE = 1000;
constexpr uint32 TEST_TIMEOUT_MS = 10000;
constexpr uint32 MIN_EVENT_POLLS = 3;
constexpr TriggerCastFlags TEST_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);

class CrossfireScript final : public Script
{
public:
    CrossfireScript() : Script(Effect::Crossfire) { }

    void OnRefresh(Player* /*player*/, Runtime& runtime, bool active) override
    {
        if (active)
            return;

        runtime.crossfire.previousTarget.Clear();
        runtime.crossfire.currentTarget.Clear();
    }
    void OnDealtDamageFinal(Player* player, Runtime& runtime, Unit* victim, uint32 damage,
        SpellInfo const* spellInfo, DamageKind kind) override
    {

        if (kind == DamageKind::Periodic)
            return;

        ObjectGuid victimGuid = victim->GetGUID();
        if (victimGuid != runtime.crossfire.currentTarget)
        {
            runtime.crossfire.previousTarget = runtime.crossfire.currentTarget;
            runtime.crossfire.currentTarget = victimGuid;
        }

        if (runtime.crossfire.previousTarget.IsEmpty()
            || runtime.crossfire.previousTarget == victimGuid)
            return;

        Unit* previousTarget = ObjectAccessor::GetUnit(*player, runtime.crossfire.previousTarget);
        Settings const& settings = GetSettings(player);
        if (!previousTarget || !previousTarget->IsAlive()
            || player->IsFriendlyTo(previousTarget)
            || !player->IsWithinDistInMap(previousTarget, settings.crossfireRangeYd))
            return;

        double scaledDamage = double(damage) * double(settings.crossfirePct) / 100.0;
        uint32 echoDamage = uint32(std::clamp(scaledDamage, 0.0,
            double(std::numeric_limits<int32>::max())));
        if (!echoDamage)
            return;

        SpellSchoolMask schoolMask = spellInfo
            ? spellInfo->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL;
        CastEffectDamage(player, previousTarget, echoDamage, schoolMask,
            SPELL_CROSSFIRE_DAMAGE);
    }
};

class CrossfireTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled runtime is ready");
        if (!actor || !IsReady())
        {
            context.Finish();
            return;
        }
        _settingsSaved = true;
        TestSettings(actor).crossfirePct = 30.0f;
        TestSettings(actor).crossfireRangeYd = 40.0f;

        _equipped = Test::EquipFabled(actor, Effect::Crossfire) != nullptr;
        context.Expect(_equipped, "Crossfire trinket equips");
        if (!_equipped)
        {
            Cleanup(context, true);
            return;
        }

        Runtime& runtime = GetRuntime(actor);
        runtime.crossfire.previousTarget.Clear();
        runtime.crossfire.currentTarget.Clear();

        Creature* targetA = context.SpawnDummy(3.0f, -0.4f);
        Creature* targetB = context.SpawnDummy(3.0f, 0.4f);
        context.Expect(targetA != nullptr && targetB != nullptr,
            "two Crossfire targets spawn");
        if (!targetA || !targetB)
        {
            Cleanup(context, true);
            return;
        }

        _targetA = targetA->GetGUID();
        _targetB = targetB->GetGUID();
        context.Expect(context.Engage(_targetA) && context.Engage(_targetB),
            "both Crossfire targets are hostile and engaged");

        _stage = Stage::HitFirstTarget;
        _elapsed = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        _elapsed += diff;

        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Fail("headless actor remains available");
            Cleanup(context, true);
            return;
        }

        if (_stage == Stage::HitFirstTarget)
        {
            Creature* targetA = context.GetCreature(_targetA);
            Creature* targetB = context.GetCreature(_targetB);
            if (!targetA || !targetB)
            {
                context.Fail("both Crossfire targets remain available");
                Cleanup(context, true);
                return;
            }

            _firstTargetBefore = targetA->GetHealth();
            _secondTargetBefore = targetB->GetHealth();
            context.ClearEvents();
            CastDamage(actor, targetA);
            Next(Stage::HitSecondTarget);
            return;
        }

        if (_stage == Stage::HitSecondTarget)
        {
            if (!AwaitDamage(context, actor, _targetA))
                return;
            Creature* targetA = context.GetCreature(_targetA);
            Creature* targetB = context.GetCreature(_targetB);
            if (!targetA || !targetB)
            {
                context.Fail("both Crossfire targets survive the first hit");
                Cleanup(context, true);
                return;
            }

            Runtime& runtime = GetRuntime(actor);
            context.Expect(targetA->GetHealth() < _firstTargetBefore
                    && targetB->GetHealth() == _secondTargetBefore,
                "the first direct spell damages only target A");
            context.Expect(runtime.crossfire.currentTarget == _targetA
                    && runtime.crossfire.previousTarget.IsEmpty(),
                "the first direct hit establishes the current target");

            _firstTargetBeforeEcho = targetA->GetHealth();
            _secondTargetBeforeHit = targetB->GetHealth();
            targetB->SetHealth(1);
            context.ClearEvents();
            CastDamage(actor, targetB);
            Next(Stage::CheckEcho);
            return;
        }

        if (_stage == Stage::CheckEcho)
        {
            if (!AwaitDamage(context, actor, _targetB))
                return;
            Creature* targetA = context.GetCreature(_targetA);
            Creature* targetB = context.GetCreature(_targetB);
            if (!targetA || !targetB)
            {
                context.Fail("Crossfire targets remain available after the lethal primary hit");
                Cleanup(context, true);
                return;
            }

            TestHarness::Event const* directEvent = context.FindEvent(
                TestHarness::EventType::DamageFinal, actor->GetGUID(), _targetB, SPELL_FIREBALL_R1);
            TestHarness::Event const* echoEvent = context.FindEvent(
                TestHarness::EventType::DamageFinal, actor->GetGUID(), _targetA,
                SPELL_CROSSFIRE_DAMAGE);
            uint32 directDamage = directEvent ? uint32(directEvent->amount) : 0;
            uint32 echoDamage = echoEvent ? uint32(echoEvent->amount) : 0;
            uint32 expectedEcho = uint32(double(directDamage)
                * double(TestSettings(actor).crossfirePct) / 100.0);
            Runtime& runtime = GetRuntime(actor);
            context.Expect(directDamage > 1 && echoDamage == expectedEcho,
                "a lethal Fire hit echoes from its full amount before the primary target dies",
                "direct=" + std::to_string(directDamage)
                    + " applied=" + std::to_string(echoDamage)
                    + " expected=" + std::to_string(expectedEcho));
            context.Expect(runtime.crossfire.currentTarget == _targetB
                    && runtime.crossfire.previousTarget == _targetA,
                "switching targets rotates the Crossfire target history");
            CrossfireScript script;
            uint32 savedArmor = targetA->GetArmor();
            targetA->SetArmor(50000);
            uint32 armoredTargetBefore = targetA->GetHealth();
            uint32 rawArmoredEcho = uint32(float(TEST_DAMAGE)
                * TestSettings(actor).crossfirePct / 100.0f);
            SpellInfo const* outputInfo = sSpellMgr->GetSpellInfo(SPELL_CROSSFIRE_DAMAGE);
            uint32 expectedArmoredEcho = Unit::CalcArmorReducedDamage(
                actor, targetA, rawArmoredEcho, outputInfo, 0, BASE_ATTACK);
            script.OnDealtDamageFinal(actor, runtime, targetB, TEST_DAMAGE, nullptr,
                DamageKind::Melee);
            uint32 armoredEcho = armoredTargetBefore - targetA->GetHealth();
            context.Expect(armoredEcho == expectedArmoredEcho && armoredEcho < rawArmoredEcho,
                "Crossfire echo respects the previous target's armor",
                "raw=" + std::to_string(rawArmoredEcho)
                    + " applied=" + std::to_string(armoredEcho)
                    + " expected=" + std::to_string(expectedArmoredEcho));

            uint32 immuneTargetBefore = targetA->GetHealth();
            targetA->ApplySpellImmune(0, IMMUNITY_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, true);
            script.OnDealtDamageFinal(actor, runtime, targetB, TEST_DAMAGE, nullptr,
                DamageKind::Melee);
            targetA->ApplySpellImmune(0, IMMUNITY_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, false);
            context.Expect(targetA->GetHealth() == immuneTargetBefore,
                "Crossfire echo respects physical damage immunity");
            targetA->SetArmor(savedArmor);
            SpellInfo const* fireballInfo = sSpellMgr->GetSpellInfo(SPELL_FIREBALL_R1);
            uint32 fireImmuneTargetBefore = targetA->GetHealth();
            targetA->ApplySpellImmune(0, IMMUNITY_DAMAGE, SPELL_SCHOOL_MASK_FIRE, true);
            script.OnDealtDamageFinal(actor, runtime, targetB, TEST_DAMAGE, fireballInfo,
                DamageKind::Spell);
            targetA->ApplySpellImmune(0, IMMUNITY_DAMAGE, SPELL_SCHOOL_MASK_FIRE, false);
            context.Expect(targetA->GetHealth() == fireImmuneTargetBefore,
                "Crossfire preserves the original spell school for immunity");

            uint32 firstTargetBeforeMiss = targetA->GetHealth();
            ObjectGuid currentBeforeMiss = runtime.crossfire.currentTarget;
            ObjectGuid previousBeforeMiss = runtime.crossfire.previousTarget;
            uint32 missedMeleeDamage = TEST_DAMAGE;
            sScriptMgr->ModifyMeleeDamage(targetB, actor, missedMeleeDamage);
            context.Expect(targetA->GetHealth() == firstTargetBeforeMiss
                    && runtime.crossfire.currentTarget == currentBeforeMiss
                    && runtime.crossfire.previousTarget == previousBeforeMiss,
                "an unresolved melee swing neither echoes nor changes target history");


            uint32 healthBeforePeriodic = targetA->GetHealth();
            ObjectGuid currentBeforePeriodic = runtime.crossfire.currentTarget;
            ObjectGuid previousBeforePeriodic = runtime.crossfire.previousTarget;
            uint32 periodicDamage = TEST_DAMAGE;
            script.OnDealtDamageFinal(actor, runtime, targetB, periodicDamage, nullptr,
                DamageKind::Periodic);
            context.Expect(targetA->GetHealth() == healthBeforePeriodic
                    && runtime.crossfire.currentTarget == currentBeforePeriodic
                    && runtime.crossfire.previousTarget == previousBeforePeriodic,
                "periodic damage neither echoes nor changes target history");

            targetA->NearTeleportTo(actor->GetPositionX() + TestSettings(actor).crossfireRangeYd + 10.0f,
                actor->GetPositionY(), actor->GetPositionZ(), targetA->GetOrientation());
            context.Expect(!actor->IsWithinDistInMap(targetA, TestSettings(actor).crossfireRangeYd),
                "previous target A is moved beyond Crossfire range");
            uint32 firstTargetBeforeRangeHit = targetA->GetHealth();
            script.OnDealtDamageFinal(actor, runtime, targetB, TEST_DAMAGE, nullptr,
                DamageKind::Spell);
            context.Expect(targetA->GetHealth() == firstTargetBeforeRangeHit,
                "a previous target beyond forty yards receives no echo",
                "targetA=" + std::to_string(targetA->GetHealth())
                    + "/" + std::to_string(firstTargetBeforeRangeHit)
                    + " distance=" + std::to_string(actor->GetDistance(targetA)));

            ObjectGuid previousBeforeDespawn = runtime.crossfire.previousTarget;
            context.Expect(context.DespawnDummy(_targetA),
                "previous target A despawns cleanly");
            script.OnDealtDamageFinal(actor, runtime, targetB, TEST_DAMAGE, nullptr,
                DamageKind::Spell);
            context.Expect(ObjectAccessor::GetUnit(*actor, _targetA) == nullptr
                    && runtime.crossfire.previousTarget == previousBeforeDespawn,
                "a stale previous target is ignored without changing target history");
            Cleanup(context, true);
            return;
        }
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context, false);
    }

private:
    enum class Stage
    {
        HitFirstTarget,
        HitSecondTarget,
        CheckEcho,
        Done
    };

    bool AwaitDamage(TestHarness::Context& context, Player* actor, ObjectGuid target)
    {
        if (context.FindEvent(TestHarness::EventType::DamageFinal, actor->GetGUID(), target, SPELL_FIREBALL_R1))
            return true;

        if (++_eventPolls < MIN_EVENT_POLLS || _elapsed < TEST_TIMEOUT_MS)
            return false;

        context.Fail("Crossfire damage resolves before timeout",
            "stage=" + std::to_string(uint32(_stage)));
        Cleanup(context, false);
        return false;
    }

    static void CastDamage(Player* actor, Unit* target)
    {
        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, int32(TEST_DAMAGE));
        actor->CastCustomSpell(SPELL_FIREBALL_R1, values, target, TEST_TRIGGER_FLAGS);
    }

    void Next(Stage stage)
    {
        _stage = stage;
        _elapsed = 0;
        _eventPolls = 0;
    }

    void Cleanup(TestHarness::Context& context, bool finish)
    {
        if (_stage == Stage::Done)
            return;

        if (_equipped)
            if (Player* actor = context.GetActor())
                Test::UnequipFabled(actor, Effect::Crossfire);
        _equipped = false;

        if (_settingsSaved)
            ClearTestSettings(context.GetActor());
        _settingsSaved = false;

        context.DespawnAllDummies();
        _stage = Stage::Done;
        if (finish)
            context.Finish();
    }

    Stage _stage = Stage::Done;
    ObjectGuid _targetA;
    ObjectGuid _targetB;
    uint32 _elapsed = 0;
    uint32 _eventPolls = 0;
    uint32 _firstTargetBefore = 0;
    uint32 _secondTargetBefore = 0;
    uint32 _firstTargetBeforeEcho = 0;
    uint32 _secondTargetBeforeHit = 0;
    bool _settingsSaved = false;
    bool _equipped = false;
};
} // namespace

std::unique_ptr<Script> MakeCrossfire()
{
    return std::make_unique<CrossfireScript>();
}

void RegisterCrossfireTests()
{
    TestHarness::RegisterSuite("fabled-crossfire",
        [] { return std::make_unique<CrossfireTestSuite>(); });
}
} // namespace Fabled

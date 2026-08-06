/*
 * mod-tertiary-stats: fabled effect "Overkill".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Spell.h"
#include "ScriptMgr.h"
#include "SpellDefines.h"

#include <limits>
#include <memory>
#include <string>

namespace Fabled
{
namespace
{
constexpr uint32 SPELL_TEST_DAMAGE = 133; // Fireball: native, direct, single-target.
constexpr uint32 TEST_DAMAGE = 5000;
constexpr uint32 TEST_TARGET_HEALTH = 1000000;
constexpr uint32 TEST_TIMEOUT_MS = 4000;

void ClearBank(Runtime& runtime)
{
    runtime.overkillBank = 0;
    runtime.overkillExpiresMs = 0;
    runtime.overkillSchoolMask = SPELL_SCHOOL_MASK_NONE;
}

void ClearPending(Runtime& runtime)
{
    runtime.overkillPendingTarget.Clear();
    runtime.overkillPendingDamage = 0;
    runtime.overkillPendingSchoolMask = SPELL_SCHOOL_MASK_NONE;
}
void CastTestDamage(Player* player, Unit* target)
{
    CastEffectDamage(player, target, TEST_DAMAGE, SPELL_SCHOOL_MASK_FIRE,
        SPELL_TEST_DAMAGE);
}

class OverkillScript final : public Script
{
public:
    OverkillScript() : Script(Effect::Overkill) { }

    void OnRefresh(Player* /*player*/, Runtime& runtime, bool active) override
    {
        if (!active)
        {
            ClearBank(runtime);
            ClearPending(runtime);
        }
    }

    void OnUpdate(Player* player, Runtime& runtime, uint32 /*diffMs*/, uint64 nowMs) override
    {
        if (runtime.overkillBank && runtime.overkillExpiresMs && nowMs >= runtime.overkillExpiresMs)
            ClearBank(runtime);

        if (!runtime.overkillPendingDamage)
            return;

        ObjectGuid targetGuid = runtime.overkillPendingTarget;
        uint64 damage = runtime.overkillPendingDamage;
        SpellSchoolMask schoolMask = runtime.overkillPendingSchoolMask;
        ClearPending(runtime);
        if (Unit* target = ObjectAccessor::GetUnit(*player, targetGuid);
            target && target->IsAlive())
        {
            CastEffectDamage(player, target, damage, schoolMask, SPELL_OVERKILL_DAMAGE);
        }
    }

    void OnBeforeDealtDamage(Player* /*player*/, Runtime& runtime, Unit* victim,
        uint32 /*damage*/, SpellInfo const* /*spellInfo*/, DamageKind kind) override
    {
        if (kind == DamageKind::Periodic || !runtime.overkillBank
            || !runtime.overkillExpiresMs || Now() >= runtime.overkillExpiresMs
            || !victim || !victim->IsAlive())
            return;

        runtime.overkillPendingTarget = victim->GetGUID();
        runtime.overkillPendingDamage = runtime.overkillBank;
        runtime.overkillPendingSchoolMask = runtime.overkillSchoolMask;
        ClearBank(runtime);
    }
    void OnDealtDamageFinal(Player* player, Runtime& runtime, Unit* victim, uint32 damage,
        SpellInfo const* spellInfo, DamageKind /*kind*/) override
    {
        if (!player || !victim || !victim->IsAlive())
            return;

        uint32 health = victim->GetHealth();
        if (!health || damage <= health || !player->isHonorOrXPTarget(victim))
            return;

        runtime.overkillBank = uint64(damage) - health;
        runtime.overkillExpiresMs = Now() + GetSettings().overkillWindowMs;
        runtime.overkillSchoolMask = spellInfo
            ? spellInfo->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL;
    }
};

class OverkillTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled custom spells are ready");
        if (!actor || !IsReady())
        {
            Finish(context);
            return;
        }

        _savedSettings = GetSettings();
        _settingsSaved = true;
        MutableSettings().overkillWindowMs = 1000;

        Test::UnequipFabled(actor, Effect::Overkill);
        _equipped = Test::EquipFabledTrinket(actor, Effect::Overkill) != nullptr;
        context.Expect(_equipped, "Overkill fabled trinket equipped");
        if (!_equipped)
        {
            Finish(context);
            return;
        }

        Creature* baseline = context.SpawnDummy(3.0f, -0.8f);
        Creature* killTarget = context.SpawnDummy(3.0f, -0.4f);
        Creature* spendTarget = context.SpawnDummy(3.0f, 0.0f);
        Creature* expiryTarget = context.SpawnDummy(3.0f, 0.4f);
        Creature* greyTarget = context.SpawnDummy(3.0f, 0.8f);
        bool spawned = baseline && killTarget && spendTarget && expiryTarget && greyTarget;
        context.Expect(spawned, "five deterministic damage targets spawned");
        if (!spawned)
        {
            Finish(context);
            return;
        }

        _baselineGuid = baseline->GetGUID();
        _killGuid = killTarget->GetGUID();
        _spendGuid = spendTarget->GetGUID();
        _expiryGuid = expiryTarget->GetGUID();
        _greyGuid = greyTarget->GetGUID();

        baseline->SetMaxHealth(TEST_TARGET_HEALTH);
        baseline->SetHealth(TEST_TARGET_HEALTH);
        spendTarget->SetMaxHealth(TEST_TARGET_HEALTH);
        spendTarget->SetHealth(TEST_TARGET_HEALTH);

        context.Expect(context.Engage(_baselineGuid), "baseline target engaged");
        _healthBefore = baseline->GetHealth();
        context.ClearEvents();
        CastTestDamage(actor, baseline);
        SetStage(Stage::AwaitBaseline);
    }

    void Update(TestHarness::Context& context, uint32 diffMs) override
    {
        if (_stage == Stage::Done)
            return;

        _elapsedMs += diffMs;

        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Expect(false, "headless actor remains available");
            Finish(context);
            return;
        }
        if (_stage != Stage::AwaitExpiry)
        {
            ObjectGuid expectedTarget = ExpectedTarget();
            bool directResolved = context.FindEvent(TestHarness::EventType::DamageFinal,
                actor->GetGUID(), expectedTarget, SPELL_TEST_DAMAGE);
            bool bankResolved = _stage != Stage::AwaitSpend
                || context.FindEvent(TestHarness::EventType::DamageFinal, actor->GetGUID(),
                    expectedTarget, SPELL_OVERKILL_DAMAGE);
            if (!directResolved || !bankResolved)
            {
                if (_elapsedMs < TEST_TIMEOUT_MS)
                    return;

                context.Fail("Overkill damage resolves before timeout",
                    "stage=" + std::to_string(uint32(_stage)));
                Finish(context);
                return;
            }
        }


        switch (_stage)
        {
            case Stage::AwaitBaseline:
                CheckBaseline(context, actor);
                break;
            case Stage::AwaitBank:
                CheckBank(context, actor);
                break;
            case Stage::AwaitSpend:
                CheckSpend(context, actor);
                break;
            case Stage::AwaitRebank:
                CheckRebank(context, actor);
                break;
            case Stage::AwaitExpiry:
                CheckExpiry(context, actor);
                break;
            case Stage::AwaitGreyKill:
                CheckGreyKill(context, actor);
                break;
            case Stage::Done:
                break;
        }
    }

    void Cancel(TestHarness::Context& context) override
    {
        Finish(context, false);
    }

private:
    enum class Stage
    {
        AwaitBaseline,
        AwaitBank,
        AwaitSpend,
        AwaitRebank,
        AwaitExpiry,
        AwaitGreyKill,
        Done
    };

    void SetStage(Stage stage)
    {
        _stage = stage;
        _elapsedMs = 0;
    }

    Creature* RequireTarget(TestHarness::Context& context, ObjectGuid guid, std::string const& name)
    {
        Creature* target = context.GetCreature(guid);
        context.Expect(target != nullptr, name);
        return target;
    }
    ObjectGuid ExpectedTarget() const
    {
        switch (_stage)
        {
            case Stage::AwaitBaseline: return _baselineGuid;
            case Stage::AwaitBank: return _killGuid;
            case Stage::AwaitSpend: return _spendGuid;
            case Stage::AwaitRebank: return _expiryGuid;
            case Stage::AwaitGreyKill: return _greyGuid;
            case Stage::AwaitExpiry:
            case Stage::Done:
                return ObjectGuid::Empty;
        }
        return ObjectGuid::Empty;
    }


    void CheckBaseline(TestHarness::Context& context, Player* actor)
    {
        Creature* baseline = RequireTarget(context, _baselineGuid, "baseline target remains available");
        Creature* killTarget = RequireTarget(context, _killGuid, "XP-eligible kill target remains available");
        if (!baseline || !killTarget)
        {
            Finish(context);
            return;
        }

        _baseDamage = _healthBefore - baseline->GetHealth();
        context.Expect(_baseDamage > 1, "native direct-damage action follows the live damage path",
            "damage=" + std::to_string(_baseDamage));
        if (_baseDamage <= 1)
        {
            Finish(context);
            return;
        }

        _killHealth = _baseDamage / 2;
        _expectedBank = uint64(_baseDamage) - _killHealth;
        killTarget->SetMaxHealth(_killHealth);
        killTarget->SetHealth(_killHealth);
        context.Expect(actor->isHonorOrXPTarget(killTarget), "same-level dummy is XP-eligible");
        context.Expect(context.Engage(_killGuid), "XP-eligible kill target engaged");
        context.ClearEvents();
        CastTestDamage(actor, killTarget);
        SetStage(Stage::AwaitBank);
    }

    void CheckBank(TestHarness::Context& context, Player* actor)
    {
        TestHarness::Event const* killingBlow = context.FindEvent(
            TestHarness::EventType::DamageFinal, actor->GetGUID(), _killGuid, SPELL_TEST_DAMAGE);
        uint32 killingDamage = killingBlow ? uint32(killingBlow->amount) : 0;
        _expectedBank = killingDamage > _killHealth ? uint64(killingDamage - _killHealth) : 0;
        Runtime& runtime = GetRuntime(actor);
        context.Expect(runtime.overkillBank == _expectedBank && runtime.overkillExpiresMs > Now()
                && runtime.overkillSchoolMask == SPELL_SCHOOL_MASK_FIRE,
            "excess killing-blow damage banks its amount and original school",
            "bank=" + std::to_string(runtime.overkillBank)
                + ",expected=" + std::to_string(_expectedBank)
                + ",school=" + std::to_string(uint32(runtime.overkillSchoolMask)));

        Creature* spendTarget = RequireTarget(context, _spendGuid, "bank-spend target remains available");
        if (!spendTarget || runtime.overkillBank != _expectedBank)
        {
            Finish(context);
            return;
        }
        uint32 unresolvedDamage = _baseDamage;
        uint64 deadlineBeforeUnresolved = runtime.overkillExpiresMs;
        sScriptMgr->ModifyMeleeDamage(spendTarget, actor, unresolvedDamage);
        context.Expect(unresolvedDamage == _baseDamage
                && runtime.overkillBank == _expectedBank
                && runtime.overkillExpiresMs == deadlineBeforeUnresolved,
            "an unresolved melee swing neither spends nor applies the bank");
        if (runtime.overkillBank != _expectedBank)
        {
            runtime.overkillBank = _expectedBank;
            runtime.overkillExpiresMs = deadlineBeforeUnresolved;
        }


        context.Expect(context.Engage(_spendGuid), "bank-spend target engaged");
        _healthBefore = spendTarget->GetHealth();
        context.ClearEvents();
        CastTestDamage(actor, spendTarget);
        SetStage(Stage::AwaitSpend);
    }

    void CheckSpend(TestHarness::Context& context, Player* actor)
    {
        Creature* spendTarget = RequireTarget(context, _spendGuid, "bank-spend target remains after the hit");
        if (!spendTarget)
        {
            Finish(context);
            return;
        }

        uint32 healthDelta = _healthBefore - spendTarget->GetHealth();
        TestHarness::Event const* directHit = context.FindEvent(
            TestHarness::EventType::DamageFinal, actor->GetGUID(), _spendGuid, SPELL_TEST_DAMAGE);
        uint32 directDamage = directHit ? uint32(directHit->amount) : 0;
        Runtime& runtime = GetRuntime(actor);
        context.Expect(uint64(healthDelta) == uint64(directDamage) + _expectedBank,
            "next direct hit releases the entire bank as separate original-school damage",
            "delta=" + std::to_string(healthDelta)
                + ",direct=" + std::to_string(directDamage)
                + ",bank=" + std::to_string(_expectedBank));
        context.Expect(runtime.overkillBank == 0 && runtime.overkillExpiresMs == 0
                && runtime.overkillPendingDamage == 0
                && context.FindEvent(TestHarness::EventType::DamageFinal, actor->GetGUID(),
                    _spendGuid, SPELL_OVERKILL_DAMAGE),
            "spending the bank resolves the tagged Overkill damage event");

        Creature* expiryTarget = RequireTarget(context, _expiryGuid, "expiry kill target remains available");
        if (!expiryTarget)
        {
            Finish(context);
            return;
        }

        expiryTarget->SetMaxHealth(_killHealth);
        expiryTarget->SetHealth(_killHealth);
        context.Expect(context.Engage(_expiryGuid), "expiry kill target engaged");
        context.ClearEvents();
        CastTestDamage(actor, expiryTarget);
        SetStage(Stage::AwaitRebank);
    }

    void CheckRebank(TestHarness::Context& context, Player* actor)
    {
        TestHarness::Event const* killingBlow = context.FindEvent(
            TestHarness::EventType::DamageFinal, actor->GetGUID(), _expiryGuid, SPELL_TEST_DAMAGE);
        uint32 killingDamage = killingBlow ? uint32(killingBlow->amount) : 0;
        _expectedBank = killingDamage > _killHealth ? uint64(killingDamage - _killHealth) : 0;
        Runtime& runtime = GetRuntime(actor);
        context.Expect(runtime.overkillBank == _expectedBank,
            "a later XP-eligible overkill re-banks its excess",
            "bank=" + std::to_string(runtime.overkillBank));
        if (runtime.overkillBank != _expectedBank)
        {
            Finish(context);
            return;
        }

        AdvanceClock(actor, GetSettings().overkillWindowMs + 1);
        SetStage(Stage::AwaitExpiry);
    }

    void CheckExpiry(TestHarness::Context& context, Player* actor)
    {
        Runtime& runtime = GetRuntime(actor);
        context.Expect(runtime.overkillBank == 0 && runtime.overkillExpiresMs == 0,
            "bank expires after its window on the next player tick");

        Creature* greyTarget = RequireTarget(context, _greyGuid, "grey kill target remains available");
        if (!greyTarget)
        {
            Finish(context);
            return;
        }

        greyTarget->SetLevel(1);
        greyTarget->SetMaxHealth(_killHealth);
        greyTarget->SetHealth(_killHealth);
        context.Expect(!actor->isHonorOrXPTarget(greyTarget), "level-one dummy is grey to the actor");
        context.Expect(context.Engage(_greyGuid), "grey kill target engaged");
        context.ClearEvents();
        CastTestDamage(actor, greyTarget);
        SetStage(Stage::AwaitGreyKill);
    }

    void CheckGreyKill(TestHarness::Context& context, Player* actor)
    {
        Runtime& runtime = GetRuntime(actor);
        context.Expect(runtime.overkillBank == 0 && runtime.overkillExpiresMs == 0,
            "grey-mob killing blow banks no excess damage");
        Finish(context);
    }

    void Finish(TestHarness::Context& context, bool finish = true)
    {
        if (_settingsSaved)
        {
            MutableSettings() = _savedSettings;
            _settingsSaved = false;
        }

        if (Player* actor = context.GetActor())
            if (_equipped)
                Test::UnequipFabled(actor, Effect::Overkill);
        _equipped = false;

        context.DespawnAllDummies();
        _stage = Stage::Done;
        if (finish)
            context.Finish();
    }

    Settings _savedSettings;
    bool _settingsSaved = false;
    bool _equipped = false;
    Stage _stage = Stage::Done;
    uint32 _elapsedMs = 0;
    uint32 _healthBefore = 0;
    uint32 _baseDamage = 0;
    uint32 _killHealth = 0;
    uint64 _expectedBank = 0;
    ObjectGuid _baselineGuid;
    ObjectGuid _killGuid;
    ObjectGuid _spendGuid;
    ObjectGuid _expiryGuid;
    ObjectGuid _greyGuid;
};
} // namespace

std::unique_ptr<Script> MakeOverkill()
{
    TestHarness::RegisterSuite("fabled-overkill",
        [] { return std::make_unique<OverkillTestSuite>(); });
    return std::make_unique<OverkillScript>();
}
} // namespace Fabled

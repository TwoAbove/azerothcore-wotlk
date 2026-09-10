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
#include "SpellMgr.h"

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
    runtime.overkill.bank = 0;
    runtime.overkill.expiresMs = 0;
    runtime.overkill.schoolMask = SPELL_SCHOOL_MASK_NONE;
}

void ClearPending(Runtime& runtime)
{
    runtime.overkill.pending.clear();
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
        if (runtime.overkill.bank && runtime.overkill.expiresMs && nowMs >= runtime.overkill.expiresMs)
            ClearBank(runtime);

        // Detach the batch: native damage callbacks may modify runtime state.
        std::vector<Runtime::OverkillState::Discharge> pending;
        pending.swap(runtime.overkill.pending);
        for (auto const& discharge : pending)
            if (Unit* target = ObjectAccessor::GetUnit(*player, discharge.target);
                target && target->IsAlive())
                CastEffectDamage(player, target, discharge.damage, discharge.schoolMask,
                    SPELL_OVERKILL_DAMAGE);

        // Retain capacity for the next native multi-target action.
        pending.clear();
        if (runtime.overkill.pending.empty())
            pending.swap(runtime.overkill.pending);
    }

    void OnBeforeDealtDamage(Player* /*player*/, Runtime& runtime, Unit* victim,
        uint32 /*damage*/, SpellInfo const* /*spellInfo*/, DamageKind kind) override
    {
        if (kind == DamageKind::Periodic || !runtime.overkill.bank
            || !runtime.overkill.expiresMs || Now() >= runtime.overkill.expiresMs
            || !victim || !victim->IsAlive())
            return;

        runtime.overkill.pending.push_back(
            { victim->GetGUID(), runtime.overkill.bank, runtime.overkill.schoolMask });
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

        runtime.overkill.bank = uint64(damage) - health;
        runtime.overkill.expiresMs = Now() + GetSettings(player).overkillWindowMs;
        runtime.overkill.schoolMask = spellInfo
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
        _settingsSaved = true;
        TestSettings(actor).overkillWindowMs = 1000;

        Test::UnequipFabled(context, actor, Effect::Overkill);
        _equipped = Test::EquipFabled(context, actor, Effect::Overkill) != nullptr;
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
        if (_stage == Stage::AwaitBatch)
        {
            auto const* first = context.FindEvent(TestHarness::EventType::DamageFinal,
                actor->GetGUID(), _baselineGuid, SPELL_OVERKILL_DAMAGE);
            auto const* last = context.FindEvent(TestHarness::EventType::DamageFinal,
                actor->GetGUID(), _spendGuid, SPELL_OVERKILL_DAMAGE);
            if ((!first || !last) && _elapsedMs < TEST_TIMEOUT_MS)
                return;
            context.Expect(first && uint64(first->amount) == _firstBatchBank,
                "first surviving AoE target receives its bank despite an intervening re-bank");
            context.Expect(last && uint64(last->amount) == _lastBatchBank,
                "last surviving AoE target receives the independently re-earned bank");
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
            case Stage::AwaitBatch:
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
        AwaitBatch,
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
            case Stage::AwaitBatch:
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
        context.Expect(runtime.overkill.bank == _expectedBank && runtime.overkill.expiresMs > Now()
                && runtime.overkill.schoolMask == SPELL_SCHOOL_MASK_FIRE,
            "excess killing-blow damage banks its amount and original school",
            "bank=" + std::to_string(runtime.overkill.bank)
                + ",expected=" + std::to_string(_expectedBank)
                + ",school=" + std::to_string(uint32(runtime.overkill.schoolMask)));

        Creature* spendTarget = RequireTarget(context, _spendGuid, "bank-spend target remains available");
        if (!spendTarget || runtime.overkill.bank != _expectedBank)
        {
            Finish(context);
            return;
        }
        uint32 unresolvedDamage = _baseDamage;
        uint64 deadlineBeforeUnresolved = runtime.overkill.expiresMs;
        sScriptMgr->ModifyMeleeDamage(spendTarget, actor, unresolvedDamage);
        context.Expect(unresolvedDamage == _baseDamage
                && runtime.overkill.bank == _expectedBank
                && runtime.overkill.expiresMs == deadlineBeforeUnresolved,
            "an unresolved melee swing neither spends nor applies the bank");
        if (runtime.overkill.bank != _expectedBank)
        {
            runtime.overkill.bank = _expectedBank;
            runtime.overkill.expiresMs = deadlineBeforeUnresolved;
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
        context.Expect(runtime.overkill.bank == 0 && runtime.overkill.expiresMs == 0
                && runtime.overkill.pending.empty()
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
        context.Expect(runtime.overkill.bank == _expectedBank,
            "a later XP-eligible overkill re-banks its excess",
            "bank=" + std::to_string(runtime.overkill.bank));
        if (runtime.overkill.bank != _expectedBank)
        {
            Finish(context);
            return;
        }

        AdvanceClock(actor, GetSettings(actor).overkillWindowMs + 1);
        SetStage(Stage::AwaitExpiry);
    }

    void CheckExpiry(TestHarness::Context& context, Player* actor)
    {
        Runtime& runtime = GetRuntime(actor);
        context.Expect(runtime.overkill.bank == 0 && runtime.overkill.expiresMs == 0,
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
        context.Expect(runtime.overkill.bank == 0 && runtime.overkill.expiresMs == 0,
            "grey-mob killing blow banks no excess damage");
        Creature* first = RequireTarget(context, _baselineGuid, "first batch target remains available");
        Creature* last = RequireTarget(context, _spendGuid, "last batch target remains available");
        Creature* seed = context.SpawnDummy(3.0f, -0.2f);
        Creature* middle = context.SpawnDummy(3.0f, 0.2f);
        if (!first || !last || !seed || !middle)
        {
            context.Fail("batch damage targets available");
            Finish(context);
            return;
        }
        seed->SetHealth(1);
        middle->SetHealth(2);
        context.Engage(seed->GetGUID());
        context.Engage(middle->GetGUID());
        context.Engage(first->GetGUID());
        context.Engage(last->GetGUID());
        context.ClearEvents();

        // Native AoE resolves each victim through DealDamage without a player
        // update between them. Exercise that exact boundary deterministically.
        auto hit = [&](Unit* target)
        {
            Unit::DealDamage(actor, target, TEST_DAMAGE, nullptr, SPELL_DIRECT_DAMAGE,
                SPELL_SCHOOL_MASK_FIRE, sSpellMgr->GetSpellInfo(SPELL_TEST_DAMAGE), false);
        };
        hit(seed);
        _firstBatchBank = runtime.overkill.bank;
        hit(first);  // A survives and spends the seed bank.
        hit(middle); // B dies and earns a new bank before either discharge runs.
        _lastBatchBank = runtime.overkill.bank;
        hit(last);   // C survives and spends the new bank.
        context.Expect(_firstBatchBank > 0 && _lastBatchBank > 0 && !middle->IsAlive(),
            "one native damage batch earns, spends, re-earns, and spends before update");
        SetStage(Stage::AwaitBatch);
    }

    void Finish(TestHarness::Context& context, bool finish = true)
    {
        if (_settingsSaved)
        {
            ClearTestSettings(context.GetActor());
            _settingsSaved = false;
        }

        if (Player* actor = context.GetActor())
            if (_equipped)
                Test::UnequipFabled(context, actor, Effect::Overkill);
        _equipped = false;

        context.DespawnAllDummies();
        _stage = Stage::Done;
        if (finish)
            context.Finish();
    }

    bool _settingsSaved = false;
    bool _equipped = false;
    Stage _stage = Stage::Done;
    uint32 _elapsedMs = 0;
    uint32 _healthBefore = 0;
    uint32 _baseDamage = 0;
    uint32 _killHealth = 0;
    uint64 _expectedBank = 0;
    uint64 _firstBatchBank = 0;
    uint64 _lastBatchBank = 0;
    ObjectGuid _baselineGuid;
    ObjectGuid _killGuid;
    ObjectGuid _spendGuid;
    ObjectGuid _expiryGuid;
    ObjectGuid _greyGuid;
};
} // namespace

std::unique_ptr<Script> MakeOverkill()
{
    return std::make_unique<OverkillScript>();
}

void RegisterOverkillTests()
{
    TestHarness::RegisterSuite("fabled-overkill",
        [] { return std::make_unique<OverkillTestSuite>(); });
}
} // namespace Fabled

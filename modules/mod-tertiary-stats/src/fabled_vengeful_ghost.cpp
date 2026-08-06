/*
 * mod-tertiary-stats: fabled effect "Vengeful Ghost".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellMgr.h"
#include "SpellScript.h"

#include <algorithm>
#include <limits>

namespace Fabled
{
namespace
{
constexpr TriggerCastFlags FABLED_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);

void RemovePhase(Player* player, Runtime& runtime)
{
    player->RemoveAurasDueToSpell(SPELL_VENGEFUL_PHASE);
    runtime.vengefulGhost.phaseActive = false;
    runtime.vengefulGhost.phaseEndMs = 0;
}

void ApplyPhase(Player* player, uint32 phaseMs)
{
    int32 durationMs = int32(std::min<uint32>(phaseMs, uint32(std::numeric_limits<int32>::max())));
    CustomSpellValues values;
    values.AddSpellMod(SPELLVALUE_AURA_DURATION, durationMs);
    player->CastCustomSpell(SPELL_VENGEFUL_PHASE, values, player, FABLED_TRIGGER_FLAGS);

    if (Aura* aura = player->GetAura(SPELL_VENGEFUL_PHASE))
    {
        aura->SetMaxDuration(durationMs);
        aura->SetDuration(durationMs);
    }
}

void StartLockout(Player* player)
{
    player->AddSpellCooldown(SPELL_VENGEFUL_COOLDOWN, 0,
        GetSettings(player).vengefulLockoutMs, false);
}

void DealNativeTestDamage(Creature* attacker, Player* victim)
{
    CustomSpellValues values;
    values.AddSpellMod(SPELLVALUE_BASE_POINT0, int32(std::min<uint64>(
        uint64(victim->GetHealth()) * 10, uint64(std::numeric_limits<int32>::max()))));
    values.AddSpellMod(SPELLVALUE_SCHOOL_MASK, SPELL_SCHOOL_MASK_SHADOW);
    attacker->CastCustomSpell(SPELL_IMPACT_DAMAGE, values, victim,
        TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS));
}

class spell_fabled_vengeful_guard final : public AuraScript
{
    PrepareAuraScript(spell_fabled_vengeful_guard);

    void CalculateAmount(AuraEffect const* /*effect*/, int32& amount,
        bool& /*canBeRecalculated*/)
    {
        amount = -1;
    }

    void Absorb(AuraEffect* /*effect*/, DamageInfo& damageInfo, uint32& absorbAmount)
    {
        Player* player = GetTarget()->ToPlayer();
        Runtime* runtime = player ? FindRuntime(player) : nullptr;
        if (!player || !runtime || !Has(*runtime, Effect::VengefulGhost)
            || !player->IsAlive() || damageInfo.GetDamage() < player->GetHealth())
            return;

        Unit* attacker = damageInfo.GetAttacker();
        if (attacker)
        {
            if (player->duel
                && (player->duel->Opponent == attacker
                    || player->duel->Opponent->GetGUID() == attacker->GetCharmerGUID()))
                return;
        }

        if (!runtime->vengefulGhost.phaseActive
            && player->HasSpellCooldown(SPELL_VENGEFUL_COOLDOWN))
            return;

        uint32 allowedDamage = player->GetHealth() > 1 ? player->GetHealth() - 1 : 0;
        absorbAmount = damageInfo.GetDamage() - allowedDamage;
        if (runtime->vengefulGhost.phaseActive)
            return;

        runtime->vengefulGhost.phaseActive = true;
        runtime->vengefulGhost.phaseEndMs = Now() + GetSettings(player).vengefulPhaseMs;
        StartLockout(player);
        ApplyPhase(player, GetSettings(player).vengefulPhaseMs);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(
            spell_fabled_vengeful_guard::CalculateAmount, EFFECT_0,
            SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_fabled_vengeful_guard::Absorb,
            EFFECT_0);
    }
};

class VengefulGhostScript final : public Script
{
public:
    VengefulGhostScript() : Script(Effect::VengefulGhost) { }

    void OnRefresh(Player* player, Runtime& runtime, bool active) override
    {
        if (active)
        {
            if (!player->HasAura(SPELL_VENGEFUL_GUARD))
                player->CastSpell(player, SPELL_VENGEFUL_GUARD, true);
            if (Aura* guard = player->GetAura(SPELL_VENGEFUL_GUARD))
            {
                guard->SetMaxDuration(-1);
                guard->SetDuration(-1);
            }
            if (!runtime.vengefulGhost.phaseActive && player->IsAlive())
            {
                if (Aura* aura = player->GetAura(SPELL_VENGEFUL_PHASE))
                {
                    runtime.vengefulGhost.phaseActive = true;
                    runtime.vengefulGhost.phaseEndMs = Now()
                        + uint64(std::max<int32>(aura->GetDuration(), 0));
                }
            }
            return;
        }

        player->RemoveAurasDueToSpell(SPELL_VENGEFUL_GUARD);
        bool phaseActive = runtime.vengefulGhost.phaseActive;
        RemovePhase(player, runtime);
        if (phaseActive && player->IsAlive())
            Unit::Kill(player, player);
    }

    void OnUpdate(Player* player, Runtime& runtime, uint32 /*diffMs*/, uint64 nowMs) override
    {
        if (!runtime.vengefulGhost.phaseActive || nowMs < runtime.vengefulGhost.phaseEndMs)
            return;

        RemovePhase(player, runtime);
        Unit::Kill(player, player);
    }

    void OnKill(Player* player, Runtime& runtime, Unit* /*victim*/, bool /*xpEligible*/) override
    {
        if (!runtime.vengefulGhost.phaseActive)
            return;

        RemovePhase(player, runtime);
        uint32 restoredHealth = uint32(float(player->GetMaxHealth())
            * GetSettings(player).vengefulResHealthPct / 100.0f);
        uint32 targetHealth = std::max<uint32>(restoredHealth, 1);
        if (targetHealth > player->GetHealth())
        {
            SpellInfo const* source = sSpellMgr->GetSpellInfo(SPELL_VENGEFUL_PHASE);
            HealInfo healInfo(player, player, targetHealth - player->GetHealth(), source,
                source ? source->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL);
            Unit::DealHeal(healInfo);
        }
    }

    void OnDeath(Player* player, Runtime& runtime) override
    {
        RemovePhase(player, runtime);
    }
};

class VengefulGhostTestSuite final : public TestHarness::Suite
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

        context.Expect(IsReady(), "fabled custom spells available");
        if (!IsReady())
        {
            context.Finish();
            return;
        }

        _settingsSaved = true;
        TestSettings(actor).vengefulPhaseMs = 100;
        TestSettings(actor).vengefulLockoutMs = 1000;
        TestSettings(actor).vengefulResHealthPct = 30.0f;

        Runtime& runtime = GetRuntime(actor);
        runtime.vengefulGhost.phaseActive = false;
        runtime.vengefulGhost.phaseEndMs = 0;
        actor->RemoveSpellCooldown(SPELL_VENGEFUL_COOLDOWN);
        actor->RemoveAurasDueToSpell(SPELL_VENGEFUL_PHASE);
        if (!actor->IsAlive())
        {
            actor->ResurrectPlayer(1.0f);
            actor->SpawnCorpseBones();
        }
        actor->SetFullHealth();

        _equipped = Test::EquipFabledTrinket(actor, Effect::VengefulGhost) != nullptr;
        context.Expect(_equipped, "Vengeful Ghost trinket equipped");
        context.Expect(actor->HasAura(SPELL_VENGEFUL_GUARD),
            "Vengeful Ghost equips its native absorb guard");
        if (!_equipped)
        {
            CleanupAndFinish(context);
            return;
        }

        Creature* firstDummy = context.SpawnDummy();
        context.Expect(firstDummy != nullptr, "first hostile dummy spawned");
        if (!firstDummy)
        {
            CleanupAndFinish(context);
            return;
        }
        _firstDummyGuid = firstDummy->GetGUID();
        context.Expect(context.Engage(_firstDummyGuid), "first dummy engaged");

        uint32 firstLethalHealth = actor->GetHealth();
        context.ClearEvents();
        DealNativeTestDamage(firstDummy, actor);
        context.Expect(actor->IsAlive() && actor->GetHealth() == 1,
            "redirected lethal hit leaves the actor alive at one health",
            "health=" + std::to_string(actor->GetHealth()));
        context.Expect(actor->HasAura(SPELL_VENGEFUL_PHASE),
            "Vengeful phase marker debuff is active");
        context.Expect(runtime.vengefulGhost.phaseActive && runtime.vengefulGhost.phaseEndMs > Now(),
            "Vengeful phase runtime is armed");
        runtime.vengefulGhost.phaseActive = false;
        runtime.vengefulGhost.phaseEndMs = 0;
        SetMask(actor, runtime.mask);
        context.Expect(runtime.vengefulGhost.phaseActive && runtime.vengefulGhost.phaseEndMs > Now(),
            "persisted phase aura reconstructs Vengeful runtime after a login-style refresh");

        TestHarness::Event const* firstFinalDamage = context.FindEvent(
            TestHarness::EventType::DamageFinal, firstDummy->GetGUID(), actor->GetGUID());
        context.Expect(firstFinalDamage && firstFinalDamage->amount == int64(firstLethalHealth - 1),
            "attacker accounting observes Vengeful-reduced final damage",
            "final=" + std::to_string(firstFinalDamage ? firstFinalDamage->amount : -1)
                + " expected=" + std::to_string(firstLethalHealth - 1));


        uint32 expectedRestoredHealth = uint32(float(actor->GetMaxHealth())
            * TestSettings(actor).vengefulResHealthPct / 100.0f);
        bool killed = context.Damage(_firstDummyGuid, firstDummy->GetHealth());
        context.Expect(killed, "actor kills the first dummy during the phase");
        context.Expect(actor->IsAlive() && actor->GetHealth() == expectedRestoredHealth,
            "phase kill restores thirty percent maximum health",
            "health=" + std::to_string(actor->GetHealth())
                + " expected=" + std::to_string(expectedRestoredHealth));
        context.Expect(!actor->HasAura(SPELL_VENGEFUL_PHASE)
                && !runtime.vengefulGhost.phaseActive && runtime.vengefulGhost.phaseEndMs == 0,
            "phase kill clears the marker and active state");
        context.Expect(actor->HasSpellCooldown(SPELL_VENGEFUL_COOLDOWN),
            "phase activation starts the native persisted lockout");

        actor->RemoveSpellCooldown(SPELL_VENGEFUL_COOLDOWN);
        context.Expect(!actor->HasSpellCooldown(SPELL_VENGEFUL_COOLDOWN),
            "test clears the lockout before retriggering");

        Creature* secondDummy = context.SpawnDummy(3.0f, 0.4f);
        context.Expect(secondDummy != nullptr, "second hostile dummy spawned");
        if (!secondDummy)
        {
            CleanupAndFinish(context);
            return;
        }
        _secondDummyGuid = secondDummy->GetGUID();
        context.Expect(context.Engage(_secondDummyGuid), "second dummy engaged");

        DealNativeTestDamage(secondDummy, actor);
        context.Expect(actor->IsAlive() && actor->GetHealth() == 1
                && runtime.vengefulGhost.phaseActive && actor->HasAura(SPELL_VENGEFUL_PHASE),
            "Vengeful phase retriggers after its lockout");

        AdvanceClock(actor, uint64(TestSettings(actor).vengefulPhaseMs) + 1);
        _stage = Stage::AwaitExpiry;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        if (_stage != Stage::AwaitExpiry)
            return;

        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Expect(false, "actor remains available for phase expiry");
            CleanupAndFinish(context);
            return;
        }

        if (actor->IsAlive())
            HandleUpdate(actor, diff);

        Runtime& runtime = GetRuntime(actor);
        context.Expect(!actor->IsAlive(), "expired Vengeful phase kills the actor for real");
        context.Expect(!runtime.vengefulGhost.phaseActive && runtime.vengefulGhost.phaseEndMs == 0
                && !actor->HasAura(SPELL_VENGEFUL_PHASE),
            "phase expiry clears its marker and active state");
        context.Expect(actor->HasSpellCooldown(SPELL_VENGEFUL_COOLDOWN),
            "phase expiry preserves the configured lockout");

        CleanupAndFinish(context);
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context);
    }

private:
    enum class Stage
    {
        AwaitExpiry,
        Done
    };

    void Cleanup(TestHarness::Context& context)
    {
        Player* actor = context.GetActor();
        if (actor)
        {
            if (_equipped)
            {
                Test::UnequipFabled(actor, Effect::VengefulGhost);
                _equipped = false;
            }
            actor->RemoveAurasDueToSpell(SPELL_VENGEFUL_PHASE);
            actor->RemoveSpellCooldown(SPELL_VENGEFUL_COOLDOWN);
            if (!actor->IsAlive())
            {
                actor->ResurrectPlayer(1.0f);
                actor->SpawnCorpseBones();
            }
            actor->SetFullHealth();
            actor->CombatStop(true);
        }

        if (_settingsSaved)
        {
            ClearTestSettings(context.GetActor());
            _settingsSaved = false;
        }
        context.DespawnAllDummies();
        _stage = Stage::Done;
    }

    void CleanupAndFinish(TestHarness::Context& context)
    {
        Cleanup(context);
        context.Finish();
    }

    Stage _stage = Stage::Done;
    ObjectGuid _firstDummyGuid;
    ObjectGuid _secondDummyGuid;
    bool _settingsSaved = false;
    bool _equipped = false;
};
} // namespace

void RegisterVengefulGhostSpellScripts()
{
    RegisterSpellScript(spell_fabled_vengeful_guard);
}

std::unique_ptr<Script> MakeVengefulGhost()
{
    return std::make_unique<VengefulGhostScript>();
}

void RegisterVengefulGhostTests()
{
    TestHarness::RegisterSuite("fabled-vengeful-ghost",
        [] { return std::make_unique<VengefulGhostTestSuite>(); });
}
} // namespace Fabled

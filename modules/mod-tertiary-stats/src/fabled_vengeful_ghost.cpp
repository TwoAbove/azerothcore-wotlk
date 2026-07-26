/*
 * mod-tertiary-stats: fabled effect "Vengeful Ghost".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellMgr.h"

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
    runtime.vengefulPhaseActive = false;
    runtime.vengefulPhaseEndMs = 0;
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
        GetSettings().vengefulLockoutMs, false);
}

class VengefulGhostScript final : public Script
{
public:
    VengefulGhostScript() : Script(Effect::VengefulGhost) { }

    void OnRefresh(Player* player, Runtime& runtime, bool active) override
    {
        if (active)
        {
            if (!runtime.vengefulPhaseActive && player->IsAlive())
            {
                if (Aura* aura = player->GetAura(SPELL_VENGEFUL_PHASE))
                {
                    runtime.vengefulPhaseActive = true;
                    runtime.vengefulPhaseEndMs = Now()
                        + uint64(std::max<int32>(aura->GetDuration(), 0));
                }
            }
            return;
        }

        bool phaseActive = runtime.vengefulPhaseActive;
        RemovePhase(player, runtime);
        if (phaseActive && player->IsAlive())
            Unit::Kill(player, player);
    }

    void OnUpdate(Player* player, Runtime& runtime, uint32 /*diffMs*/, uint64 nowMs) override
    {
        if (!runtime.vengefulPhaseActive || nowMs < runtime.vengefulPhaseEndMs)
            return;

        RemovePhase(player, runtime);
        Unit::Kill(player, player);
    }

    void OnKill(Player* player, Runtime& runtime, Unit* /*victim*/, bool /*xpEligible*/) override
    {
        if (!runtime.vengefulPhaseActive)
            return;

        RemovePhase(player, runtime);
        uint32 restoredHealth = uint32(float(player->GetMaxHealth())
            * GetSettings().vengefulResHealthPct / 100.0f);
        uint32 targetHealth = std::max<uint32>(restoredHealth, 1);
        if (targetHealth > player->GetHealth())
        {
            SpellInfo const* source = sSpellMgr->GetSpellInfo(SPELL_VENGEFUL_PHASE);
            HealInfo healInfo(player, player, targetHealth - player->GetHealth(), source,
                source ? source->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL);
            Unit::DealHeal(healInfo);
        }
    }

    void OnIncomingDamage(Player* player, Runtime& runtime, Unit* /*attacker*/, uint32& damage) override
    {
        if (!player->IsAlive())
            return;

        uint32 health = player->GetHealth();
        if (damage < health)
            return;

        if (runtime.vengefulPhaseActive)
        {
            damage = health > 1 ? health - 1 : 0;
            return;
        }

        if (player->HasSpellCooldown(SPELL_VENGEFUL_COOLDOWN))
            return;

        damage = health > 1 ? health - 1 : 0;
        runtime.vengefulPhaseActive = true;
        runtime.vengefulPhaseEndMs = Now() + GetSettings().vengefulPhaseMs;
        StartLockout(player);
        ApplyPhase(player, GetSettings().vengefulPhaseMs);
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

        _savedSettings = MutableSettings();
        _settingsSaved = true;
        MutableSettings().vengefulPhaseMs = 100;
        MutableSettings().vengefulLockoutMs = 1000;
        MutableSettings().vengefulResHealthPct = 30.0f;

        Runtime& runtime = GetRuntime(actor);
        runtime.vengefulPhaseActive = false;
        runtime.vengefulPhaseEndMs = 0;
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
        Unit::DealDamage(firstDummy, actor, actor->GetHealth(), nullptr, NODAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false, true, nullptr, DIRECT_DAMAGE);
        context.Expect(actor->IsAlive() && actor->GetHealth() == 1,
            "redirected lethal hit leaves the actor alive at one health",
            "health=" + std::to_string(actor->GetHealth()));
        context.Expect(actor->HasAura(SPELL_VENGEFUL_PHASE),
            "Vengeful phase marker debuff is active");
        context.Expect(runtime.vengefulPhaseActive && runtime.vengefulPhaseEndMs > Now(),
            "Vengeful phase runtime is armed");
        runtime.vengefulPhaseActive = false;
        runtime.vengefulPhaseEndMs = 0;
        SetMask(actor, runtime.mask);
        context.Expect(runtime.vengefulPhaseActive && runtime.vengefulPhaseEndMs > Now(),
            "persisted phase aura reconstructs Vengeful runtime after a login-style refresh");

        TestHarness::Event const* firstFinalDamage = context.FindEvent(
            TestHarness::EventType::DamageFinal, firstDummy->GetGUID(), actor->GetGUID());
        context.Expect(firstFinalDamage && firstFinalDamage->amount == int64(firstLethalHealth - 1),
            "attacker accounting observes Vengeful-reduced final damage",
            "final=" + std::to_string(firstFinalDamage ? firstFinalDamage->amount : -1)
                + " expected=" + std::to_string(firstLethalHealth - 1));


        uint32 expectedRestoredHealth = uint32(float(actor->GetMaxHealth())
            * MutableSettings().vengefulResHealthPct / 100.0f);
        bool killed = context.Damage(_firstDummyGuid, firstDummy->GetHealth());
        context.Expect(killed, "actor kills the first dummy during the phase");
        context.Expect(actor->IsAlive() && actor->GetHealth() == expectedRestoredHealth,
            "phase kill restores thirty percent maximum health",
            "health=" + std::to_string(actor->GetHealth())
                + " expected=" + std::to_string(expectedRestoredHealth));
        context.Expect(!actor->HasAura(SPELL_VENGEFUL_PHASE)
                && !runtime.vengefulPhaseActive && runtime.vengefulPhaseEndMs == 0,
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

        Unit::DealDamage(secondDummy, actor, actor->GetHealth(), nullptr, DIRECT_DAMAGE,
            SPELL_SCHOOL_MASK_NORMAL, nullptr, false, true);
        context.Expect(actor->IsAlive() && actor->GetHealth() == 1
                && runtime.vengefulPhaseActive && actor->HasAura(SPELL_VENGEFUL_PHASE),
            "Vengeful phase retriggers after its lockout");

        AdvanceClock(actor, uint64(MutableSettings().vengefulPhaseMs) + 1);
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
        context.Expect(!runtime.vengefulPhaseActive && runtime.vengefulPhaseEndMs == 0
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
            MutableSettings() = _savedSettings;
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
    Settings _savedSettings;
    ObjectGuid _firstDummyGuid;
    ObjectGuid _secondDummyGuid;
    bool _settingsSaved = false;
    bool _equipped = false;
};
} // namespace

std::unique_ptr<Script> MakeVengefulGhost()
{
    TestHarness::RegisterSuite("fabled-vengeful-ghost",
        [] { return std::make_unique<VengefulGhostTestSuite>(); });
    return std::make_unique<VengefulGhostScript>();
}
} // namespace Fabled

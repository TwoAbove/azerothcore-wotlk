/*
 * mod-tertiary-stats: fabled effect "Ghostwalk".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "Player.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <cmath>
#include <memory>
#include <string>

namespace Fabled
{
namespace
{
constexpr TriggerCastFlags FABLED_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);

void BreakFade(Player* player, Runtime& runtime)
{
    player->RemoveAurasDueToSpell(SPELL_GHOSTWALK_AURA);
    runtime.ghostwalk.fadeAtMs = 0;
}

void ApplyFade(Player* player)
{
    if (!player->IsAlive())
        return;

    CustomSpellValues values;
    values.AddSpellMod(SPELLVALUE_BASE_POINT0, int32(std::lround(GetSettings(player).ghostwalkSpeedPct)));
    values.AddSpellMod(SPELLVALUE_BASE_POINT1, int32(player->GetLevel()) * 5);
    player->CastCustomSpell(SPELL_GHOSTWALK_AURA, values, player, FABLED_TRIGGER_FLAGS);

    if (Aura* aura = player->GetAura(SPELL_GHOSTWALK_AURA))
    {
        aura->SetMaxDuration(-1);
        aura->SetDuration(-1);
    }
}

class GhostwalkScript final : public Script
{
public:
    GhostwalkScript() : Script(Effect::Ghostwalk) { }

    void OnRefresh(Player* player, Runtime& runtime, bool active) override
    {
        if (!active)
            BreakFade(player, runtime);
    }

    void OnUpdate(Player* player, Runtime& runtime, uint32 /*diffMs*/, uint64 nowMs) override
    {
        if (!player->IsAlive())
        {
            BreakFade(player, runtime);
            return;
        }

        if (player->IsInCombat())
            return;

        if (player->HasAura(SPELL_GHOSTWALK_AURA))
        {
            runtime.ghostwalk.fadeAtMs = 0;
            return;
        }

        if (!runtime.ghostwalk.fadeAtMs)
        {
            runtime.ghostwalk.fadeAtMs = nowMs + GetSettings(player).ghostwalkDelayMs;
            return;
        }

        if (nowMs < runtime.ghostwalk.fadeAtMs)
            return;

        runtime.ghostwalk.fadeAtMs = 0;
        ApplyFade(player);
    }

    void OnBeforeDealtDamage(Player* player, Runtime& runtime, Unit* /*victim*/,
        uint32 /*damage*/, SpellInfo const* /*spellInfo*/, DamageKind /*kind*/) override
    {
        BreakFade(player, runtime);
    }

    void OnSpellPrepare(Player* player, Runtime& runtime, Spell* spell) override
    {
        if (spell)
            BreakFade(player, runtime);
    }

    void OnCombatEnter(Player* player, Runtime& runtime) override
    {
        BreakFade(player, runtime);
    }
    void OnDeath(Player* player, Runtime& runtime) override
    {
        BreakFade(player, runtime);
    }

};

class GhostwalkTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled custom spells are available");
        if (!actor || !IsReady())
        {
            context.Finish();
            return;
        }
        _settingsSaved = true;
        TestSettings(actor).ghostwalkDelayMs = 6000;
        TestSettings(actor).ghostwalkSpeedPct = 40.0f;

        context.DespawnAllDummies();
        Test::UnequipFabled(actor, Effect::Ghostwalk);
        _equipped = Test::EquipFabledTrinket(actor, Effect::Ghostwalk) != nullptr;
        context.Expect(_equipped, "Ghostwalk fabled trinket equips");
        if (!_equipped)
        {
            Finish(context);
            return;
        }

        _baseRunSpeed = actor->GetSpeedRate(MOVE_RUN);
        context.Expect(!actor->HasAura(SPELL_GHOSTWALK_AURA),
            "Ghostwalk does not fade immediately after equip");

        HandleUpdate(actor, 0);
        context.Expect(GetRuntime(actor).ghostwalk.fadeAtMs != 0,
            "Ghostwalk arms its out-of-combat fade deadline");
        AdvanceClock(actor, GetSettings(actor).ghostwalkDelayMs + 1);
        _stage = Stage::AwaitFirstFade;
        _elapsedMs = 0;
    }

    void Update(TestHarness::Context& context, uint32 diffMs) override
    {
        if (_stage == Stage::Done)
            return;

        _elapsedMs += diffMs;
        if (_elapsedMs < 50)
            return;

        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Fail("headless actor remains available");
            Finish(context);
            return;
        }

        if (_stage == Stage::AwaitFirstFade)
        {
            HandleUpdate(actor, diffMs);
            Aura* aura = actor->GetAura(SPELL_GHOSTWALK_AURA);
            AuraEffect const* stealth = aura ? aura->GetEffect(EFFECT_1) : nullptr;
            context.Expect(aura != nullptr, "Ghostwalk fades after six seconds out of combat");
            context.Expect(aura && aura->GetDuration() == -1,
                "Ghostwalk fade persists until a break condition");
            context.Expect(actor->GetSpeedRate(MOVE_RUN) > _baseRunSpeed,
                "Ghostwalk increases run speed",
                "before=" + std::to_string(_baseRunSpeed)
                    + ",after=" + std::to_string(actor->GetSpeedRate(MOVE_RUN)));
            context.Expect(stealth && stealth->GetAuraType() == SPELL_AURA_MOD_STEALTH
                    && stealth->GetAmount() == int32(actor->GetLevel()) * 5,
                "Ghostwalk applies level-scaled stealth");

            Creature* dummy = context.SpawnDummy();
            bool engaged = dummy && context.Engage(dummy->GetGUID());
            context.Expect(engaged, "hostile test target engages the actor");
            if (!engaged)
            {
                Finish(context);
                return;
            }

            HandleUpdate(actor, 0);
            context.Expect(!actor->HasAura(SPELL_GHOSTWALK_AURA),
                "entering combat breaks Ghostwalk");
            context.Expect(GetRuntime(actor).ghostwalk.fadeAtMs == 0,
                "combat break clears the fade deadline");

            context.DespawnAllDummies();
            HandleUpdate(actor, 0);
            context.Expect(GetRuntime(actor).ghostwalk.fadeAtMs != 0,
                "leaving combat re-arms Ghostwalk");
            AdvanceClock(actor, GetSettings(actor).ghostwalkDelayMs + 1);
            _stage = Stage::AwaitRefade;
            _elapsedMs = 0;
            return;
        }

        HandleUpdate(actor, diffMs);
        context.Expect(actor->HasAura(SPELL_GHOSTWALK_AURA),
            "Ghostwalk automatically re-fades after combat");
        SpellInfo const* positiveInfo = sSpellMgr->GetSpellInfo(1243); // Power Word: Fortitude.
        context.Expect(positiveInfo != nullptr, "positive Ghostwalk test spell resolves");
        if (positiveInfo)
        {
            Spell positiveSpell(actor, positiveInfo, TRIGGERED_NONE);
            HandleSpellPrepare(actor, &positiveSpell);
            context.Expect(!actor->HasAura(SPELL_GHOSTWALK_AURA),
                "a positive player cast breaks Ghostwalk");
            ApplyFade(actor);
        }
        Unit::Kill(actor, actor);
        context.Expect(!actor->HasAura(SPELL_GHOSTWALK_AURA)
                && GetRuntime(actor).ghostwalk.fadeAtMs == 0,
            "death clears Ghostwalk and its pending lifecycle");
        HandleUpdate(actor, 0);
        context.Expect(!actor->HasAura(SPELL_GHOSTWALK_AURA),
            "Ghostwalk cannot reapply while dead");
        actor->ResurrectPlayer(1.0f);
        actor->SpawnCorpseBones();
        HandleUpdate(actor, 0);
        context.Expect(!actor->HasAura(SPELL_GHOSTWALK_AURA)
                && GetRuntime(actor).ghostwalk.fadeAtMs != 0,
            "resurrection starts a fresh Ghostwalk fade delay");
        Finish(context);
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context);
    }

private:
    enum class Stage
    {
        AwaitFirstFade,
        AwaitRefade,
        Done
    };

    void Cleanup(TestHarness::Context& context)
    {
        context.DespawnAllDummies();
        if (Player* actor = context.GetActor())
        {
            if (_equipped)
                Test::UnequipFabled(actor, Effect::Ghostwalk);
            else
                actor->RemoveAurasDueToSpell(SPELL_GHOSTWALK_AURA);
        }
        _equipped = false;

        if (_settingsSaved)
            ClearTestSettings(context.GetActor());
        _settingsSaved = false;
    }

    void Finish(TestHarness::Context& context)
    {
        Cleanup(context);
        _stage = Stage::Done;
        context.Finish();
    }

    Stage _stage = Stage::Done;
    bool _settingsSaved = false;
    bool _equipped = false;
    float _baseRunSpeed = 1.0f;
    uint32 _elapsedMs = 0;
};
} // namespace

std::unique_ptr<Script> MakeGhostwalk()
{
    return std::make_unique<GhostwalkScript>();
}

void RegisterGhostwalkTests()
{
    TestHarness::RegisterSuite("fabled-ghostwalk",
        [] { return std::make_unique<GhostwalkTestSuite>(); });
}
} // namespace Fabled

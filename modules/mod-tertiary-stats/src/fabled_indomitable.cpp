/*
 * mod-tertiary-stats: fabled effect "Indomitable".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "Player.h"
#include "SpellAuraDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "test_harness.h"

#include <memory>

namespace Fabled
{
namespace
{
constexpr uint32 TEST_ROOT_SPELL = 339; // Entangling Roots (rank 1).
constexpr uint32 TEST_STUN_SPELL = 853; // Hammer of Justice (rank 1).

constexpr uint64 HARD_CONTROL_MECHANIC_MASK =
    (1ULL << MECHANIC_CHARM) |
    (1ULL << MECHANIC_DISORIENTED) |
    (1ULL << MECHANIC_FEAR) |
    (1ULL << MECHANIC_SLEEP) |
    (1ULL << MECHANIC_STUN) |
    (1ULL << MECHANIC_KNOCKOUT) |
    (1ULL << MECHANIC_POLYMORPH) |
    (1ULL << MECHANIC_BANISH) |
    (1ULL << MECHANIC_SHACKLE) |
    (1ULL << MECHANIC_TURN) |
    (1ULL << MECHANIC_HORROR) |
    (1ULL << MECHANIC_SAPPED);

bool IsHardControlAura(SpellInfo const* spellInfo, uint8 effectMask)
{
    if (spellInfo->GetSpellMechanicMaskByEffectMask(effectMask) & HARD_CONTROL_MECHANIC_MASK)
        return true;

    for (uint8 index = 0; index < MAX_SPELL_EFFECTS; ++index)
    {
        if (!(effectMask & (1u << index)))
            continue;

        switch (spellInfo->Effects[index].ApplyAuraName)
        {
            case SPELL_AURA_MOD_POSSESS:
            case SPELL_AURA_MOD_CONFUSE:
            case SPELL_AURA_MOD_CHARM:
            case SPELL_AURA_MOD_FEAR:
            case SPELL_AURA_MOD_STUN:
            case SPELL_AURA_MOD_POSSESS_PET:
            case SPELL_AURA_AOE_CHARM:
                return true;
            default:
                break;
        }
    }

    return false;
}

void SetReady(Runtime& runtime, bool ready)
{
    runtime.indomitable.ready = ready;
}

class IndomitableScript final : public Script
{
public:
    IndomitableScript() : Script(Effect::Indomitable) { }

    void OnRefresh(Player* player, Runtime& runtime, bool active) override
    {
        if (!active)
            SetReady(runtime, false);
        else if (!player->IsInCombat())
            SetReady(runtime, true);
    }

    void ModifySpellEffectImmunityMask(Player* player, Runtime& runtime,
        Unit* caster, SpellInfo const* spellInfo, uint8 candidateEffectMask,
        uint8& immuneEffectMask) override
    {
        if (!runtime.indomitable.ready || !caster || caster == player
            || player->IsFriendlyTo(caster)
            || spellInfo->HasAttribute(SPELL_ATTR0_NO_IMMUNITIES))
            return;

        uint8 addedMask = 0;
        for (uint8 index = 0; index < MAX_SPELL_EFFECTS; ++index)
        {
            uint8 effectBit = 1u << index;
            if ((candidateEffectMask & effectBit)
                && IsHardControlAura(spellInfo, effectBit)
                && !player->IsImmunedToSpellEffect(spellInfo, index, caster))
                addedMask |= effectBit;
        }
        if (!addedMask)
            return;

        immuneEffectMask |= addedMask;
        SetReady(runtime, false);
        player->SendPlaySpellVisual(SPELL_VISUAL_KIT_INDOMITABLE);
    }

    void OnCombatExit(Player* /*player*/, Runtime& runtime) override
    {
        SetReady(runtime, true);
    }
};

class IndomitableTestSuite final : public TestHarness::Suite
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

        actor->CombatStop(true);
        actor->RemoveAurasDueToSpell(TEST_ROOT_SPELL);
        actor->RemoveAurasDueToSpell(TEST_STUN_SPELL);
        _equipped = Test::EquipFabledTrinket(actor, Effect::Indomitable) != nullptr;
        context.Expect(_equipped && GetRuntime(actor).indomitable.ready,
            "Indomitable readies when equipped outside combat");

        actor->CastSpell(actor, TEST_STUN_SPELL, true);
        context.Expect(actor->HasAura(TEST_STUN_SPELL)
                && GetRuntime(actor).indomitable.ready,
            "friendly control does not block or consume the charge");
        actor->RemoveAurasDueToSpell(TEST_STUN_SPELL);

        Creature* dummy = context.SpawnDummy(3.0f, 0.0f);
        context.Expect(dummy && context.Engage(dummy->GetGUID()),
            "hostile control caster is engaged");
        if (!_equipped || !dummy)
        {
            Finish(context);
            return;
        }

        _dummyGuid = dummy->GetGUID();
        _stage = Stage::ApplyRoot;
        _elapsedMs = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        if (_stage == Stage::Done)
            return;

        _elapsedMs += diff;
        if (_elapsedMs < 100)
            return;
        _elapsedMs = 0;

        Player* actor = context.GetActor();
        Creature* dummy = context.GetCreature(_dummyGuid);
        if (!actor || !dummy)
        {
            context.Fail("Indomitable test objects remain available");
            Finish(context);
            return;
        }

        if (_stage == Stage::ApplyRoot)
        {
            dummy->CastSpell(actor, TEST_ROOT_SPELL, true);
            context.Expect(actor->HasAura(TEST_ROOT_SPELL)
                    && GetRuntime(actor).indomitable.ready,
                "roots apply without consuming Indomitable");
            actor->RemoveAurasDueToSpell(TEST_ROOT_SPELL);
            _stage = Stage::BlockStun;
            return;
        }

        if (_stage == Stage::BlockStun)
        {
            dummy->CastSpell(actor, TEST_STUN_SPELL, true);
            context.Expect(!actor->HasAura(TEST_STUN_SPELL)
                    && !actor->HasUnitState(UNIT_STATE_STUNNED)
                    && !GetRuntime(actor).indomitable.ready,
                "the first hostile hard control aura is rejected before application");
            _stage = Stage::AllowSecondStun;
            return;
        }

        dummy->CastSpell(actor, TEST_STUN_SPELL, true);
        context.Expect(actor->HasAura(TEST_STUN_SPELL)
                && actor->HasUnitState(UNIT_STATE_STUNNED),
            "a second hard control aura in the same combat applies normally");
        Finish(context);
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context);
    }

private:
    enum class Stage
    {
        ApplyRoot,
        BlockStun,
        AllowSecondStun,
        Done
    };

    void Cleanup(TestHarness::Context& context)
    {
        if (Player* actor = context.GetActor())
        {
            actor->RemoveAurasDueToSpell(TEST_ROOT_SPELL);
            actor->RemoveAurasDueToSpell(TEST_STUN_SPELL);
            actor->CombatStop(true);
            if (_equipped)
                Test::UnequipFabled(actor, Effect::Indomitable);
        }
        _equipped = false;
        context.DespawnAllDummies();
    }

    void Finish(TestHarness::Context& context)
    {
        Cleanup(context);
        _stage = Stage::Done;
        context.Finish();
    }

    ObjectGuid _dummyGuid;
    Stage _stage = Stage::Done;
    uint32 _elapsedMs = 0;
    bool _equipped = false;
};
} // namespace

std::unique_ptr<Script> MakeIndomitable()
{
    return std::make_unique<IndomitableScript>();
}

void RegisterIndomitableTests()
{
    TestHarness::RegisterSuite("fabled-indomitable",
        [] { return std::make_unique<IndomitableTestSuite>(); });
}
} // namespace Fabled

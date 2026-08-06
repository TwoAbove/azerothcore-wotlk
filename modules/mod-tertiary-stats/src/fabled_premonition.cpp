/*
 * mod-tertiary-stats: fabled effect "Premonition".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "test_harness.h"

#include <memory>
#include <string>

namespace Fabled
{
namespace
{
constexpr TriggerCastFlags FABLED_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);
constexpr uint32 TEST_MANA_SPELL = 133; // Fireball rank 1.
constexpr uint32 TEST_WINDOW_MS = 4000;

class PremonitionScript final : public Script
{
public:
    PremonitionScript() : Script(Effect::Premonition) { }

    void OnRefresh(Player* player, Runtime& /*runtime*/, bool active) override
    {
        if (!active)
            player->RemoveAurasDueToSpell(SPELL_PREMONITION_AURA);
    }

    void OnKill(Player* player, Runtime& /*runtime*/, Unit* /*victim*/, bool xpEligible) override
    {
        if (!xpEligible)
            return;

        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, -100);
        player->CastCustomSpell(SPELL_PREMONITION_AURA, values, player, FABLED_TRIGGER_FLAGS);
        if (Aura* aura = player->GetAura(SPELL_PREMONITION_AURA))
        {
            int32 duration = int32(GetSettings(player).premonitionWindowMs);
            aura->SetMaxDuration(duration);
            aura->SetDuration(duration);
        }
    }
};

class PremonitionTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        if (!actor)
        {
            Finish(context);
            return;
        }
        _settingsSaved = true;
        TestSettings(actor).premonitionWindowMs = TEST_WINDOW_MS;

        context.Expect(IsReady(), "fabled custom spells are ready");
        if (!IsReady())
        {
            Finish(context);
            return;
        }

        _equipped = Test::EquipFabledTrinket(actor, Effect::Premonition) != nullptr;
        context.Expect(_equipped, "Premonition trinket equipped");
        if (!_equipped)
        {
            Finish(context);
            return;
        }

        SpellInfo const* manaSpell = sSpellMgr->GetSpellInfo(TEST_MANA_SPELL);
        int32 costBefore = manaSpell
            ? manaSpell->CalcPowerCost(actor, manaSpell->GetSchoolMask()) : 0;
        context.Expect(manaSpell != nullptr && costBefore > 0,
            "mana-costing test spell has a baseline cost",
            "cost=" + std::to_string(costBefore));

        Creature* eligible = context.SpawnDummy();
        context.Expect(eligible != nullptr, "XP-eligible dummy spawned");
        if (!eligible)
        {
            Finish(context);
            return;
        }
        eligible->SetLevel(actor->GetLevel());
        context.Expect(actor->isHonorOrXPTarget(eligible),
            "same-level dummy is XP-eligible");
        Unit::Kill(actor, eligible);

        Aura* aura = actor->GetAura(SPELL_PREMONITION_AURA);
        AuraEffect const* effect = aura ? aura->GetEffect(EFFECT_0) : nullptr;
        int32 duration = aura ? aura->GetDuration() : 0;
        context.Expect(aura && effect && effect->GetAmount() == -100
                && duration > int32(TEST_WINDOW_MS - 500)
                && duration <= int32(TEST_WINDOW_MS),
            "eligible killing blow opens the four-second free-cost window",
            "amount=" + std::to_string(effect ? effect->GetAmount() : 0)
                + " duration=" + std::to_string(duration));

        int32 costDuring = manaSpell
            ? manaSpell->CalcPowerCost(actor, manaSpell->GetSchoolMask()) : -1;
        context.Expect(manaSpell && costBefore > 0 && costDuring == 0,
            "Premonition zeroes an all-school mana cost",
            "before=" + std::to_string(costBefore)
                + " during=" + std::to_string(costDuring));

        if (aura)
            aura->SetDuration(500);
        Creature* refreshTarget = context.SpawnDummy(4.0f, 0.25f);
        context.Expect(refreshTarget != nullptr, "refresh dummy spawned");
        if (!refreshTarget)
        {
            Finish(context);
            return;
        }
        refreshTarget->SetLevel(actor->GetLevel());
        Unit::Kill(actor, refreshTarget);
        aura = actor->GetAura(SPELL_PREMONITION_AURA);
        context.Expect(aura && aura->GetDuration() > int32(TEST_WINDOW_MS - 500),
            "subsequent eligible killing blow refreshes the window",
            "duration=" + std::to_string(aura ? aura->GetDuration() : 0));

        actor->RemoveAurasDueToSpell(SPELL_PREMONITION_AURA);
        Creature* grey = context.SpawnDummy(5.0f, -0.25f);
        context.Expect(grey != nullptr, "grey dummy spawned");
        if (!grey)
        {
            Finish(context);
            return;
        }
        grey->SetLevel(1);
        context.Expect(!actor->isHonorOrXPTarget(grey),
            "level-one dummy is not XP-eligible");
        Unit::Kill(actor, grey);
        context.Expect(!actor->HasAura(SPELL_PREMONITION_AURA),
            "grey killing blow does not open or refresh the window");

        Creature* unequipTarget = context.SpawnDummy(6.0f, 0.5f);
        context.Expect(unequipTarget != nullptr, "unequip-removal dummy spawned");
        if (!unequipTarget)
        {
            Finish(context);
            return;
        }
        unequipTarget->SetLevel(actor->GetLevel());
        Unit::Kill(actor, unequipTarget);
        context.Expect(actor->HasAura(SPELL_PREMONITION_AURA),
            "window is active before unequipping");

        Test::UnequipFabled(actor, Effect::Premonition);
        _equipped = false;
        context.Expect(!actor->HasAura(SPELL_PREMONITION_AURA),
            "unequipping Premonition removes its window aura");

        Finish(context);
    }
    void Update(TestHarness::Context& /*context*/, uint32 /*diff*/) override { }


private:
    void Finish(TestHarness::Context& context)
    {
        if (Player* actor = context.GetActor())
        {
            if (_equipped)
                Test::UnequipFabled(actor, Effect::Premonition);
            else
                actor->RemoveAurasDueToSpell(SPELL_PREMONITION_AURA);
        }
        _equipped = false;

        if (_settingsSaved)
            ClearTestSettings(context.GetActor());
        _settingsSaved = false;

        context.DespawnAllDummies();
        context.Finish();
    }

    bool _settingsSaved = false;
    bool _equipped = false;
};
} // namespace

std::unique_ptr<Script> MakePremonition()
{
    return std::make_unique<PremonitionScript>();
}

void RegisterPremonitionTests()
{
    TestHarness::RegisterSuite("fabled-premonition",
        [] { return std::make_unique<PremonitionTestSuite>(); });
}
} // namespace Fabled

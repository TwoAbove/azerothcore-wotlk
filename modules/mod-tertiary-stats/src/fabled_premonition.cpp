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

#include <array>
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

        _equipped = Test::EquipFabled(context, actor, Effect::Premonition) != nullptr;
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

        Test::UnequipFabled(context, actor, Effect::Premonition);
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
                Test::UnequipFabled(context, actor, Effect::Premonition);
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

class PremonitionRuneTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        // InitRunes allocates private state without a reset API; changing a mage's
        // class in place cannot safely restore that state. Use a real DK actor.
        context.Expect(actor && actor->IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_ABILITY),
            "rune regression requires an isolated Death Knight harness actor");
        if (!actor || !actor->IsClass(CLASS_DEATH_KNIGHT, CLASS_CONTEXT_ABILITY))
        {
            context.Finish();
            return;
        }
        constexpr uint32 bloodBoil = 48721;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(bloodBoil);
        context.Expect(IsReady() && info && info->PowerType == POWER_RUNE && info->RuneCostID,
            "Blood Boil and Premonition rune-test prerequisites are available");
        if (!IsReady() || !info || info->PowerType != POWER_RUNE || !info->RuneCostID)
        {
            context.Finish();
            return;
        }
        _saved = true;
        _runicPower = actor->GetPower(POWER_RUNIC_POWER);
        for (uint8 i = 0; i < MAX_RUNES; ++i)
        {
            _cooldowns[i] = actor->GetRuneCooldown(i);
            actor->SetRuneCooldown(i, 10000);
        }
        actor->RemoveAurasDueToSpell(SPELL_PREMONITION_AURA);
        // Ignore only timing restrictions, not resource checks or consumption.
        constexpr TriggerCastFlags flags = TriggerCastFlags(
            TRIGGERED_IGNORE_GCD | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD);
        context.Expect(actor->CastSpell(actor, bloodBoil, flags) == SPELL_FAILED_NO_POWER,
            "without Premonition exhausted runes reject Blood Boil");
        actor->CastCustomSpell(SPELL_PREMONITION_AURA, SPELLVALUE_BASE_POINT0, -1, actor, true);
        context.Expect(actor->CastSpell(actor, bloodBoil, flags) == SPELL_FAILED_NO_POWER,
            "a partial power discount cannot round an indivisible rune cost down to zero");
        actor->RemoveAurasDueToSpell(SPELL_PREMONITION_AURA);

        _equipped = Test::EquipFabled(context, actor, Effect::Premonition) != nullptr;
        Creature* eligible = context.SpawnDummy();
        context.Expect(_equipped && eligible, "rune-test trinket and eligible target are available");
        if (!_equipped || !eligible)
        {
            Finish(context);
            return;
        }
        eligible->SetLevel(actor->GetLevel());
        Unit::Kill(actor, eligible);
        context.Expect(actor->HasAura(SPELL_PREMONITION_AURA),
            "eligible kill opens the rune free-cost window");

        CheckFreeCast(context, bloodBoil, flags, 10000);
        for (uint8 i = 0; i < MAX_RUNES; ++i)
            actor->SetRuneCooldown(i, 0);
        CheckFreeCast(context, bloodBoil, flags, 0);
        Finish(context);
    }

    void Update(TestHarness::Context&, uint32) override { }
    void Cancel(TestHarness::Context& context) override { Finish(context); }

private:
    void CheckFreeCast(TestHarness::Context& context, uint32 spellId,
        TriggerCastFlags flags, uint32 expectedCooldown)
    {
        Player* actor = context.GetActor();
        context.ClearEvents();
        SpellCastResult result = actor->CastSpell(actor, spellId, flags);
        context.Expect(result == SPELL_CAST_OK
                && context.FindEvent(TestHarness::EventType::Cast,
                    actor->GetGUID(), ObjectGuid::Empty, spellId),
            expectedCooldown ? "Premonition completes Blood Boil with exhausted runes"
                             : "Premonition completes Blood Boil with available runes");
        bool unchanged = true;
        for (uint8 i = 0; i < MAX_RUNES; ++i)
            unchanged = unchanged && actor->GetRuneCooldown(i) == expectedCooldown;
        context.Expect(unchanged, expectedCooldown
            ? "free cast does not change exhausted rune cooldowns"
            : "free cast consumes no available runes");
    }

    void Finish(TestHarness::Context& context)
    {
        if (Player* actor = context.GetActor())
        {
            if (_saved)
            {
                for (uint8 i = 0; i < MAX_RUNES; ++i)
                    actor->SetRuneCooldown(i, _cooldowns[i]);
                actor->SetPower(POWER_RUNIC_POWER, _runicPower);
            }
            if (_equipped)
                Test::UnequipFabled(context, actor, Effect::Premonition);
            actor->RemoveAurasDueToSpell(SPELL_PREMONITION_AURA);
        }
        _saved = false;
        _equipped = false;
        context.DespawnAllDummies();
        context.Finish();
    }

    std::array<uint32, MAX_RUNES> _cooldowns{};
    uint32 _runicPower = 0;
    bool _saved = false;
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
    TestHarness::RegisterSuite("fabled-premonition-runes",
        [] { return std::make_unique<PremonitionRuneTestSuite>(); });
}
} // namespace Fabled

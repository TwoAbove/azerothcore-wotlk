/*
 * mod-tertiary-stats: fabled effect "Leviathan's Gift".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Player.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellDefines.h"
#include "test_harness.h"

#include <cmath>
#include <memory>
#include <string>

namespace Fabled
{
namespace
{
constexpr TriggerCastFlags FABLED_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);
void RefreshSwimBonus(Player* player)
{
    Aura* leviathan = player ? player->GetAura(SPELL_LEVIATHAN_PASSIVE) : nullptr;
    AuraEffect* swim = leviathan ? leviathan->GetEffect(EFFECT_1) : nullptr;
    if (!swim)
        return;

    int32 amount = int32(std::lround(GetSettings(player).leviathanSwimPct));
    for (AuraEffect const* effect :
        player->GetAuraEffectsByType(SPELL_AURA_MOD_INCREASE_SWIM_SPEED))
    {
        if (effect->GetId() != SPELL_LEVIATHAN_PASSIVE && effect->GetAmount() > 0)
            amount += effect->GetAmount();
    }

    if (swim->GetAmount() != amount)
        swim->ChangeAmount(amount);
}


class LeviathansGiftScript final : public Script
{
public:
    LeviathansGiftScript() : Script(Effect::LeviathansGift) { }

    void OnRefresh(Player* player, Runtime& /*runtime*/, bool active) override
    {
        player->RemoveAurasDueToSpell(SPELL_LEVIATHAN_PASSIVE);
        if (!active)
            return;

        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT1, int32(std::lround(GetSettings(player).leviathanSwimPct)));
        player->CastCustomSpell(SPELL_LEVIATHAN_PASSIVE, values, player, FABLED_TRIGGER_FLAGS);

        if (Aura* aura = player->GetAura(SPELL_LEVIATHAN_PASSIVE))
        {
            aura->SetMaxDuration(-1);
            aura->SetDuration(-1);
            RefreshSwimBonus(player);
        }
    }
    void OnUpdate(Player* player, Runtime& runtime, uint32 /*diffMs*/, uint64 /*nowMs*/) override
    {
        if (!player->IsAlive())
            return;
        if (!player->HasAura(SPELL_LEVIATHAN_PASSIVE))
            OnRefresh(player, runtime, true);
        else
            RefreshSwimBonus(player);
    }

};

class LeviathansGiftTestSuite final : public TestHarness::Suite
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
        actor->RemoveAurasDueToSpell(SPELL_LEVIATHAN_PASSIVE);
        _baseSwimRate = actor->GetSpeedRate(MOVE_SWIM);

        _equipped = Test::EquipFabledTrinket(actor, Effect::LeviathansGift) != nullptr;
        context.Expect(_equipped, "Leviathan's Gift trinket equips");
        if (!_equipped)
        {
            Cleanup(context, true);
            return;
        }

        _stage = Stage::CheckEquipped;
        _elapsed = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        _elapsed += diff;
        if (_elapsed < 100)
            return;

        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Fail("headless actor remains available");
            Cleanup(context, true);
            return;
        }

        if (_stage == Stage::CheckEquipped)
        {
            Aura* aura = actor->GetAura(SPELL_LEVIATHAN_PASSIVE);
            float actualRate = actor->GetSpeedRate(MOVE_SWIM);
            float expectedRate = _baseSwimRate * (1.0f + GetSettings(actor).leviathanSwimPct / 100.0f);
            context.Expect(aura != nullptr && aura->GetMaxDuration() == -1 && aura->GetDuration() == -1,
                "Leviathan's Gift applies its permanent passive aura");
            context.Expect(actor->HasAuraType(SPELL_AURA_WATER_BREATHING),
                "Leviathan's Gift grants water breathing");
            context.Expect(!actor->HasAuraType(SPELL_AURA_WATER_WALK)
                    && !actor->HasAuraType(SPELL_AURA_UNDERWATER_WALKING),
                "Leviathan's Gift does not grant water walking or underwater walking");
            context.Expect(std::fabs(actualRate - expectedRate) < 0.02f,
                "Leviathan's Gift increases swim speed by its configured percentage",
                "base=" + std::to_string(_baseSwimRate) + " actual=" + std::to_string(actualRate)
                    + " expected=" + std::to_string(expectedRate));
            CustomSpellValues fleetfoot;
            fleetfoot.AddSpellMod(SPELLVALUE_BASE_POINT2, 20);
            actor->CastCustomSpell(82002, fleetfoot, actor, FABLED_TRIGGER_FLAGS);
            RefreshSwimBonus(actor);
            float additiveRate = actor->GetSpeedRate(MOVE_SWIM);
            float expectedAdditiveRate = _baseSwimRate
                * (1.0f + (GetSettings(actor).leviathanSwimPct + 20.0f) / 100.0f);
            context.Expect(std::fabs(additiveRate - expectedAdditiveRate) < 0.02f,
                "Leviathan adds Fleetfoot to its swim-speed modifier",
                "actual=" + std::to_string(additiveRate)
                    + " expected=" + std::to_string(expectedAdditiveRate));
            actor->RemoveAurasDueToSpell(82002);
            RefreshSwimBonus(actor);

            Unit::Kill(actor, actor);
            _stage = Stage::CheckDeath;
            _elapsed = 0;
            return;
        }

        if (_stage == Stage::CheckDeath)
        {
            context.Expect(!actor->HasAura(SPELL_LEVIATHAN_PASSIVE),
                "death removes Leviathan's Gift from the dead player");
            actor->ResurrectPlayer(1.0f);
            actor->SpawnCorpseBones();
            HandleUpdate(actor, diff);
            _stage = Stage::CheckResurrected;
            _elapsed = 0;
            return;
        }

        if (_stage == Stage::CheckResurrected)
        {
            context.Expect(actor->HasAura(SPELL_LEVIATHAN_PASSIVE),
                "Leviathan's Gift reapplies after resurrection");
            Test::UnequipFabled(actor, Effect::LeviathansGift);
            _equipped = false;
            _stage = Stage::CheckUnequipped;
            _elapsed = 0;
            return;
        }

        if (_stage == Stage::CheckUnequipped)
        {
            float actualRate = actor->GetSpeedRate(MOVE_SWIM);
            context.Expect(!actor->HasAura(SPELL_LEVIATHAN_PASSIVE),
                "unequipping removes the Leviathan's Gift aura");
            context.Expect(std::fabs(actualRate - _baseSwimRate) < 0.02f,
                "unequipping restores the original swim speed",
                "base=" + std::to_string(_baseSwimRate) + " actual=" + std::to_string(actualRate));
            Cleanup(context, true);
        }
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context, false);
    }

private:
    enum class Stage
    {
        CheckEquipped,
        CheckDeath,
        CheckResurrected,
        CheckUnequipped,
        Done
    };

    void Cleanup(TestHarness::Context& context, bool finish)
    {
        if (_stage == Stage::Done)
            return;

        if (_equipped)
            if (Player* actor = context.GetActor())
                Test::UnequipFabled(actor, Effect::LeviathansGift);
        _equipped = false;

        context.DespawnAllDummies();
        _stage = Stage::Done;
        if (finish)
            context.Finish();
    }

    Stage _stage = Stage::Done;
    float _baseSwimRate = 1.0f;
    uint32 _elapsed = 0;
    bool _equipped = false;
};
} // namespace

std::unique_ptr<Script> MakeLeviathansGift()
{
    return std::make_unique<LeviathansGiftScript>();
}

void RegisterLeviathansGiftTests()
{
    TestHarness::RegisterSuite("fabled-leviathans-gift",
        [] { return std::make_unique<LeviathansGiftTestSuite>(); });
}
} // namespace Fabled

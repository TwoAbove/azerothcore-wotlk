/*
 * mod-tertiary-stats: fabled effect "Momentum".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "test_harness.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace Fabled
{
namespace
{
constexpr TriggerCastFlags FABLED_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);

class MomentumScript final : public Script
{
public:
    MomentumScript() : Script(Effect::Momentum) { }

    void OnRefresh(Player* player, Runtime& /*runtime*/, bool active) override
    {
        if (!active)
            player->RemoveAurasDueToSpell(SPELL_MOMENTUM_AURA);
    }

    void OnKill(Player* player, Runtime& /*runtime*/, Unit* /*victim*/, bool /*xpEligible*/) override
    {
        Settings const& settings = GetSettings(player);
        Aura* aura = player->GetAura(SPELL_MOMENTUM_AURA);
        if (aura)
        {
            uint32 nextStack = std::min<uint32>(uint32(aura->GetStackAmount()) + 1,
                settings.momentumMaxStacks);
            aura->SetStackAmount(uint8(nextStack));
        }
        else
        {
            int32 amount = int32(std::lround(settings.momentumPctPerStack));
            CustomSpellValues values;
            values.AddSpellMod(SPELLVALUE_BASE_POINT0, amount);
            values.AddSpellMod(SPELLVALUE_BASE_POINT1, amount);
            values.AddSpellMod(SPELLVALUE_BASE_POINT2, amount);
            player->CastCustomSpell(SPELL_MOMENTUM_AURA, values, player, FABLED_TRIGGER_FLAGS);
            aura = player->GetAura(SPELL_MOMENTUM_AURA);
        }

        if (aura)
        {
            int32 duration = int32(settings.momentumWindowMs);
            aura->SetMaxDuration(duration);
            aura->SetDuration(duration);
        }
    }
};

class MomentumTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        _actor = context.GetActor();
        context.Expect(_actor != nullptr, "headless actor available");
        if (!_actor)
        {
            context.Finish();
            return;
        }
        _settingsSaved = true;
        Settings& settings = TestSettings(_actor);
        settings.momentumPctPerStack = 5.0f;
        settings.momentumMaxStacks = 40;
        settings.momentumWindowMs = 15000;

        context.Expect(IsReady(), "fabled custom spells are ready");
        if (!IsReady())
        {
            Cleanup(context);
            context.Finish();
            return;
        }

        _baselineRunSpeed = _actor->GetSpeedRate(MOVE_RUN);
        _equipped = Test::EquipFabledTrinket(_actor, Effect::Momentum) != nullptr;
        context.Expect(_equipped, "Momentum fabled trinket equipped");
        if (!_equipped)
        {
            Cleanup(context);
            context.Finish();
            return;
        }

        Creature* first = context.SpawnDummy(3.0f, -0.35f);
        Creature* second = context.SpawnDummy(3.0f, 0.35f);
        Creature* controlled = context.SpawnDummy(5.0f, 0.0f);
        context.Expect(first != nullptr && second != nullptr && controlled != nullptr,
            "two kill targets and one controlled attacker spawned");
        if (!first || !second || !controlled)
        {
            Cleanup(context);
            context.Finish();
            return;
        }

        _firstGuid = first->GetGUID();
        _secondGuid = second->GetGUID();
        _controlledGuid = controlled->GetGUID();
        controlled->SetOwnerGUID(_actor->GetGUID());
        bool firstKilled = context.Engage(_firstGuid)
            && context.Damage(_firstGuid, first->GetHealth());
        context.Expect(firstKilled, "first killing blow executed");
        if (!firstKilled)
        {
            Cleanup(context);
            context.Finish();
            return;
        }

        _stage = Stage::AwaitFirstStack;
        _elapsed = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        _elapsed += diff;
        if (_stage == Stage::AwaitFirstStack)
        {
            if (_elapsed < 100)
                return;

            Aura* aura = _actor->GetAura(SPELL_MOMENTUM_AURA);
            context.Expect(aura != nullptr && aura->GetStackAmount() == 1,
                "first kill grants one Momentum stack");
            if (!aura)
            {
                Cleanup(context);
                context.Finish();
                _stage = Stage::Done;
                return;
            }

            _firstStackRunSpeed = _actor->GetSpeedRate(MOVE_RUN);
            int32 shortenedDuration = std::max<int32>(1, aura->GetMaxDuration() / 2);
            aura->SetDuration(shortenedDuration);
            int32 durationBeforeSecondKill = aura->GetDuration();

            Creature* second = context.GetCreature(_secondGuid);
            Creature* controlled = context.GetCreature(_controlledGuid);
            bool secondKilled = second && controlled;
            if (secondKilled)
            {
                Unit::DealDamage(controlled, second, second->GetHealth(), nullptr, DIRECT_DAMAGE,
                    SPELL_SCHOOL_MASK_NORMAL, nullptr, false, true);
                secondKilled = !second->IsAlive();
            }
            context.Expect(secondKilled, "controlled-unit killing blow executed");

            Aura* refreshed = _actor->GetAura(SPELL_MOMENTUM_AURA);
            int32 refreshedDuration = refreshed ? refreshed->GetDuration() : 0;
            float secondStackRunSpeed = _actor->GetSpeedRate(MOVE_RUN);
            context.Expect(refreshed != nullptr && refreshed->GetStackAmount() == 2,
                "two kills produce two shared-timer Momentum stacks");
            context.Expect(refreshedDuration > durationBeforeSecondKill,
                "second kill refreshes the shared Momentum duration",
                "before=" + std::to_string(durationBeforeSecondKill)
                    + ",after=" + std::to_string(refreshedDuration));
            context.Expect(_firstStackRunSpeed > _baselineRunSpeed
                    && secondStackRunSpeed > _firstStackRunSpeed,
                "run speed grows with each Momentum stack",
                "baseline=" + std::to_string(_baselineRunSpeed)
                    + ",one=" + std::to_string(_firstStackRunSpeed)
                    + ",two=" + std::to_string(secondStackRunSpeed));

            if (!secondKilled || !refreshed)
            {
                Cleanup(context);
                context.Finish();
                _stage = Stage::Done;
                return;
            }

            refreshed->SetDuration(1);
            _stage = Stage::AwaitExpiry;
            _elapsed = 0;
            return;
        }

        if (_stage != Stage::AwaitExpiry || _elapsed < 100)
            return;

        if (_actor->HasAura(SPELL_MOMENTUM_AURA) && _elapsed < 2000)
            return;

        context.Expect(!_actor->HasAura(SPELL_MOMENTUM_AURA),
            "shared timer expiry removes the entire Momentum stack");
        Cleanup(context);
        context.Finish();
        _stage = Stage::Done;
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context);
    }

private:
    enum class Stage
    {
        AwaitFirstStack,
        AwaitExpiry,
        Done
    };

    void Cleanup(TestHarness::Context& context)
    {
        if (_settingsSaved)
        {
            ClearTestSettings(_actor);
            _settingsSaved = false;
        }
        if (_actor && _equipped)
        {
            Test::UnequipFabled(_actor, Effect::Momentum);
            _equipped = false;
        }
        context.DespawnAllDummies();
    }

    Player* _actor = nullptr;
    ObjectGuid _firstGuid;
    ObjectGuid _secondGuid;
    ObjectGuid _controlledGuid;
    Stage _stage = Stage::Done;
    uint32 _elapsed = 0;
    float _baselineRunSpeed = 0.0f;
    float _firstStackRunSpeed = 0.0f;
    bool _settingsSaved = false;
    bool _equipped = false;
};
} // namespace

std::unique_ptr<Script> MakeMomentum()
{
    return std::make_unique<MomentumScript>();
}

void RegisterMomentumTests()
{
    TestHarness::RegisterSuite("fabled-momentum",
        [] { return std::make_unique<MomentumTestSuite>(); });
}
} // namespace Fabled

/*
 * mod-tertiary-stats: fabled effect "Warcaster".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "test_harness.h"

#include <algorithm>
#include <string>

namespace Fabled
{
namespace
{
constexpr uint32 TEST_CAST_SPELL = 133;    // Fireball (rank 1), 1.5-second cast.
constexpr uint32 TEST_CHANNEL_SPELL = 5143; // Arcane Missiles (rank 1), 5-second channel.

class WarcasterScript final : public Script
{
public:
    WarcasterScript() : Script(Effect::Warcaster) { }

    bool CanCastWhileMoving(Player* /*player*/, Runtime& /*runtime*/, Spell const* /*spell*/) override
    {
        return true;
    }
};

class WarcasterTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {

        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled custom spell catalog is ready");
        if (!actor || !IsReady())
        {
            Finish(context);
            return;
        }
        _actorGuid = actor->GetGUID();

        SpellInfo const* castInfo = sSpellMgr->GetSpellInfo(TEST_CAST_SPELL);
        SpellInfo const* channelInfo = sSpellMgr->GetSpellInfo(TEST_CHANNEL_SPELL);
        context.Expect(castInfo && castInfo->CalcCastTime(actor) > 0,
            "cast-time test spell resolved");
        context.Expect(channelInfo && channelInfo->IsChanneled(),
            "channeled test spell resolved");
        if (!castInfo || !castInfo->CalcCastTime(actor) || !channelInfo || !channelInfo->IsChanneled())
        {
            Finish(context);
            return;
        }

        _hadCastSpell = actor->HasSpell(TEST_CAST_SPELL);
        _hadChannelSpell = actor->HasSpell(TEST_CHANNEL_SPELL);
        if (!_hadCastSpell)
            actor->learnSpell(TEST_CAST_SPELL, true);
        if (!_hadChannelSpell)
            actor->learnSpell(TEST_CHANNEL_SPELL, true);

        _equipped = Test::EquipFabledTrinket(actor, Effect::Warcaster) != nullptr;
        context.Expect(_equipped, "Warcaster trinket equipped");

        Creature* dummy = context.SpawnDummy();
        context.Expect(dummy != nullptr && context.Engage(dummy ? dummy->GetGUID() : ObjectGuid::Empty),
            "hostile test target available");
        if (!_equipped || !dummy)
        {
            Finish(context);
            return;
        }
        _dummyGuid = dummy->GetGUID();

        actor->SetPower(POWER_MANA, actor->GetMaxPower(POWER_MANA));
        Test::SetMoving(actor, false);
        context.ClearEvents();

        SpellCastResult result = actor->CastSpell(dummy, TEST_CAST_SPELL, false);
        Spell* current = actor->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        bool started = result == SPELL_CAST_OK && current
            && current->GetSpellInfo()->Id == TEST_CAST_SPELL;
        context.Expect(started, "protected cast-time spell started",
            "result=" + std::to_string(uint32(result)));
        if (!started)
        {
            Finish(context);
            return;
        }

        Test::SetMoving(actor, true);
        Test::Nudge(actor, 0.10f, 0.0f);
        _castTimeoutMs = std::max<uint32>(3000, castInfo->CalcCastTime(actor) + 1000);
        _stage = Stage::AwaitProtectedCast;
        _elapsed = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        if (_stage == Stage::Done)
            return;

        _elapsed += diff;
        switch (_stage)
        {
            case Stage::AwaitProtectedCast:
                UpdateProtectedCast(context);
                break;
            case Stage::AwaitProtectedChannel:
                UpdateProtectedChannel(context);
                break;
            case Stage::AwaitControlReady:
                UpdateControlReady(context);
                break;
            case Stage::AwaitControlCancel:
                UpdateControlCancel(context);
                break;
            case Stage::Done:
                break;
        }
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context);
    }

private:
    enum class Stage
    {
        AwaitProtectedCast,
        AwaitProtectedChannel,
        AwaitControlReady,
        AwaitControlCancel,
        Done
    };

    bool HasEvent(TestHarness::Context const& context, TestHarness::EventType type,
        uint32 spellId) const
    {
        return context.FindEvent(type, _actorGuid, ObjectGuid::Empty, spellId) != nullptr;
    }

    void UpdateProtectedCast(TestHarness::Context& context)
    {
        bool completed = HasEvent(context, TestHarness::EventType::Cast, TEST_CAST_SPELL);
        bool cancelled = HasEvent(context, TestHarness::EventType::CastCancel, TEST_CAST_SPELL);
        if (!completed && !cancelled && _elapsed < _castTimeoutMs)
            return;

        context.Expect(completed, "movement does not prevent protected cast completion");
        context.Expect(!cancelled, "movement does not cancel protected cast");
        if (!completed || cancelled)
        {
            Finish(context);
            return;
        }

        Player* actor = context.GetActor();
        Unit* dummy = context.GetUnit(_dummyGuid);
        if (!actor || !dummy)
        {
            context.Expect(false, "test objects remain available for channel");
            Finish(context);
            return;
        }

        Test::SetMoving(actor, false);
        actor->SetPower(POWER_MANA, actor->GetMaxPower(POWER_MANA));
        context.ClearEvents();
        SpellCastResult result = actor->CastSpell(dummy, TEST_CHANNEL_SPELL, false);
        Spell* channel = actor->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        bool started = result == SPELL_CAST_OK && channel
            && channel->GetSpellInfo()->Id == TEST_CHANNEL_SPELL;
        context.Expect(started, "protected channel started",
            "result=" + std::to_string(uint32(result)));
        if (!started)
        {
            Finish(context);
            return;
        }

        Test::SetMoving(actor, true);
        Test::Nudge(actor, 0.10f, 0.0f);
        _stage = Stage::AwaitProtectedChannel;
        _elapsed = 0;
    }

    void UpdateProtectedChannel(TestHarness::Context& context)
    {
        bool started = HasEvent(context, TestHarness::EventType::Cast, TEST_CHANNEL_SPELL);
        bool cancelled = HasEvent(context, TestHarness::EventType::CastCancel, TEST_CHANNEL_SPELL);
        if (!started && !cancelled && _elapsed < 1000)
            return;

        Player* actor = context.GetActor();
        Spell* channel = actor ? actor->GetCurrentSpell(CURRENT_CHANNELED_SPELL) : nullptr;
        bool live = channel && channel->GetSpellInfo()->Id == TEST_CHANNEL_SPELL
            && channel->getState() == SPELL_STATE_CASTING;
        context.Expect(started, "protected channel emits its cast event");
        context.Expect(!cancelled, "movement does not cancel protected channel");
        context.Expect(live, "movement leaves protected channel active");
        if (!actor)
        {
            Finish(context);
            return;
        }

        actor->InterruptNonMeleeSpells(true);
        Test::SetMoving(actor, false);
        Test::UnequipFabled(actor, Effect::Warcaster);
        _equipped = false;
        context.ClearEvents();
        _stage = Stage::AwaitControlReady;
        _elapsed = 0;
    }

    void UpdateControlReady(TestHarness::Context& context)
    {
        // Let the interrupted channel's global cooldown expire before the control cast.
        if (_elapsed < 1700)
            return;

        Player* actor = context.GetActor();
        Unit* dummy = context.GetUnit(_dummyGuid);
        if (!actor || !dummy)
        {
            context.Expect(false, "test objects remain available for control cast");
            Finish(context);
            return;
        }

        actor->SetPower(POWER_MANA, actor->GetMaxPower(POWER_MANA));
        actor->RemoveSpellCooldown(TEST_CAST_SPELL);
        Test::SetMoving(actor, false);
        context.ClearEvents();
        SpellCastResult result = actor->CastSpell(dummy, TEST_CAST_SPELL, false);
        Spell* current = actor->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        bool started = result == SPELL_CAST_OK && current
            && current->GetSpellInfo()->Id == TEST_CAST_SPELL;
        context.Expect(started, "unequipped control cast started",
            "result=" + std::to_string(uint32(result)));
        if (!started)
        {
            Finish(context);
            return;
        }

        Test::SetMoving(actor, true);
        Test::Nudge(actor, 0.10f, 0.0f);
        _stage = Stage::AwaitControlCancel;
        _elapsed = 0;
    }

    void UpdateControlCancel(TestHarness::Context& context)
    {
        Player* actor = context.GetActor();
        Spell* current = actor ? actor->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
        bool noLongerCasting = !current || current->GetSpellInfo()->Id != TEST_CAST_SPELL;
        bool cancelEvent = HasEvent(context, TestHarness::EventType::CastCancel, TEST_CAST_SPELL);
        if (!noLongerCasting && !cancelEvent && _elapsed < 1000)
            return;

        context.Expect(noLongerCasting || cancelEvent,
            "movement cancels the same cast when Warcaster is unequipped",
            "cancelEvent=" + std::to_string(cancelEvent ? 1 : 0));
        Finish(context);
    }

    void Cleanup(TestHarness::Context& context)
    {
        if (_cleaned)
            return;
        _cleaned = true;

        if (Player* actor = context.GetActor())
        {
            Test::SetMoving(actor, false);
            actor->InterruptNonMeleeSpells(true);
            if (_equipped)
                Test::UnequipFabled(actor, Effect::Warcaster);
            if (!_hadCastSpell)
                actor->removeSpell(TEST_CAST_SPELL, SPEC_MASK_ALL, true);
            if (!_hadChannelSpell)
                actor->removeSpell(TEST_CHANNEL_SPELL, SPEC_MASK_ALL, true);
        }
        context.DespawnAllDummies();
    }

    void Finish(TestHarness::Context& context)
    {
        Cleanup(context);
        _stage = Stage::Done;
        context.Finish();
    }

    Stage _stage = Stage::Done;
    ObjectGuid _actorGuid;
    ObjectGuid _dummyGuid;
    uint32 _elapsed = 0;
    uint32 _castTimeoutMs = 3000;
    bool _hadCastSpell = false;
    bool _hadChannelSpell = false;
    bool _equipped = false;
    bool _cleaned = false;
};
} // namespace

std::unique_ptr<Script> MakeWarcaster()
{
    return std::make_unique<WarcasterScript>();
}

void RegisterWarcasterTests()
{
    TestHarness::RegisterSuite("fabled-warcaster",
        [] { return std::make_unique<WarcasterTestSuite>(); });
}
} // namespace Fabled

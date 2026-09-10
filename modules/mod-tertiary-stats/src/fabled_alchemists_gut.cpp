/*
 * mod-tertiary-stats: fabled effect "Alchemist's Gut".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "test_harness.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace Fabled
{
namespace
{
constexpr TriggerCastFlags FABLED_TRIGGER_FLAGS =
    TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS);
constexpr uint32 POTION_COOLDOWN_CATEGORY = 4;
constexpr uint32 TEST_POTION_ITEM = 3928; // Superior Healing Potion.

void ClearPotionCooldown(Player* player, uint32 spellId, uint32 category)
{
    // Potions do not receive a normal cooldown in combat. Instead,
    // Spell::SendSpellCooldown stores the item in m_lastPotionId and
    // Spell::CheckCast rejects the next potion until combat ends. Out of
    // combat UpdatePotionCooldown materializes the spell/category cooldown.
    // Clear both representations and notify the client about the spell row.
    player->SetLastPotionId(0);
    player->RemoveSpellCooldown(spellId, true);
    player->RemoveCategoryCooldown(POTION_COOLDOWN_CATEGORY);
    if (category && category != POTION_COOLDOWN_CATEGORY)
        player->RemoveCategoryCooldown(category);
}

void AddToxicity(Player* player)
{
    Settings const& settings = GetSettings(player);
    Aura* aura = player->GetAura(SPELL_TOXICITY);
    if (!aura)
    {
        CustomSpellValues values;
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, int32(std::lround(settings.toxicityPctPerTick)));
        player->CastCustomSpell(SPELL_TOXICITY, values, player, FABLED_TRIGGER_FLAGS);
        aura = player->GetAura(SPELL_TOXICITY);
    }
    else if (aura->GetStackAmount() < std::numeric_limits<uint8>::max())
    {
        // SetStackAmount deliberately bypasses the DBC StackAmount limit and
        // recalculates the effect amount, multiplying the per-stack percent.
        aura->SetStackAmount(uint8(aura->GetStackAmount() + 1));
    }

    if (!aura)
        return;

    int32 duration = int32(std::min<uint32>(settings.toxicityDurationMs,
        uint32(std::numeric_limits<int32>::max())));
    aura->SetMaxDuration(duration);
    aura->SetDuration(duration);
}

class AlchemistsGutScript final : public Script
{
public:
    AlchemistsGutScript() : Script(Effect::AlchemistsGut) { }

    void OnRefresh(Player* player, Runtime& /*runtime*/, bool active) override
    {
        if (!active)
            player->RemoveAurasDueToSpell(SPELL_TOXICITY);
    }

    void OnSpellCastComplete(Player* player, Runtime& /*runtime*/, Spell* spell) override
    {
        ItemTemplate const* itemTemplate = spell
            ? sObjectMgr->GetItemTemplate(spell->GetCastItemEntry()) : nullptr;
        if (!itemTemplate || itemTemplate->Class != ITEM_CLASS_CONSUMABLE
            || itemTemplate->SubClass != ITEM_SUBCLASS_POTION)
            return;

        SpellInfo const* spellInfo = spell->GetSpellInfo();
        uint32 spellId = spellInfo->Id;
        uint32 category = spellInfo->GetCategory();

        ClearPotionCooldown(player, spellId, category);
        AddToxicity(player);
    }
};

class AlchemistsGutTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        _stage = Stage::Setup;
        if (actor)
            _baselinePotionCount = actor->GetItemCount(TEST_POTION_ITEM);
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled custom spells are available");
        if (!actor || !IsReady())
        {
            Finish(context);
            return;
        }

        _settingsSaved = true;
        TestSettings(actor).toxicityPctPerTick = 5.0f;
        TestSettings(actor).toxicityDurationMs = 30000;

        _equipped = Test::EquipFabled(context, actor, Effect::AlchemistsGut) != nullptr;
        context.Expect(_equipped, "Alchemist's Gut trinket equipped");
        if (!_equipped || !Has(GetRuntime(actor), Effect::AlchemistsGut))
        {
            Finish(context);
            return;
        }

        Creature* dummy = context.SpawnDummy();
        context.Expect(dummy && context.Engage(dummy ? dummy->GetGUID() : ObjectGuid::Empty),
            "passive dummy keeps the potion test in combat");
        if (!dummy)
        {
            Finish(context);
            return;
        }

        ItemTemplate const* potionTemplate = sObjectMgr->GetItemTemplate(TEST_POTION_ITEM);
        bool validPotion = potionTemplate
            && potionTemplate->Class == ITEM_CLASS_CONSUMABLE
            && potionTemplate->SubClass == ITEM_SUBCLASS_POTION;
        context.Expect(validPotion, "Superior Healing Potion item template is a potion");
        if (!validPotion)
        {
            Finish(context);
            return;
        }

        for (uint8 index = 0; index < MAX_ITEM_PROTO_SPELLS; ++index)
        {
            if (potionTemplate->Spells[index].SpellId
                && potionTemplate->Spells[index].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            {
                _potionSpellId = potionTemplate->Spells[index].SpellId;
                break;
            }
        }
        context.Expect(_potionSpellId != 0, "Superior Healing Potion has an on-use spell");
        if (!_potionSpellId)
        {
            Finish(context);
            return;
        }

        bool added = actor->AddItem(TEST_POTION_ITEM, 2);
        context.Expect(added && actor->GetItemCount(TEST_POTION_ITEM) == _baselinePotionCount + 2,
            "two Superior Healing Potions added to inventory");
        if (!added)
        {
            Finish(context);
            return;
        }

        actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 4));
        context.Expect(Drink(actor, 0), "first potion item-use cast started");
        _stage = Stage::AwaitFirstDrink;
        _elapsedMs = 0;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        if (_stage == Stage::Done)
            return;

        _elapsedMs += diff;
        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Fail("headless actor remains available");
            Finish(context);
            return;
        }

        if (_stage == Stage::AwaitFirstDrink)
        {
            Aura* aura = actor->GetAura(SPELL_TOXICITY);
            if ((!aura || aura->GetStackAmount() != 1 || actor->GetLastPotionId())
                && _elapsedMs < 1500)
                return;

            context.Expect(aura && aura->GetStackAmount() == 1,
                "first potion adds one Toxicity stack");
            context.Expect(actor->GetLastPotionId() == 0
                    && !actor->HasSpellCooldown(_potionSpellId),
                "first potion combat lockout and spell cooldown are cleared");

            actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 4));
            context.Expect(Drink(actor, 1), "second potion item-use cast starts back-to-back");
            _stage = Stage::AwaitSecondDrink;
            _elapsedMs = 0;
            return;
        }

        if (_stage == Stage::AwaitSecondDrink)
        {
            Aura* aura = actor->GetAura(SPELL_TOXICITY);
            if ((!aura || aura->GetStackAmount() != 2 || actor->GetLastPotionId())
                && _elapsedMs < 1500)
                return;

            uint8 stacks = aura ? aura->GetStackAmount() : 0;
            int32 amount = aura && aura->GetEffect(EFFECT_0) ? aura->GetEffect(EFFECT_0)->GetAmount() : 0;
            int32 duration = aura ? aura->GetDuration() : 0;
            context.Expect(stacks == 2, "second potion succeeds and adds a second Toxicity stack",
                "stacks=" + std::to_string(stacks));
            context.Expect(actor->GetItemCount(TEST_POTION_ITEM) == _baselinePotionCount,
                "both back-to-back potion items were consumed");
            context.Expect(actor->GetLastPotionId() == 0
                    && !actor->HasSpellCooldown(_potionSpellId),
                "second potion leaves the next potion immediately available");
            int32 expectedAmount = int32(std::lround(TestSettings(actor).toxicityPctPerTick)) * 2;
            context.Expect(amount == expectedAmount,
                "two Toxicity stacks recalculate to ten percent max health per tick",
                "amount=" + std::to_string(amount));
            context.Expect(duration <= 30000 && duration >= 28000,
                "second drink refreshes Toxicity to approximately thirty seconds",
                "durationMs=" + std::to_string(duration));

            actor->SetHealth(actor->GetMaxHealth());
            _healthBeforeTick = actor->GetHealth();
            _expectedTickDamage = uint32(std::ceil(double(actor->GetMaxHealth()) * 0.10));
            _stage = Stage::AwaitToxicityTick;
            _elapsedMs = 0;
            return;
        }

        if (_stage == Stage::AwaitOutOfCombatDrink)
        {
            Aura* aura = actor->GetAura(SPELL_TOXICITY);
            if ((!aura || aura->GetStackAmount() != 1) && _elapsedMs < 1500)
                return;

            context.Expect(aura && aura->GetStackAmount() == 1
                    && actor->GetItemCount(TEST_POTION_ITEM) == _baselinePotionCount
                    && actor->GetLastPotionId() == 0,
                "an out-of-combat potion also applies Toxicity and clears its cooldown");
            Finish(context);
            return;
        }

        if (_stage != Stage::AwaitToxicityTick || _elapsedMs < 3400)
            return;

        uint32 healthAfter = actor->GetHealth();
        uint32 healthLost = _healthBeforeTick > healthAfter ? _healthBeforeTick - healthAfter : 0;
        context.Expect(healthLost >= _expectedTickDamage && healthLost <= _expectedTickDamage + 1,
            "Toxicity ticks for five percent max health per stack",
            "lost=" + std::to_string(healthLost)
                + " expected=" + std::to_string(_expectedTickDamage));

        actor->RemoveAurasDueToSpell(SPELL_TOXICITY);
        context.DespawnAllDummies();
        bool added = actor->AddItem(TEST_POTION_ITEM, 1);
        actor->SetHealth(std::max<uint32>(1, actor->GetMaxHealth() / 4));
        context.Expect(!actor->IsInCombat() && added && Drink(actor, 2),
            "out-of-combat potion item-use dispatched");
        _stage = Stage::AwaitOutOfCombatDrink;
        _elapsedMs = 0;
    }

    void Cancel(TestHarness::Context& context) override
    {
        Finish(context, false);
    }

private:
    enum class Stage
    {
        Setup,
        AwaitFirstDrink,
        AwaitSecondDrink,
        AwaitToxicityTick,
        AwaitOutOfCombatDrink,
        Done
    };

    bool Drink(Player* actor, uint8 castCount)
    {
        Item* potion = actor->GetItemByEntry(TEST_POTION_ITEM);
        if (!potion)
            return false;

        SpellCastTargets targets;
        targets.SetUnitTarget(actor);
        actor->CastItemUseSpell(potion, targets, castCount, 0);
        return true;
    }

    void Finish(TestHarness::Context& context, bool finish = true)
    {
        if (_stage == Stage::Done)
            return;

        if (Player* actor = context.GetActor())
        {
            if (_equipped)
                Test::UnequipFabled(context, actor, Effect::AlchemistsGut);
            actor->RemoveAurasDueToSpell(SPELL_TOXICITY);
            _equipped = false;
            actor->SetLastPotionId(0);
            if (_potionSpellId)
                ClearPotionCooldown(actor, _potionSpellId, POTION_COOLDOWN_CATEGORY);

            uint32 potionCount = actor->GetItemCount(TEST_POTION_ITEM);
            if (potionCount > _baselinePotionCount)
                actor->DestroyItemCount(TEST_POTION_ITEM, potionCount - _baselinePotionCount, true);
            if (actor->IsAlive())
                actor->SetHealth(actor->GetMaxHealth());
        }

        context.DespawnAllDummies();
        if (_settingsSaved)
        {
            ClearTestSettings(context.GetActor());
            _settingsSaved = false;
        }
        _stage = Stage::Done;
        if (finish)
            context.Finish();
    }

    Stage _stage = Stage::Done;
    bool _settingsSaved = false;
    bool _equipped = false;
    uint32 _baselinePotionCount = 0;
    uint32 _potionSpellId = 0;
    uint32 _elapsedMs = 0;
    uint32 _healthBeforeTick = 0;
    uint32 _expectedTickDamage = 0;
};
} // namespace

std::unique_ptr<Script> MakeAlchemistsGut()
{
    return std::make_unique<AlchemistsGutScript>();
}

void RegisterAlchemistsGutTests()
{
    TestHarness::RegisterSuite("fabled-alchemists-gut",
        [] { return std::make_unique<AlchemistsGutTestSuite>(); });
}
} // namespace Fabled

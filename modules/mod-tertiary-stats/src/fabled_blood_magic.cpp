/*
 * mod-tertiary-stats: fabled effect "Blood Magic".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellDefines.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace Fabled
{
namespace
{
constexpr uint32 SPELL_TEST_ARCANE_EXPLOSION = 1449;
constexpr uint32 SPELL_TEST_FROSTBOLT = 116;
constexpr uint32 BLOOD_DEBT_TICK_MS = 1000;

void ResetBloodDebtSchedule(Player* player)
{
    Runtime& runtime = GetRuntime(player);
    runtime.bloodDebtTickAccumulator = 0;
    runtime.bloodDebtTicksRemaining = uint32(std::max<uint64>(1,
        (uint64(GetSettings().bloodMagicDebtDurationMs) + BLOOD_DEBT_TICK_MS - 1)
            / BLOOD_DEBT_TICK_MS));
}

int32 HealthCost(int32 missingMana)
{
    double cost = std::ceil(double(missingMana) * double(GetSettings().bloodMagicHealthPerMana));
    return int32(std::clamp(cost, 0.0, double(std::numeric_limits<int32>::max())));
}

bool IsBloodMagicSpell(Player* player, Spell const* spell)
{
    if (!player || !spell || spell->IsTriggered() || spell->m_CastItem
        || player->GetCommandStatus(CHEAT_POWER))
        return false;

    SpellInfo const* spellInfo = spell->GetSpellInfo();
    Runtime* runtime = FindRuntime(player);
    return spellInfo && spellInfo->PowerType == POWER_MANA
        && runtime && Has(*runtime, Effect::BloodMagic);
}


void AddBloodDebt(Player* player, int32 debt)
{
    if (!player || debt <= 0)
        return;

    if (Aura* aura = player->GetAura(SPELL_BLOOD_MAGIC_DEBT))
    {
        if (AuraEffect* effect = aura->GetEffect(EFFECT_0))
        {
            int64 remaining = int64(effect->GetAmount()) + debt;
            effect->SetAmount(int32(std::min<int64>(
                remaining, std::numeric_limits<int32>::max())));
            aura->SetMaxDuration(int32(GetSettings().bloodMagicDebtDurationMs));
            aura->SetDuration(int32(GetSettings().bloodMagicDebtDurationMs));
            ResetBloodDebtSchedule(player);
        }
        return;
    }

    CustomSpellValues values;
    values.AddSpellMod(SPELLVALUE_BASE_POINT0, debt);
    values.AddSpellMod(SPELLVALUE_AURA_DURATION,
        int32(GetSettings().bloodMagicDebtDurationMs));
    player->CastCustomSpell(SPELL_BLOOD_MAGIC_DEBT, values, player,
        TriggerCastFlags(TRIGGERED_FULL_MASK | TRIGGERED_DISALLOW_PROC_EVENTS));
    if (player->HasAura(SPELL_BLOOD_MAGIC_DEBT))
        ResetBloodDebtSchedule(player);
}

void UpdateBloodDebt(Player* player, uint32 diffMs)
{
    Aura* aura = player ? player->GetAura(SPELL_BLOOD_MAGIC_DEBT) : nullptr;
    Runtime* runtime = player ? FindRuntime(player) : nullptr;
    if (!aura)
    {
        if (runtime)
        {
            runtime->bloodDebtTickAccumulator = 0;
            runtime->bloodDebtTicksRemaining = 0;
        }
        return;
    }

    if (!runtime)
        runtime = &GetRuntime(player);
    AuraEffect* effect = aura->GetEffect(EFFECT_0);
    int32 remaining = effect ? effect->GetAmount() : 0;
    if (!effect || remaining <= 0)
    {
        player->RemoveAurasDueToSpell(SPELL_BLOOD_MAGIC_DEBT);
        return;
    }
    if (!runtime->bloodDebtTicksRemaining)
    {
        runtime->bloodDebtTicksRemaining = uint32(std::max<int64>(1,
            (int64(std::max(aura->GetDuration(), 1)) + BLOOD_DEBT_TICK_MS - 1)
                / BLOOD_DEBT_TICK_MS));
    }

    uint64 elapsed = uint64(runtime->bloodDebtTickAccumulator) + diffMs;
    runtime->bloodDebtTickAccumulator = uint32(std::min<uint64>(
        elapsed, std::numeric_limits<uint32>::max()));
    while (runtime->bloodDebtTickAccumulator >= BLOOD_DEBT_TICK_MS
        && runtime->bloodDebtTicksRemaining && player->IsAlive())
    {
        runtime->bloodDebtTickAccumulator -= BLOOD_DEBT_TICK_MS;
        int32 damage = int32((int64(remaining) + runtime->bloodDebtTicksRemaining - 1)
            / runtime->bloodDebtTicksRemaining);
        --runtime->bloodDebtTicksRemaining;
        remaining -= damage;
        effect->SetAmount(remaining);
        Unit::DealDamage(player, player, uint32(damage), nullptr, DOT,
            SPELL_SCHOOL_MASK_SHADOW, aura->GetSpellInfo(), false, true, nullptr, DOT);
    }

    if (remaining <= 0 && player->IsAlive())
        player->RemoveAurasDueToSpell(SPELL_BLOOD_MAGIC_DEBT);
}

class BloodDebtPlayerScript final : public PlayerScript
{
public:
    BloodDebtPlayerScript() : PlayerScript("BloodDebtPlayerScript",
        { PLAYERHOOK_ON_BEFORE_UPDATE }) { }

    void OnPlayerBeforeUpdate(Player* player, uint32 diff) override
    {
        UpdateBloodDebt(player, diff);
    }
};


class BloodMagicPowerScript final : public AllSpellScript
{
public:
    BloodMagicPowerScript() : AllSpellScript("BloodMagicPowerScript",
        { ALLSPELLHOOK_ON_CALCULATE_POWER_COST, ALLSPELLHOOK_ON_CAST }) { }

    void OnCalculatePowerCost(Spell* spell, int32& powerCost) override
    {
        Player* player = spell && spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
        if (IsReady() && powerCost > 0 && IsBloodMagicSpell(player, spell))
            powerCost = std::min<int32>(powerCost, player->GetPower(POWER_MANA));
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* /*spellInfo*/,
        bool /*skipCheck*/) override
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        if (!IsReady() || !IsBloodMagicSpell(player, spell))
            return;
        int32 missingMana = std::max(spell->GetPowerCostBeforeScripts() - spell->GetPowerCost(), 0);
        AddBloodDebt(player, HealthCost(missingMana));
    }
};

class BloodMagicScript final : public Script
{
public:
    BloodMagicScript() : Script(Effect::BloodMagic) { }
};

class BloodMagicTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        _savedSettings = GetSettings();
        MutableSettings().bloodMagicHealthPerMana = 4.0f;
        MutableSettings().bloodMagicDebtDurationMs = 3000;

        Player* actor = context.GetActor();
        SpellInfo const* explosion = sSpellMgr->GetSpellInfo(SPELL_TEST_ARCANE_EXPLOSION);
        SpellInfo const* frostbolt = sSpellMgr->GetSpellInfo(SPELL_TEST_FROSTBOLT);
        context.Expect(actor && IsReady() && explosion && frostbolt,
            "Blood Magic test prerequisites are available");
        if (!actor || !IsReady() || !explosion || !frostbolt)
        {
            Cleanup(context);
            return;
        }

        if (!actor->IsAlive())
        {
            actor->ResurrectPlayer(1.0f);
            actor->SpawnCorpseBones();
        }
        _savedHealth = actor->GetHealth();
        _savedMana = actor->GetPower(POWER_MANA);
        _savedMaxMana = actor->GetMaxPower(POWER_MANA);
        _explosionManaCost = explosion->CalcPowerCost(actor, explosion->GetSchoolMask());
        _frostboltManaCost = frostbolt->CalcPowerCost(actor, frostbolt->GetSchoolMask());
        _equipped = Test::EquipFabledTrinket(actor, Effect::BloodMagic) != nullptr;
        context.Expect(_equipped && _explosionManaCost > 1 && _frostboltManaCost > 0,
            "Blood Magic trinket and mana costs resolve");
        if (!_equipped || _explosionManaCost <= 1 || _frostboltManaCost <= 0)
        {
            Cleanup(context);
            return;
        }

        actor->RemoveAurasDueToSpell(SPELL_BLOOD_MAGIC_DEBT);
        actor->SetFullHealth();
        _healthBeforeDebt = actor->GetHealth();
        _startingMana = uint32(_explosionManaCost - std::min(_explosionManaCost - 1, 5));
        actor->SetPower(POWER_MANA, _startingMana);
        _partialDebt = HealthCost(_explosionManaCost - int32(_startingMana));
        actor->CastSpell(actor, SPELL_TEST_ARCANE_EXPLOSION, false);
        context.Expect(actor->GetHealth() == _healthBeforeDebt,
            "Blood Magic applies no immediate health damage");
        context.Expect(DebtAmount(actor) == _partialDebt,
            "only missing mana enters the Blood Debt pool",
            "debt=" + std::to_string(DebtAmount(actor))
                + " expected=" + std::to_string(_partialDebt));
        _stage = Stage::PartialResult;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        _waited += diff;
        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Fail("headless actor remains available");
            Cleanup(context);
            return;
        }

        if (_stage == Stage::PartialResult && _waited >= 250)
        {
            context.Expect(actor->GetPower(POWER_MANA) == 0,
                "Blood Magic spends the available mana");
            _stage = Stage::StackCast;
            _waited = 0;
            return;
        }

        if (_stage == Stage::StackCast && _waited >= 1700)
        {
            actor->SetPower(POWER_MANA, 0);
            _debtBeforeStack = DebtAmount(actor);
            Aura* debtBeforeStack = actor->GetAura(SPELL_BLOOD_MAGIC_DEBT);
            _durationBeforeStack = debtBeforeStack ? debtBeforeStack->GetDuration() : 0;
            _healthBeforeStack = actor->GetHealth();
            _stackDebt = HealthCost(_explosionManaCost);
            actor->CastSpell(actor, SPELL_TEST_ARCANE_EXPLOSION, false);
            Aura* debt = actor->GetAura(SPELL_BLOOD_MAGIC_DEBT);
            context.Expect(actor->GetHealth() == _healthBeforeStack,
                "adding Blood Debt causes no immediate damage");
            context.Expect(debt && DebtAmount(actor) == _debtBeforeStack + _stackDebt,
                "a later cast adds to the exact remaining debt",
                "before=" + std::to_string(_debtBeforeStack)
                    + " added=" + std::to_string(_stackDebt)
                    + " after=" + std::to_string(DebtAmount(actor)));
            context.Expect(debt && debt->GetDuration() > _durationBeforeStack,
                "a later cast refreshes the Blood Debt duration",
                "before=" + std::to_string(_durationBeforeStack)
                    + " after=" + std::to_string(debt ? debt->GetDuration() : 0));
            _stage = Stage::DebtDrain;
            _waited = 0;
            return;
        }


        if (_stage == Stage::DebtDrain)
        {
            if (actor->HasAura(SPELL_BLOOD_MAGIC_DEBT) && _waited < 4000)
                return;
            uint32 damageTaken = _healthBeforeDebt - actor->GetHealth();
            int32 generatedDebt = _partialDebt + _stackDebt;
            context.Expect(!actor->HasAura(SPELL_BLOOD_MAGIC_DEBT)
                    && damageTaken > 0 && damageTaken <= uint32(generatedDebt),
                "Blood Debt drains without exceeding generated damage",
                "damage=" + std::to_string(damageTaken)
                    + " generated=" + std::to_string(generatedDebt));
            _stage = Stage::FullCast;
            _waited = 0;
            return;
        }

        if (_stage == Stage::FullCast && _waited >= 1700)
        {
            actor->SetFullHealth();
            actor->SetPower(POWER_MANA, actor->GetMaxPower(POWER_MANA));
            _fullHealthBefore = actor->GetHealth();
            _fullManaBefore = actor->GetPower(POWER_MANA);
            actor->CastSpell(actor, SPELL_TEST_ARCANE_EXPLOSION, false);
            _stage = Stage::FullResult;
            _waited = 0;
            return;
        }

        if (_stage == Stage::FullResult && _waited >= 250)
        {
            context.Expect(actor->GetPower(POWER_MANA)
                    == _fullManaBefore - uint32(_explosionManaCost),
                "a fully funded spell spends normal mana");
            context.Expect(actor->GetHealth() == _fullHealthBefore
                    && !actor->HasAura(SPELL_BLOOD_MAGIC_DEBT),
                "a fully funded spell creates no Blood Debt");
            _stage = Stage::CancelledCast;
            _waited = 0;
            return;
        }

        if (_stage == Stage::CancelledCast && _waited >= 1700)
        {
            Creature* dummy = context.SpawnDummy();
            context.Expect(dummy != nullptr, "interrupted-cast target spawned");
            if (!dummy)
            {
                Cleanup(context);
                return;
            }
            _dummyGuid = dummy->GetGUID();
            actor->SetPower(POWER_MANA, 0);
            _cancelHealth = actor->GetHealth();
            _cancelMana = actor->GetPower(POWER_MANA);
            actor->CastSpell(dummy, SPELL_TEST_FROSTBOLT, false);
            actor->InterruptNonMeleeSpells(false);
            _stage = Stage::CancelledResult;
            _waited = 0;
            return;
        }

        if (_stage == Stage::CancelledResult && _waited >= 250)
        {
            context.Expect(actor->GetHealth() == _cancelHealth
                    && actor->GetPower(POWER_MANA) == _cancelMana
                    && !actor->HasAura(SPELL_BLOOD_MAGIC_DEBT),
                "an interrupted cast spends neither resources nor Blood Debt");
            _stage = Stage::LethalCast;
            _waited = 0;
            return;
        }

        if (_stage == Stage::LethalCast && _waited >= 1700)
        {
            actor->SetPower(POWER_MANA, 0);
            int32 lethalDebt = HealthCost(_explosionManaCost);
            actor->SetHealth(std::max<uint32>(1, uint32(lethalDebt / 4)));
            _lethalHealthBefore = actor->GetHealth();
            actor->CastSpell(actor, SPELL_TEST_ARCANE_EXPLOSION, false);
            context.Expect(actor->IsAlive() && actor->GetHealth() == _lethalHealthBefore
                    && actor->HasAura(SPELL_BLOOD_MAGIC_DEBT),
                "a lethal Blood Debt cast completes without immediate death");
            _stage = Stage::LethalResult;
            _waited = 0;
            return;
        }


        if (_stage == Stage::LethalResult)
        {
            if (actor->IsAlive() && _waited < 4000)
                return;
            context.Expect(!actor->IsAlive(),
                "Blood Debt can kill through its visible periodic damage");
            Cleanup(context);
        }
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context);
    }

private:
    enum class Stage : uint8
    {
        PartialResult,
        StackCast,
        DebtDrain,
        FullCast,
        FullResult,
        CancelledCast,
        CancelledResult,
        LethalCast,
        LethalResult,
        Done
    };

    static int32 DebtAmount(Player* player)
    {
        Aura* aura = player ? player->GetAura(SPELL_BLOOD_MAGIC_DEBT) : nullptr;
        AuraEffect* effect = aura ? aura->GetEffect(EFFECT_0) : nullptr;
        return effect ? effect->GetAmount() : 0;
    }

    void Cleanup(TestHarness::Context& context)
    {
        if (_cleaned)
            return;
        _cleaned = true;
        _stage = Stage::Done;

        if (Player* actor = context.GetActor())
        {
            actor->InterruptNonMeleeSpells(false);
            actor->RemoveAurasDueToSpell(SPELL_BLOOD_MAGIC_DEBT);
            if (_equipped)
                Test::UnequipFabled(actor, Effect::BloodMagic);
            if (!actor->IsAlive())
            {
                actor->ResurrectPlayer(1.0f);
                actor->SpawnCorpseBones();
            }
            actor->SetHealth(std::min(_savedHealth, actor->GetMaxHealth()));
            actor->SetMaxPower(POWER_MANA, _savedMaxMana);
            actor->SetPower(POWER_MANA, std::min(_savedMana, actor->GetMaxPower(POWER_MANA)));
            actor->CombatStop(true);
        }
        context.DespawnAllDummies();
        MutableSettings() = _savedSettings;
        if (!context.IsFinished())
            context.Finish();
    }

    Settings _savedSettings;
    ObjectGuid _dummyGuid;
    uint32 _savedHealth = 0;
    uint32 _savedMana = 0;
    uint32 _savedMaxMana = 0;
    uint32 _startingMana = 0;
    uint32 _healthBeforeDebt = 0;
    uint32 _healthBeforeStack = 0;
    uint32 _fullHealthBefore = 0;
    uint32 _fullManaBefore = 0;
    uint32 _cancelHealth = 0;
    uint32 _cancelMana = 0;
    uint32 _lethalHealthBefore = 0;
    int32 _partialDebt = 0;
    int32 _debtBeforeStack = 0;
    int32 _stackDebt = 0;
    int32 _durationBeforeStack = 0;
    int32 _explosionManaCost = 0;
    int32 _frostboltManaCost = 0;
    uint32 _waited = 0;
    Stage _stage = Stage::Done;
    bool _equipped = false;
    bool _cleaned = false;
};
} // namespace

std::unique_ptr<Script> MakeBloodMagic()
{
    new BloodDebtPlayerScript();
    new BloodMagicPowerScript();
    TestHarness::RegisterSuite("fabled-blood-magic",
        [] { return std::make_unique<BloodMagicTestSuite>(); });
    return std::make_unique<BloodMagicScript>();
}
} // namespace Fabled

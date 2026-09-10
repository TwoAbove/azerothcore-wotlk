/*
 * mod-tertiary-stats: fabled effect "Keeper".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "test_harness.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <memory>
#include <string_view>
#include <unordered_set>

namespace Fabled
{
namespace
{
constexpr SpellGroup SPELL_GROUP_ELIXIR_UNSTABLE_FLASKS = static_cast<SpellGroup>(3);
constexpr SpellGroup SPELL_GROUP_WELL_FED = static_cast<SpellGroup>(1001);
constexpr int32 MIN_WELL_FED_DURATION_MS = 60000;
constexpr uint32 TEST_ELIXIR_SPELL = 3593; // Elixir of Fortitude, guardian elixir group.
constexpr uint32 TEST_EXTRA_SPELL = 1243;  // Power Word: Fortitude, normally not kept.
constexpr uint32 TEST_PERIODIC_SPELL = 774; // Rejuvenation: finite periodic healing.

std::unordered_set<uint32> ParseExtraSpells(std::string const& csv)
{
    std::unordered_set<uint32> spells;
    std::string_view remaining(csv);
    while (!remaining.empty())
    {
        std::size_t comma = remaining.find(',');
        std::string_view token = remaining.substr(0, comma);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
            token.remove_prefix(1);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
            token.remove_suffix(1);

        uint32 spellId = 0;
        auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), spellId);
        if (!token.empty() && error == std::errc() && end == token.data() + token.size() && spellId)
            spells.insert(spellId);

        if (comma == std::string_view::npos)
            break;
        remaining.remove_prefix(comma + 1);
    }
    return spells;
}



ItemTemplate const* GetCastItemTemplate(Aura const* aura)
{
    if (!aura || !aura->GetCastItemEntry())
        return nullptr;

    return sObjectMgr->GetItemTemplate(aura->GetCastItemEntry());
}

bool IsDrumAura(Aura const* aura)
{
    ItemTemplate const* itemTemplate = GetCastItemTemplate(aura);
    return itemTemplate && itemTemplate->Class == ITEM_CLASS_CONSUMABLE
        && itemTemplate->SubClass == ITEM_SUBCLASS_CONSUMABLE_OTHER
        && itemTemplate->RequiredSkill == SKILL_LEATHERWORKING;
}

bool IsElixirOrFlask(uint32 spellId)
{
    return sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_BATTLE)
        || sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_GUARDIAN)
        || sSpellMgr->IsSpellMemberOfSpellGroup(spellId, SPELL_GROUP_ELIXIR_UNSTABLE_FLASKS);
}

bool IsWellFedOrScroll(AuraApplication const* application)
{
    if (!application || !application->IsPositive())
        return false;

    Aura const* aura = application->GetBase();
    if (!aura)
        return false;

    // AzerothCore already maintains an exact Well Fed stacking group. The item
    // fallback also covers food buffs missing from that group while excluding
    // the short eating/drinking regeneration aura.
    if (sSpellMgr->IsSpellMemberOfSpellGroup(aura->GetId(), SPELL_GROUP_WELL_FED))
        return true;

    ItemTemplate const* itemTemplate = GetCastItemTemplate(aura);
    if (!itemTemplate || itemTemplate->Class != ITEM_CLASS_CONSUMABLE)
        return false;

    if (itemTemplate->SubClass == ITEM_SUBCLASS_SCROLL)
        return true;

    return itemTemplate->SubClass == ITEM_SUBCLASS_FOOD
        && aura->GetMaxDuration() >= MIN_WELL_FED_DURATION_MS;
}

void TrackAura(Player* player, Runtime& runtime, Aura* aura)
{
    if (!player || !player->IsAlive() || !aura || aura->GetMaxDuration() <= 0
        || aura->GetDuration() <= 0 || IsDrumAura(aura))
        return;

    AuraApplication const* application = aura->GetApplicationOfTarget(player->GetGUID());
    if (!application)
        return;

    uint32 spellId = aura->GetId();
    bool extraSpell = GetSettings(player).IsKeeperExtraSpell(spellId);
    if (!extraSpell && !IsElixirOrFlask(spellId) && !IsWellFedOrScroll(application))
        return;

    auto managed = std::find_if(runtime.keeper.managed.begin(), runtime.keeper.managed.end(),
        [aura](KeeperAuraState const& state) { return state.aura == aura; });
    if (managed == runtime.keeper.managed.end())
        runtime.keeper.managed.push_back({ aura });
    aura->SetDurationPaused(true);
}

void TrackAppliedAuras(Player* player, Runtime& runtime)
{
    if (!player || !player->IsAlive())
        return;

    for (auto const& [spellId, application] : player->GetAppliedAuras())
    {
        (void)spellId;
        TrackAura(player, runtime, application ? application->GetBase() : nullptr);
    }
}

void ForgetAura(Runtime& runtime, Aura const* aura)
{
    std::erase_if(runtime.keeper.managed,
        [aura](KeeperAuraState const& state) { return state.aura == aura; });
}

void ReleaseAuras(Runtime& runtime)
{
    for (KeeperAuraState const& state : runtime.keeper.managed)
        state.aura->SetDurationPaused(false);
    runtime.keeper.managed.clear();
}

bool IsTracked(Runtime const& runtime, Aura const* aura)
{
    return std::any_of(runtime.keeper.managed.begin(), runtime.keeper.managed.end(),
        [aura](KeeperAuraState const& state) { return state.aura == aura; });
}

class KeeperScript final : public Script
{
public:
    KeeperScript() : Script(Effect::Keeper) { }

    void OnRefresh(Player* player, Runtime& runtime, bool active) override
    {
        ReleaseAuras(runtime);
        if (active)
            TrackAppliedAuras(player, runtime);
    }

    void OnAuraApply(Player* player, Runtime& runtime, Aura* aura) override
    {
        TrackAura(player, runtime, aura);
    }

    void OnAuraRemove(Player* /*player*/, Runtime& runtime, Aura* aura) override
    {
        if (IsTracked(runtime, aura))
            aura->SetDurationPaused(false);
        ForgetAura(runtime, aura);
    }

    void OnDeath(Player* /*player*/, Runtime& runtime) override
    {
        ReleaseAuras(runtime);
    }

    void OnResurrect(Player* player, Runtime& runtime) override
    {
        TrackAppliedAuras(player, runtime);
    }
};

class KeeperTestSuite final : public TestHarness::Suite
{
public:
    void Start(TestHarness::Context& context) override
    {
        Player* actor = context.GetActor();
        context.Expect(actor != nullptr, "headless actor available");
        context.Expect(IsReady(), "fabled spell catalog ready");
        if (!actor || !IsReady())
        {
            context.Finish();
            return;
        }

        _settingsSaved = true;
        TestSettings(actor).SetKeeperExtraSpells({});

        Item* item = Test::EquipFabled(context, actor, Effect::Keeper);
        context.Expect(item != nullptr, "Keeper fabled trinket equipped");
        if (!item)
        {
            Cleanup(context);
            return;
        }
        _equipped = true;

        actor->RemoveAurasDueToSpell(TEST_ELIXIR_SPELL);
        actor->RemoveAurasDueToSpell(TEST_EXTRA_SPELL);
        actor->CastSpell(actor, TEST_ELIXIR_SPELL, true);
        actor->CastSpell(actor, TEST_EXTRA_SPELL, true);

        Aura* elixir = actor->GetAura(TEST_ELIXIR_SPELL);
        Aura* ordinary = actor->GetAura(TEST_EXTRA_SPELL);
        context.Expect(elixir != nullptr, "Elixir of Fortitude aura applied");
        context.Expect(ordinary != nullptr, "ordinary control buff applied");
        if (!elixir || !ordinary)
        {
            Cleanup(context);
            return;
        }

        Runtime& runtime = GetRuntime(actor);
        int32 elixirMaxDuration = elixir->GetMaxDuration();
        int32 ordinaryMaxDuration = ordinary->GetMaxDuration();
        context.Expect(elixirMaxDuration > TEST_FROZEN_DURATION_MS
                && ordinaryMaxDuration > TEST_FROZEN_DURATION_MS,
            "test buffs start with finite persistable durations");
        context.Expect(IsTracked(runtime, elixir) && !IsTracked(runtime, ordinary),
            "aura apply hook tracks only Keeper-eligible buffs");

        elixir->SetDuration(1);
        ordinary->SetDuration(TEST_FROZEN_DURATION_MS);
        actor->Unit::Update(TEST_ELAPSED_MS);
        elixir = actor->GetAura(TEST_ELIXIR_SPELL);
        ordinary = actor->GetAura(TEST_EXTRA_SPELL);
        context.Expect(elixir && elixir->GetDuration() == 1
                && elixir->GetMaxDuration() == elixirMaxDuration,
            "near-expiry Keeper aura survives a longer real unit update with finite duration");
        context.Expect(ordinary
                && ordinary->GetDuration() == TEST_FROZEN_DURATION_MS - TEST_ELAPSED_MS,
            "ordinary aura counts down during the same real update");
        if (!elixir || !ordinary)
        {
            Cleanup(context);
            return;
        }
        elixir->RefreshDuration();
        actor->Unit::Update(TEST_ELAPSED_MS);
        context.Expect(elixir->GetDuration() == elixirMaxDuration,
            "refreshing a paused aura preserves its paused countdown");

        TestSettings(actor).SetKeeperExtraSpells(" 1243 ");
        actor->RemoveAurasDueToSpell(TEST_EXTRA_SPELL);
        actor->CastSpell(actor, TEST_EXTRA_SPELL, true);
        Aura* managedExtra = actor->GetAura(TEST_EXTRA_SPELL);
        context.Expect(managedExtra && IsTracked(runtime, managedExtra)
                && managedExtra->GetMaxDuration() == ordinaryMaxDuration,
            "configured aura apply hook records a finite application");
        if (!managedExtra)
        {
            Cleanup(context);
            return;
        }

        actor->RemoveAurasDueToSpell(TEST_EXTRA_SPELL);
        context.Expect(runtime.keeper.managed.size() == 1 && IsTracked(runtime, elixir),
            "aura remove hook forgets only the exact removed application");
        actor->CastSpell(actor, TEST_EXTRA_SPELL, true);
        Aura* replacement = actor->GetAura(TEST_EXTRA_SPELL);
        context.Expect(replacement && IsTracked(runtime, replacement),
            "a replacement application receives its own managed identity");
        if (!replacement)
        {
            Cleanup(context);
            return;
        }

        replacement->SetDuration(TEST_FROZEN_DURATION_MS);
        Test::UnequipFabled(context, actor, Effect::Keeper);
        _equipped = false;
        context.Expect(runtime.keeper.managed.empty()
                && replacement->GetDuration() == TEST_FROZEN_DURATION_MS
                && replacement->GetMaxDuration() == ordinaryMaxDuration,
            "unequipping releases current finite auras without replacing or removing them");
        replacement->SetDuration(1);
        actor->Unit::Update(TEST_ELAPSED_MS);
        context.Expect(!actor->HasAura(TEST_EXTRA_SPELL),
            "released near-expiry aura expires on the next real unit update");
        actor->CastSpell(actor, TEST_EXTRA_SPELL, true);
        replacement = actor->GetAura(TEST_EXTRA_SPELL);
        if (!replacement)
        {
            context.Expect(false, "control aura reapplied before re-equipping");
            Cleanup(context);
            return;
        }

        item = Test::EquipFabled(context, actor, Effect::Keeper);
        _equipped = item != nullptr;
        context.Expect(item && IsTracked(runtime, elixir) && IsTracked(runtime, replacement),
            "equipment refresh discovers existing finite auras after clearing and re-attuning");
        if (!item)
        {
            Cleanup(context);
            return;
        }

        // Callback-level coverage deliberately retains the same applications:
        // real death removes these non-death-persistent buffs.
        int32 elixirBeforeDeath = elixir->GetDuration();
        int32 replacementBeforeDeath = replacement->GetDuration();
        sScriptMgr->OnPlayerJustDied(actor);
        actor->Unit::Update(TEST_ELAPSED_MS);
        context.Expect(runtime.keeper.managed.empty()
                && elixir->GetDuration() == elixirBeforeDeath - TEST_ELAPSED_MS
                && replacement->GetDuration() == replacementBeforeDeath - TEST_ELAPSED_MS,
            "death callback releases retained aura timers while Keeper remains equipped");

        bool applySickness = false;
        sScriptMgr->OnPlayerResurrect(actor, 1.0f, applySickness);
        context.Expect(IsTracked(runtime, elixir) && IsTracked(runtime, replacement),
            "resurrection callback re-enrolls retained eligible auras without re-equipping");
        int32 elixirAfterResurrect = elixir->GetDuration();
        actor->Unit::Update(TEST_ELAPSED_MS);
        context.Expect(elixir->GetDuration() == elixirAfterResurrect,
            "a retained eligible aura pauses again after the resurrection callback");

        TestSettings(actor).SetKeeperExtraSpells("1243,774");
        actor->RemoveAurasDueToSpell(TEST_PERIODIC_SPELL);
        actor->CastSpell(actor, TEST_PERIODIC_SPELL, true);
        Aura* periodic = actor->GetAura(TEST_PERIODIC_SPELL);
        context.Expect(periodic != nullptr, "finite periodic control aura applied");
        if (periodic)
        {
            int32 periodicDuration = periodic->GetDuration();
            // Advance beyond the complete original lifetime, then observe another real heal.
            actor->Unit::Update(uint32(periodicDuration));
            actor->SetHealth(1);
            actor->Unit::Update(3000);
            context.Expect(actor->GetHealth() > 1
                    && actor->HasAura(TEST_PERIODIC_SPELL),
                "paused aura keeps healing beyond its original finite tick budget");
            Test::UnequipFabled(context, actor, Effect::Keeper);
            _equipped = false;
            actor->SetHealth(1);
            actor->Unit::Update(3000);
            context.Expect(actor->GetHealth() > 1,
                "resumed aura still heals after paused ticks exceeded its original budget");
            actor->Unit::Update(uint32(periodicDuration));
            context.Expect(!actor->HasAura(TEST_PERIODIC_SPELL),
                "periodic aura expires normally after its duration is resumed");
        }

        Cleanup(context);
    }

    void Update(TestHarness::Context& /*context*/, uint32 /*diff*/) override { }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context, false);
    }

private:
    static constexpr int32 TEST_FROZEN_DURATION_MS = 30000;
    static constexpr int32 TEST_ELAPSED_MS = 1234;

    void Cleanup(TestHarness::Context& context, bool finish = true)
    {
        if (Player* actor = context.GetActor())
        {
            if (_equipped)
                Test::UnequipFabled(context, actor, Effect::Keeper);
            actor->RemoveAurasDueToSpell(TEST_ELIXIR_SPELL);
            actor->RemoveAurasDueToSpell(TEST_EXTRA_SPELL);
            actor->RemoveAurasDueToSpell(TEST_PERIODIC_SPELL);
            actor->SetFullHealth();
        }

        if (_settingsSaved)
        {
            ClearTestSettings(context.GetActor());
            _settingsSaved = false;
        }

        _equipped = false;
        if (finish)
            context.Finish();
    }

    bool _settingsSaved = false;
    bool _equipped = false;
};
} // namespace

void Settings::SetKeeperExtraSpells(std::string csv)
{
    _keeperExtraSpells = ParseExtraSpells(csv);
}

bool Settings::IsKeeperExtraSpell(uint32 spellId) const
{
    return _keeperExtraSpells.contains(spellId);
}

std::unique_ptr<Script> MakeKeeper()
{
    return std::make_unique<KeeperScript>();
}

void RegisterKeeperTests()
{
    TestHarness::RegisterSuite("fabled-keeper",
        [] { return std::make_unique<KeeperTestSuite>(); });
}
} // namespace Fabled

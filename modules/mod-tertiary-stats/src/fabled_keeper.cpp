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
        runtime.keeper.managed.push_back({ aura, aura->GetMaxDuration() });
    else
        managed->maxDuration = aura->GetMaxDuration();
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
        runtime.keeper.managed.clear();
        if (active)
            TrackAppliedAuras(player, runtime);
    }

    void OnAuraApply(Player* player, Runtime& runtime, Aura* aura) override
    {
        TrackAura(player, runtime, aura);
    }

    void OnAuraRemove(Player* /*player*/, Runtime& runtime, Aura* aura) override
    {
        ForgetAura(runtime, aura);
    }

    void OnUpdate(Player* player, Runtime& runtime, uint32 diffMs, uint64 /*nowMs*/) override
    {
        if (!player || !player->IsAlive())
        {
            runtime.keeper.managed.clear();
            return;
        }

        for (KeeperAuraState const& state : runtime.keeper.managed)
        {
            int32 duration = state.aura->GetDuration();
            if (duration <= 0)
                continue;

            int64 compensated = int64(duration) + diffMs;
            state.aura->SetDuration(int32(std::min<int64>(compensated, state.maxDuration)));
        }
    }

    void OnDeath(Player* /*player*/, Runtime& runtime) override
    {
        runtime.keeper.managed.clear();
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
        TestSettings(actor).keeperExtraSpells.clear();

        Item* item = Test::EquipFabledTrinket(actor, Effect::Keeper);
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

        elixir->SetDuration(TEST_FROZEN_DURATION_MS - TEST_ELAPSED_MS);
        ordinary->SetDuration(TEST_FROZEN_DURATION_MS - TEST_ELAPSED_MS);
        HandleUpdate(actor, TEST_ELAPSED_MS);
        context.Expect(elixir->GetDuration() == TEST_FROZEN_DURATION_MS
                && elixir->GetMaxDuration() == elixirMaxDuration,
            "elapsed compensation pauses a finite aura without making it permanent");
        context.Expect(ordinary->GetDuration() == TEST_FROZEN_DURATION_MS - TEST_ELAPSED_MS
                && ordinary->GetMaxDuration() == ordinaryMaxDuration,
            "elapsed compensation leaves an ordinary aura unchanged");

        elixir->SetDuration(elixirMaxDuration - 100);
        HandleUpdate(actor, TEST_ELAPSED_MS);
        context.Expect(elixir->GetDuration() == elixirMaxDuration,
            "elapsed compensation is capped at the original maximum");

        TestSettings(actor).keeperExtraSpells = " 1243 ";
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
        Test::UnequipFabled(actor, Effect::Keeper);
        _equipped = false;
        context.Expect(runtime.keeper.managed.empty()
                && replacement->GetDuration() == TEST_FROZEN_DURATION_MS
                && replacement->GetMaxDuration() == ordinaryMaxDuration,
            "unequipping releases current finite auras without replacing or removing them");

        item = Test::EquipFabledTrinket(actor, Effect::Keeper);
        _equipped = item != nullptr;
        context.Expect(item && IsTracked(runtime, elixir) && IsTracked(runtime, replacement),
            "equipment refresh discovers existing finite auras as relog does");
        if (!item)
        {
            Cleanup(context);
            return;
        }

        int32 elixirBeforeDeath = elixir->GetDuration();
        int32 replacementBeforeDeath = replacement->GetDuration();
        HandleDeath(actor);
        elixir->SetDuration(elixirBeforeDeath - TEST_ELAPSED_MS);
        replacement->SetDuration(replacementBeforeDeath - TEST_ELAPSED_MS);
        HandleUpdate(actor, TEST_ELAPSED_MS);
        context.Expect(runtime.keeper.managed.empty()
                && elixir->GetDuration() == elixirBeforeDeath - TEST_ELAPSED_MS
                && replacement->GetDuration() == replacementBeforeDeath - TEST_ELAPSED_MS,
            "death releases surviving aura timers while Keeper remains equipped");

        HandleResurrect(actor);
        context.Expect(IsTracked(runtime, elixir) && IsTracked(runtime, replacement),
            "resurrection re-enrolls surviving eligible auras without re-equipping");
        int32 elixirAfterResurrect = elixir->GetDuration();
        elixir->SetDuration(elixirAfterResurrect - TEST_ELAPSED_MS);
        HandleUpdate(actor, TEST_ELAPSED_MS);
        context.Expect(elixir->GetDuration() == elixirAfterResurrect,
            "a surviving eligible aura pauses again after resurrection");

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
                Test::UnequipFabled(actor, Effect::Keeper);
            actor->RemoveAurasDueToSpell(TEST_ELIXIR_SPELL);
            actor->RemoveAurasDueToSpell(TEST_EXTRA_SPELL);
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

bool Settings::IsKeeperExtraSpell(uint32 spellId) const
{
    if (_keeperExtraSpellsCacheCsv != keeperExtraSpells)
    {
        _keeperExtraSpellsCache = ParseExtraSpells(keeperExtraSpells);
        _keeperExtraSpellsCacheCsv = keeperExtraSpells;
    }

    return _keeperExtraSpellsCache.contains(spellId);
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

/*
 * mod-tertiary-stats: fabled effect "Keeper".
 */
#include "fabled.h"
#include "fabled_test_utils.h"

#include "Creature.h"
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
void LoadKeeperExtraSpells(std::string const& csv);

namespace
{
constexpr SpellGroup SPELL_GROUP_ELIXIR_UNSTABLE_FLASKS = static_cast<SpellGroup>(3);
constexpr SpellGroup SPELL_GROUP_WELL_FED = static_cast<SpellGroup>(1001);
constexpr int32 MIN_WELL_FED_DURATION_MS = 60000;
constexpr uint32 TEST_ELIXIR_SPELL = 3593; // Elixir of Fortitude, guardian elixir group.
constexpr uint32 TEST_EXTRA_SPELL = 1243;  // Power Word: Fortitude, normally not kept.
std::shared_ptr<std::unordered_set<uint32> const> _extraSpells =
    std::make_shared<std::unordered_set<uint32> const>();

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

void RestoreManaged(Player* player, Runtime& runtime)
{
    if (player)
    {
        for (KeeperAuraIdentity const& identity : runtime.keeperManaged)
        {
            AuraApplication* application = player->GetAuraApplication(identity.spellId,
                identity.casterGuid, identity.castItemGuid, identity.effectMask);
            Aura* aura = application ? application->GetBase() : nullptr;
            if (aura && aura->GetInstanceId() == identity.instanceId)
            {
                aura->SetMaxDuration(identity.maxDuration);
                aura->SetDuration(identity.duration);
            }
        }
    }

    runtime.keeperManaged.clear();
}

class KeeperScript final : public Script
{
public:
    KeeperScript() : Script(Effect::Keeper) { }

    void OnRefresh(Player* player, Runtime& runtime, bool active) override
    {
        if (active)
        {
            if (!runtime.keeperNextScanMs)
                runtime.keeperNextScanMs = Now();
            return;
        }

        RestoreManaged(player, runtime);
        runtime.keeperNextScanMs = 0;
    }

    void OnUpdate(Player* player, Runtime& runtime, uint32 /*diffMs*/, uint64 nowMs) override
    {
        if (!player || (runtime.keeperNextScanMs && nowMs < runtime.keeperNextScanMs))
            return;

        Settings const& settings = GetSettings();
        runtime.keeperNextScanMs = nowMs + settings.keeperScanIntervalMs;
        std::shared_ptr<std::unordered_set<uint32> const> extraSpells =
            std::atomic_load(&_extraSpells);

        Unit::AuraApplicationMap const& applications = player->GetAppliedAuras();
        for (auto const& [spellId, application] : applications)
        {
            Aura* aura = application ? application->GetBase() : nullptr;
            if (!aura || IsDrumAura(aura))
                continue;

            bool extraSpell = extraSpells->find(spellId) != extraSpells->end();
            if (!extraSpell && !IsElixirOrFlask(spellId) && !IsWellFedOrScroll(application))
                continue;

            KeeperAuraIdentity identity{ spellId, aura->GetCasterGUID(), aura->GetCastItemGUID(),
                aura->GetInstanceId(), application->GetEffectMask(), aura->GetMaxDuration(),
                aura->GetDuration() };
            auto sameAura = [&](KeeperAuraIdentity const& managed)
            {
                return managed.spellId == identity.spellId
                    && managed.casterGuid == identity.casterGuid
                    && managed.castItemGuid == identity.castItemGuid
                    && managed.instanceId == identity.instanceId
                    && managed.effectMask == identity.effectMask;
            };
            if (std::find_if(runtime.keeperManaged.begin(), runtime.keeperManaged.end(), sameAura)
                == runtime.keeperManaged.end())
                runtime.keeperManaged.push_back(identity);

            aura->SetMaxDuration(-1);
            aura->SetDuration(-1);
        }
    }

    void OnDeath(Player* player, Runtime& runtime) override
    {
        RestoreManaged(player, runtime);
        runtime.keeperNextScanMs = Now() + GetSettings().keeperScanIntervalMs;
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

        _savedSettings = GetSettings();
        _settingsSaved = true;
        MutableSettings().keeperScanIntervalMs = 50;
        MutableSettings().keeperExtraSpells.clear();
        LoadKeeperExtraSpells(MutableSettings().keeperExtraSpells);

        if (Creature* dummy = context.SpawnDummy())
            _dummyGuid = dummy->GetGUID();
        context.Expect(bool(_dummyGuid), "keeper test dummy spawned");

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

        _elixirMaxDuration = elixir->GetMaxDuration();
        _ordinaryMaxDuration = ordinary->GetMaxDuration();
        context.Expect(_ordinaryMaxDuration > 0, "ordinary control buff starts finite");
        AdvanceClock(actor, MutableSettings().keeperScanIntervalMs + 1);
        _stage = Stage::InitialScan;
    }

    void Update(TestHarness::Context& context, uint32 diff) override
    {
        if (_stage == Stage::Done)
            return;

        Player* actor = context.GetActor();
        if (!actor)
        {
            context.Fail("headless actor remains available");
            Cleanup(context);
            return;
        }

        _elapsed += diff;
        if (_elapsed < 100)
            return;
        _elapsed = 0;
        HandleUpdate(actor, diff);

        if (_stage == Stage::InitialScan)
        {
            Aura* elixir = actor->GetAura(TEST_ELIXIR_SPELL);
            Aura* ordinary = actor->GetAura(TEST_EXTRA_SPELL);
            context.Expect(elixir && elixir->GetMaxDuration() == -1 && elixir->GetDuration() == -1,
                "guardian elixir becomes permanent after Keeper scan");
            context.Expect(ordinary && ordinary->GetMaxDuration() == _ordinaryMaxDuration
                    && ordinary->GetDuration() != -1,
                "non-whitelisted magic buff keeps its finite duration");
            if (!elixir || !ordinary)
            {
                Cleanup(context);
                return;
            }
            for (KeeperAuraIdentity const& identity : GetRuntime(actor).keeperManaged)
            {
                if (identity.spellId == TEST_ELIXIR_SPELL)
                {
                    _elixirMaxDuration = identity.maxDuration;
                    _elixirDuration = identity.duration;
                    break;
                }
            }

            MutableSettings().keeperExtraSpells = " 1243 ";
            LoadKeeperExtraSpells(MutableSettings().keeperExtraSpells);
            AdvanceClock(actor, MutableSettings().keeperScanIntervalMs + 1);
            _stage = Stage::ExtraSpellScan;
            return;
        }

        Aura* extra = actor->GetAura(TEST_EXTRA_SPELL);
        context.Expect(extra && extra->GetMaxDuration() == -1 && extra->GetDuration() == -1,
            "keeperExtraSpells CSV makes the configured buff permanent");
        uint64 managedInstanceId = extra ? extra->GetInstanceId() : 0;
        actor->RemoveAurasDueToSpell(TEST_EXTRA_SPELL);
        actor->CastSpell(actor, TEST_EXTRA_SPELL, true);
        Aura* replacement = actor->GetAura(TEST_EXTRA_SPELL);
        context.Expect(replacement && replacement->GetInstanceId() != managedInstanceId,
            "a later application has a distinct aura identity");

        Test::UnequipFabled(actor, Effect::Keeper);
        _equipped = false;
        Runtime& runtime = GetRuntime(actor);
        Aura* restoredElixir = actor->GetAura(TEST_ELIXIR_SPELL);
        context.Expect(restoredElixir
                && restoredElixir->GetMaxDuration() == _elixirMaxDuration
                && restoredElixir->GetDuration() == _elixirDuration
                && actor->HasAura(TEST_EXTRA_SPELL) && runtime.keeperManaged.empty(),
            "unequipping restores managed timers and preserves a later recast");
        Cleanup(context);
    }

    void Cancel(TestHarness::Context& context) override
    {
        Cleanup(context, false);
    }

private:
    enum class Stage
    {
        InitialScan,
        ExtraSpellScan,
        Done
    };

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
            MutableSettings() = _savedSettings;
            LoadKeeperExtraSpells(MutableSettings().keeperExtraSpells);
            _settingsSaved = false;
        }
        if (_dummyGuid)
            context.DespawnDummy(_dummyGuid);

        _stage = Stage::Done;
        if (finish)
            context.Finish();
    }

    Stage _stage = Stage::Done;
    Settings _savedSettings;
    bool _settingsSaved = false;
    bool _equipped = false;
    ObjectGuid _dummyGuid;
    uint32 _elapsed = 0;
    int32 _ordinaryMaxDuration = 0;
    int32 _elixirMaxDuration = 0;
    int32 _elixirDuration = 0;
};
} // namespace

void LoadKeeperExtraSpells(std::string const& csv)
{
    std::shared_ptr<std::unordered_set<uint32> const> spells =
        std::make_shared<std::unordered_set<uint32> const>(ParseExtraSpells(csv));
    std::atomic_store(&_extraSpells, std::move(spells));
}

std::unique_ptr<Script> MakeKeeper()
{
    TestHarness::RegisterSuite("fabled-keeper",
        [] { return std::make_unique<KeeperTestSuite>(); });
    return std::make_unique<KeeperScript>();
}
} // namespace Fabled

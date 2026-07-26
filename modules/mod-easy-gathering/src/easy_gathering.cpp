/*
 * mod-easy-gathering — every player carries a phantom gnomish multitool.
 *
 * Fishing:
 *  - Removes the "equipped fishing pole" requirement from the Fishing skill
 *    spells (the channeled ranks only — lure spells keep their pole target
 *    requirement, otherwise lures could be applied to any item).
 *  - While the player channels Fishing without a real pole equipped, a pole
 *    is painted into the visible mainhand (PLAYER_VISIBLE_ITEM fields, the
 *    same mechanism transmog uses — the actual weapons never move) and the
 *    best pole in the player's bags contributes its +fishing equip spells
 *    and lure/line enchant spells as real temporary auras, so they count in
 *    the catch roll (GameObject::Use reads live GetSkillValue(SKILL_FISHING)).
 *  - No pole anywhere: fishing still works at base skill with a default pole
 *    visual.
 *
 * Tools:
 *  - Strips configured totem-category tool requirements (mining pick,
 *    skinning knife, blacksmith hammer, arclight spanner, gyromatic
 *    micro-adjustor, virtuoso inking set) from all spells at startup.
 *    Crafted profession foci (enchanting rods, Philosopher's Stone) are
 *    intentionally not covered.
 *
 * Multi-tap nodes:
 *  - Mining veins and herb nodes remain available for 10 seconds after the
 *    first completed gather. Each character can gather the node once.
 */

#include "Bag.h"
#include "Config.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "UpdateFields.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr char PHANTOM_KEY[] = "mod-easy-gathering";
    constexpr char MULTITAP_KEY[] = "mod-easy-gathering-multitap";

    bool _fishingEnable = true;
    bool _toolsEnable = true;
    uint32 _defaultPoleVisual = 6256; // plain Fishing Pole
    bool _multiTapEnable = true;
    uint32 _multiTapWindowSeconds = 10;
    uint32 _materialStackSize = 200;
    std::unordered_set<uint32> _toolCategories;

    // Channeled Fishing skill spells (filled at startup while stripping).
    std::unordered_set<uint32> _fishingSpells;

    struct PhantomPole : public DataMap::Base
    {
        uint32 spellId = 0;             // fishing spell being channeled
        std::vector<uint32> auraIds;    // mirrored bonus auras to remove afterwards
    };

    struct MultiTapNode : public DataMap::Base
    {
        ObjectGuid opener;
        std::unordered_set<ObjectGuid> completed;
        uint64 expiresAtMs = 0;
        bool expiring = false;
    };

    bool IsMultiTapNode(GameObject const* go)
    {
        if (!go || go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
            return false;

        LockEntry const* lock = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lock)
            return false;

        for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
            if (lock->Type[i] == LOCK_KEY_SKILL
                && (lock->Index[i] == LOCKTYPE_HERBALISM || lock->Index[i] == LOCKTYPE_MINING))
                return true;

        return false;
    }

    bool IsUsableFishingPole(Item const* item)
    {
        if (!item || item->IsBroken())
            return false;

        ItemTemplate const* proto = item->GetTemplate();
        return proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
    }

    // Total +fishing skill a spell grants (0 if none).
    int32 FishingSkillBonus(uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return 0;

        int32 bonus = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].ApplyAuraName == SPELL_AURA_MOD_SKILL && info->Effects[i].MiscValue == SKILL_FISHING)
                bonus += info->Effects[i].CalcValue();
        return bonus;
    }

    // Collect the pole's +fishing spells: on-equip item spells plus equip-spells
    // from its permanent (fishing line) and temporary (lure) enchantments.
    int32 CollectPoleBonusSpells(Item const* pole, std::vector<uint32>& out)
    {
        int32 score = 0;

        for (_Spell const& spellData : pole->GetTemplate()->Spells)
            if (spellData.SpellId > 0 && spellData.SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
                if (int32 bonus = FishingSkillBonus(spellData.SpellId))
                {
                    out.push_back(spellData.SpellId);
                    score += bonus;
                }

        for (EnchantmentSlot slot : { PERM_ENCHANTMENT_SLOT, TEMP_ENCHANTMENT_SLOT })
        {
            SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(pole->GetEnchantmentId(slot));
            if (!enchant)
                continue;

            for (uint8 i = 0; i < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++i)
                if (enchant->type[i] == ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL && enchant->spellid[i])
                    if (int32 bonus = FishingSkillBonus(enchant->spellid[i]))
                    {
                        out.push_back(enchant->spellid[i]);
                        score += bonus;
                    }
        }

        return score;
    }

    // Best pole in the backpack or equipped bags (highest total +fishing).
    Item* FindBestPole(Player* player, std::vector<uint32>& bonusSpells)
    {
        Item* best = nullptr;
        int32 bestScore = -1;

        auto consider = [&](Item* item)
        {
            if (!IsUsableFishingPole(item))
                return;

            std::vector<uint32> spells;
            int32 score = CollectPoleBonusSpells(item, spells);
            if (score > bestScore)
            {
                best = item;
                bestScore = score;
                bonusSpells = std::move(spells);
            }
        };

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, i));

        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Bag* bag = player->GetBagByPos(i))
                for (uint32 j = 0; j < bag->GetBagSize(); ++j)
                    consider(player->GetItemByPos(i, j));

        return best;
    }

    void SetVisibleRaw(Player* player, uint8 slot, uint32 entry)
    {
        player->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENTRYID + slot * 2, entry);
        player->SetUInt32Value(PLAYER_VISIBLE_ITEM_1_ENCHANTMENT + slot * 2, 0);
    }
}

class EasyGatheringWorld : public WorldScript
{
public:
    EasyGatheringWorld() : WorldScript("EasyGatheringWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        _fishingEnable = sConfigMgr->GetOption<bool>("EasyGathering.Fishing.Enable", true);
        _defaultPoleVisual = sConfigMgr->GetOption<uint32>("EasyGathering.Fishing.DefaultPoleVisual", 6256);
        _toolsEnable = sConfigMgr->GetOption<bool>("EasyGathering.Tools.Enable", true);
        _multiTapEnable = sConfigMgr->GetOption<bool>("EasyGathering.MultiTap.Enable", true);
        _multiTapWindowSeconds = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("EasyGathering.MultiTap.WindowSeconds", 10));
        _materialStackSize = std::min<uint32>(
            sConfigMgr->GetOption<uint32>("EasyGathering.Materials.StackSize", 200),
            std::numeric_limits<int32>::max() - 1);

        _toolCategories.clear();
        std::string categories = sConfigMgr->GetOption<std::string>("EasyGathering.Tools.TotemCategories", "14,15,121,162,165,166");
        for (std::string_view token : Acore::Tokenize(categories, ',', false))
            if (Optional<uint32> id = Acore::StringTo<uint32>(token))
                _toolCategories.insert(*id);
    }

    void OnStartup() override
    {
        if (!_fishingEnable && !_toolsEnable && !_materialStackSize)
            return;

        uint32 fishingCount = 0;
        uint32 toolCount = 0;
        std::unordered_set<uint32> professionMaterials;

        for (uint32 id = 0; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo* spellInfo = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(id));
            if (!spellInfo)
                continue;

            // Channeled + requires equipped fishing pole == the Fishing skill spells.
            // Lure spells carry the same equip fields as an *item target* restriction
            // and are not channeled — they must keep it.
            if (_fishingEnable
                && spellInfo->IsChanneled()
                && spellInfo->EquippedItemClass == ITEM_CLASS_WEAPON
                && spellInfo->EquippedItemSubClassMask == (1 << ITEM_SUBCLASS_WEAPON_FISHING_POLE))
            {
                spellInfo->EquippedItemClass = -1;
                spellInfo->EquippedItemSubClassMask = 0;
                spellInfo->EquippedItemInventoryTypeMask = 0;
                _fishingSpells.insert(id);
                ++fishingCount;
            }

            if (_toolsEnable)
            {
                std::array<uint32, 2> const original = spellInfo->TotemCategory;
                bool const lootBefore = spellInfo->IsLootCrafting();

                bool stripped = false;
                for (uint32& category : spellInfo->TotemCategory)
                    if (category && _toolCategories.count(category))
                    {
                        category = 0;
                        stripped = true;
                    }

                if (stripped)
                {
                    // IsLootCrafting() keys off TotemCategory[0] for inscription
                    // CREATE_ITEM_2 spells (fake item -> spell_loot_template flow).
                    // Never flip that classification; such spells keep their tool.
                    if (spellInfo->IsLootCrafting() != lootBefore)
                        spellInfo->TotemCategory = original;
                    else
                        ++toolCount;
                }
            }
            if (_materialStackSize)
            {
                SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(id);
                bool const isProfessionSpell = std::any_of(bounds.first, bounds.second, [](auto const& ability)
                {
                    return IsProfessionSkill(ability.second->SkillLine);
                });

                if (isProfessionSpell)
                    for (int32 reagent : spellInfo->Reagent)
                        if (reagent > 0)
                            professionMaterials.insert(static_cast<uint32>(reagent));
            }
        }

        uint32 materialCount = 0;
        if (_materialStackSize)
            for (ItemTemplate* itemTemplate : *sObjectMgr->GetItemTemplateStoreFast())
            {
                if (!itemTemplate || itemTemplate->Stackable <= 0)
                    continue;

                bool const materialClass = itemTemplate->Class == ITEM_CLASS_GEM
                    || itemTemplate->Class == ITEM_CLASS_REAGENT
                    || itemTemplate->Class == ITEM_CLASS_TRADE_GOODS;
                if (!materialClass && !professionMaterials.contains(itemTemplate->ItemId))
                    continue;

                if (itemTemplate->Stackable != static_cast<int32>(_materialStackSize))
                {
                    itemTemplate->Stackable = static_cast<int32>(_materialStackSize);
                    ++materialCount;
                }
            }

        LOG_INFO("module", "mod-easy-gathering: stripped fishing pole requirement from {} spell(s), tool requirements from {} spell(s).", fishingCount, toolCount);
        if (_multiTapEnable)
            LOG_INFO("module", "mod-easy-gathering: multi-tap mining and herb nodes enabled with a {} second window.", _multiTapWindowSeconds);
        if (_materialStackSize)
            LOG_INFO("module", "mod-easy-gathering: set {} profession material stack sizes to {}.", materialCount, _materialStackSize);
    }
};

class EasyGatheringPlayer : public PlayerScript
{
public:
    EasyGatheringPlayer() : PlayerScript("EasyGatheringPlayer", { PLAYERHOOK_ON_SPELL_CAST, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_BEFORE_LOGOUT }) { }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!_fishingEnable || _fishingSpells.find(spell->GetSpellInfo()->Id) == _fishingSpells.end())
            return;

        // Clear any stale phantom from an immediate re-cast.
        RestorePhantom(player);

        // A real pole is equipped: vanilla behavior already covers everything.
        if (IsUsableFishingPole(player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)))
            return;

        PhantomPole* data = player->CustomData.GetDefault<PhantomPole>(PHANTOM_KEY);
        data->spellId = spell->GetSpellInfo()->Id;

        std::vector<uint32> bonusSpells;
        Item* pole = FindBestPole(player, bonusSpells);

        // Paint the pole into the visible mainhand, hide the offhand (poles are
        // two-handed). The actual equipped items are untouched.
        if (pole)
            player->SetVisibleItemSlot(EQUIPMENT_SLOT_MAINHAND, pole);
        else if (_defaultPoleVisual)
            SetVisibleRaw(player, EQUIPMENT_SLOT_MAINHAND, _defaultPoleVisual);
        SetVisibleRaw(player, EQUIPMENT_SLOT_OFFHAND, 0);

        // Mirror the bagged pole's +fishing spells as real auras so they count
        // in the catch roll.
        for (uint32 spellId : bonusSpells)
            if (!player->HasAura(spellId))
            {
                player->AddAura(spellId, player);
                data->auraIds.push_back(spellId);
            }
    }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        PhantomPole* data = player->CustomData.Get<PhantomPole>(PHANTOM_KEY);
        if (!data)
            return;

        Spell* current = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!current || current->GetSpellInfo()->Id != data->spellId)
            RestorePhantom(player);
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        RestorePhantom(player);
    }

private:
    static void RestorePhantom(Player* player)
    {
        PhantomPole* data = player->CustomData.Get<PhantomPole>(PHANTOM_KEY);
        if (!data)
            return;

        for (uint32 spellId : data->auraIds)
            player->RemoveAurasDueToSpell(spellId);

        // Re-sync visible weapons from the real equipped items. Going through
        // SetVisibleItemSlot fires OnPlayerAfterSetVisibleItemSlot, so transmog
        // overrides re-apply themselves.
        player->SetVisibleItemSlot(EQUIPMENT_SLOT_MAINHAND, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND));
        player->SetVisibleItemSlot(EQUIPMENT_SLOT_OFFHAND, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND));

        player->CustomData.Erase(PHANTOM_KEY);
    }
};

class EasyGatheringSpell : public AllSpellScript
{
public:
    EasyGatheringSpell() : AllSpellScript("EasyGatheringSpell", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
    {
        if (!_multiTapEnable || res != SPELL_CAST_OK)
            return;

        Player* player = spell->GetCaster()->ToPlayer();
        GameObject* go = spell->m_targets.GetGOTarget();
        if (!player || !IsMultiTapNode(go))
            return;

        MultiTapNode* data = go->CustomData.Get<MultiTapNode>(MULTITAP_KEY);
        if (!data)
            return;

        uint64 const now = GameTime::GetGameTimeMS().count();
        if (data->expiring || (data->expiresAtMs && now >= data->expiresAtMs))
        {
            res = SPELL_FAILED_NOT_READY;
            return;
        }

        if (data->completed.contains(player->GetGUID()))
        {
            res = SPELL_FAILED_ALREADY_OPEN;
            return;
        }

        if (go->getLootState() == GO_ACTIVATED && data->opener && data->opener != player->GetGUID())
            res = SPELL_FAILED_CHEST_IN_USE;
    }
};

class EasyGatheringGameObject : public AllGameObjectScript
{
public:
    EasyGatheringGameObject() : AllGameObjectScript("EasyGatheringGameObject") { }

    void OnGameObjectLootStateChanged(GameObject* go, uint32 state, Unit* unit) override
    {
        if (!_multiTapEnable || !IsMultiTapNode(go))
            return;

        if (state == GO_ACTIVATED)
        {
            Player* player = unit ? unit->ToPlayer() : nullptr;
            if (!player)
                return;

            MultiTapNode* data = go->CustomData.GetDefault<MultiTapNode>(MULTITAP_KEY);
            if (!data->expiring)
                data->opener = player->GetGUID();
            return;
        }

        if (state != GO_JUST_DEACTIVATED)
            return;

        MultiTapNode* data = go->CustomData.Get<MultiTapNode>(MULTITAP_KEY);
        if (!data)
            return;

        if (data->expiring)
        {
            go->CustomData.Erase(MULTITAP_KEY);
            return;
        }

        if (!data->opener)
            return;

        data->completed.insert(data->opener);
        data->opener.Clear();
        if (!data->expiresAtMs)
            data->expiresAtMs = GameTime::GetGameTimeMS().count() + uint64(_multiTapWindowSeconds) * IN_MILLISECONDS;

        // LootHandler clears the shared Loot object after this callback. Moving
        // back to READY makes the next eligible player generate fresh loot.
        go->SetLootState(GO_READY);
        go->ForceValuesUpdateAtIndex(GAMEOBJECT_DYNAMIC);
    }

    void OnGameObjectUpdate(GameObject* go, uint32 /*diff*/) override
    {
        if (!_multiTapEnable)
            return;

        MultiTapNode* data = go->CustomData.Get<MultiTapNode>(MULTITAP_KEY);
        if (!data || data->expiring || !data->expiresAtMs
            || uint64(GameTime::GetGameTimeMS().count()) < data->expiresAtMs)
            return;

        data->expiring = true;
        go->DespawnOrUnsummon();
    }
};

void AddSC_easy_gathering_earthcaller();

void AddSC_easy_gathering()
{
    new EasyGatheringWorld();
    new EasyGatheringPlayer();
    new EasyGatheringSpell();
    new EasyGatheringGameObject();
    AddSC_easy_gathering_earthcaller();
}

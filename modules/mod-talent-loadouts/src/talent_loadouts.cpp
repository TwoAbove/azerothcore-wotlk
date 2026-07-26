#include "talent_loadouts.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "ScriptedGossip.h"
#include "SpellMgr.h"

#include <algorithm>
#include <ctime>

using namespace Acore::ChatCommands;

namespace
{
    // Collation-correct escaping for the free-form loadout name. Combined with
    // ValidateName() (which rejects control characters and over-long names) this
    // makes the name safe to embed as a query argument without ever
    // string-interpolating raw player input. The escaped value is only ever
    // passed through a positional "{}" argument, never as part of a format
    // string, so any braces inside a name are treated as literal data.
    std::string EscapeName(std::string const& name)
    {
        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);
        return escaped;
    }
}

TalentLoadoutsMgr* TalentLoadoutsMgr::instance()
{
    static TalentLoadoutsMgr instance;
    return &instance;
}

std::string TalentLoadoutsMgr::GetClassString(Player* player) const
{
    return EnumUtils::ToTitle(Classes(player->getClass()));
}

bool TalentLoadoutsMgr::ValidateName(std::string const& raw, std::string& out, std::string& reason) const
{
    std::size_t const start = raw.find_first_not_of(" \t");
    std::size_t const end = raw.find_last_not_of(" \t");
    std::string const name = (start == std::string::npos) ? "" : raw.substr(start, end - start + 1);

    if (name.empty())
    {
        reason = "Loadout name cannot be empty. Usage: .spec save <name>";
        return false;
    }

    if (name.size() > TALENT_LOADOUTS_NAME_MAX_LEN)
    {
        reason = "Loadout name is too long (maximum 32 characters).";
        return false;
    }

    for (unsigned char c : name)
    {
        if (c < 0x20 || c == 0x7F)
        {
            reason = "Loadout name contains invalid characters.";
            return false;
        }
    }

    out = name;
    return true;
}

void TalentLoadoutsMgr::ApplyGlyph(Player* player, uint8 slot, uint32 glyphID)
{
    if (uint32 oldGlyph = player->GetGlyph(slot))
    {
        if (GlyphPropertiesEntry const* gpOld = sGlyphPropertiesStore.LookupEntry(oldGlyph))
            player->RemoveAurasDueToSpell(gpOld->SpellId);
        player->SetGlyph(slot, 0, true);
    }

    if (GlyphPropertiesEntry const* gp = sGlyphPropertiesStore.LookupEntry(glyphID))
    {
        player->CastSpell(player, gp->SpellId, true);
        player->SetGlyph(slot, glyphID, true);
    }
}

uint32 TalentLoadoutsMgr::CountLoadouts(Player* player) const
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT COUNT(*) FROM `mod_talent_loadouts` WHERE `guid`={}",
        player->GetGUID().GetCounter());

    return result ? static_cast<uint32>((*result)[0].Get<uint64>()) : 0u;
}

bool TalentLoadoutsMgr::LoadoutExists(Player* player, std::string const& name) const
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM `mod_talent_loadouts` WHERE `guid`={} AND `name`='{}'",
        player->GetGUID().GetCounter(), EscapeName(name));

    return result != nullptr;
}

std::vector<TalentLoadoutInfo> TalentLoadoutsMgr::ListLoadouts(Player* player) const
{
    std::vector<TalentLoadoutInfo> loadouts;

    QueryResult result = CharacterDatabase.Query(
        "SELECT l.`name`, l.`spec`, l.`class`, l.`created`, "
        "(SELECT COUNT(*) FROM `mod_talent_loadouts_talents` t WHERE t.`guid`=l.`guid` AND t.`name`=l.`name`), "
        "(SELECT COUNT(*) FROM `mod_talent_loadouts_glyphs` g WHERE g.`guid`=l.`guid` AND g.`name`=l.`name`) "
        "FROM `mod_talent_loadouts` l WHERE l.`guid`={} ORDER BY l.`name` ASC",
        player->GetGUID().GetCounter());

    if (!result)
        return loadouts;

    do
    {
        Field* fields = result->Fetch();

        TalentLoadoutInfo info;
        info.name        = fields[0].Get<std::string>();
        info.spec        = fields[1].Get<uint8>();
        info.playerClass = fields[2].Get<uint8>();
        info.created     = fields[3].Get<uint32>();
        info.talentCount = static_cast<uint32>(fields[4].Get<uint64>());
        info.glyphCount  = static_cast<uint32>(fields[5].Get<uint64>());

        loadouts.push_back(std::move(info));
    } while (result->NextRow());

    return loadouts;
}

bool TalentLoadoutsMgr::SaveLoadout(Player* player, std::string const& name, std::string& message)
{
    uint32 const guid = player->GetGUID().GetCounter();
    std::string const escaped = EscapeName(name);

    bool const exists = LoadoutExists(player, name);

    if (!exists && maxLoadouts > 0 && CountLoadouts(player) >= maxLoadouts)
    {
        message = "You have reached the maximum number of saved loadouts (" + std::to_string(maxLoadouts) + ").";
        return false;
    }

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    // Upsert: clear any existing rows for this (guid, name) first.
    trans->Append("DELETE FROM `mod_talent_loadouts_talents` WHERE `guid`={} AND `name`='{}'", guid, escaped);
    trans->Append("DELETE FROM `mod_talent_loadouts_glyphs` WHERE `guid`={} AND `name`='{}'", guid, escaped);
    trans->Append("DELETE FROM `mod_talent_loadouts` WHERE `guid`={} AND `name`='{}'", guid, escaped);

    trans->Append(
        "INSERT INTO `mod_talent_loadouts` (`guid`, `name`, `spec`, `class`, `created`) VALUES ({}, '{}', {}, {}, {})",
        guid, escaped, player->GetActiveSpec(), player->getClass(), static_cast<uint32>(time(nullptr)));

    // Snapshot the talents learned in the active spec. One stored row per
    // learned rank-spell. Partial builds are intentionally saveable, so this is
    // not gated on free talent points.
    uint32 talentCount = 0;
    uint8 const activeSpecMask = player->GetActiveSpecMask();
    for (auto const& [spell, talent] : player->GetTalentMap())
    {
        if (talent->State == PLAYERSPELL_REMOVED || !(talent->specMask & activeSpecMask))
            continue;

        trans->Append(
            "INSERT INTO `mod_talent_loadouts_talents` (`guid`, `name`, `spell`) VALUES ({}, '{}', {})",
            guid, escaped, spell);
        ++talentCount;
    }

    // Snapshot the live glyphs of the active spec.
    uint32 glyphCount = 0;
    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
    {
        if (uint32 glyph = player->GetGlyph(slot))
        {
            trans->Append(
                "INSERT INTO `mod_talent_loadouts_glyphs` (`guid`, `name`, `slot`, `glyph`) VALUES ({}, '{}', {}, {})",
                guid, escaped, slot, glyph);
            ++glyphCount;
        }
    }

    CharacterDatabase.CommitTransaction(trans);

    message = std::string(exists ? "Overwrote" : "Saved") + " loadout '" + name + "': " +
        std::to_string(talentCount) + " talents, " + std::to_string(glyphCount) + " glyphs.";
    return true;
}

bool TalentLoadoutsMgr::LoadLoadout(Player* player, std::string const& name, std::string& message)
{
    uint32 const guid = player->GetGUID().GetCounter();
    std::string const escaped = EscapeName(name);

    QueryResult head = CharacterDatabase.Query(
        "SELECT `class` FROM `mod_talent_loadouts` WHERE `guid`={} AND `name`='{}'",
        guid, escaped);

    if (!head)
    {
        message = "No loadout named '" + name + "' was found.";
        return false;
    }

    uint8 const storedClass = (*head)[0].Get<uint8>();
    if (storedClass != player->getClass())
    {
        message = "Loadout '" + name + "' was created by a different class and cannot be applied.";
        return false;
    }

    // Free reset of the active spec first.
    player->resetTalents(true);

    // Gather the stored talents and resolve each rank-spell to (talentId, rank).
    // Sort the replay by TalentEntry::Row ascending so that prerequisite
    // talents (which live in lower rows) are learned before dependents;
    // LearnTalent(command = true) bypasses the free-point/tier/rank checks but
    // still honours DependsOn / classMask, so ordering matters.
    struct ReplayEntry
    {
        uint32 row;
        uint16 talentId;
        uint8  rank;
        uint32 spell;
    };

    std::vector<ReplayEntry> replay;
    uint32 totalStored = 0;

    if (QueryResult talents = CharacterDatabase.Query(
        "SELECT `spell` FROM `mod_talent_loadouts_talents` WHERE `guid`={} AND `name`='{}'",
        guid, escaped))
    {
        do
        {
            uint32 const spell = (*talents)[0].Get<uint32>();
            ++totalStored;

            TalentSpellPos const* pos = GetTalentSpellPos(spell);
            if (!pos)
                continue;

            TalentEntry const* talentEntry = sTalentStore.LookupEntry(pos->talent_id);
            if (!talentEntry)
                continue;

            replay.push_back({ talentEntry->Row, pos->talent_id, pos->rank, spell });
        } while (talents->NextRow());
    }

    std::sort(replay.begin(), replay.end(),
        [](ReplayEntry const& a, ReplayEntry const& b) { return a.row < b.row; });

    for (ReplayEntry const& entry : replay)
        player->LearnTalent(entry.talentId, entry.rank, true);

    // Replay glyphs. Clear every slot first (ApplyGlyph with 0), then apply the
    // stored glyphs, so the resulting glyph set exactly matches the snapshot.
    uint32 glyphBySlot[MAX_GLYPH_SLOT_INDEX] = { 0 };
    uint32 glyphCount = 0;
    if (QueryResult glyphs = CharacterDatabase.Query(
        "SELECT `slot`, `glyph` FROM `mod_talent_loadouts_glyphs` WHERE `guid`={} AND `name`='{}'",
        guid, escaped))
    {
        do
        {
            Field* fields = glyphs->Fetch();
            uint8 const slot = fields[0].Get<uint8>();
            uint32 const glyph = fields[1].Get<uint32>();
            if (slot < MAX_GLYPH_SLOT_INDEX)
            {
                glyphBySlot[slot] = glyph;
                if (glyph)
                    ++glyphCount;
            }
        } while (glyphs->NextRow());
    }

    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
        ApplyGlyph(player, slot, glyphBySlot[slot]);

    // Push the new talents + glyphs to the client so no relog is required.
    player->SendTalentsInfoData(false);

    // Verify through the talent map (works for passive talents too, unlike
    // HasSpell): a talent counts as applied only if it is now present in the
    // active spec. Anything unresolved, or rejected by DependsOn / classMask
    // during replay, is reported as not applied.
    uint32 appliedTalents = 0;
    for (ReplayEntry const& entry : replay)
        if (player->HasTalent(entry.spell, player->GetActiveSpec()))
            ++appliedTalents;

    uint32 const notApplied = totalStored - appliedTalents;

    message = "Loadout '" + name + "' applied: " + std::to_string(appliedTalents) + " talents, " +
        std::to_string(glyphCount) + " glyphs.";
    if (notApplied > 0)
        message += " " + std::to_string(notApplied) + " talent(s) could not be applied.";

    return true;
}

void TalentLoadoutsMgr::DeleteLoadout(Player* player, std::string const& name)
{
    uint32 const guid = player->GetGUID().GetCounter();
    std::string const escaped = EscapeName(name);

    CharacterDatabase.Execute("DELETE FROM `mod_talent_loadouts` WHERE `guid`={} AND `name`='{}'", guid, escaped);
    CharacterDatabase.Execute("DELETE FROM `mod_talent_loadouts_talents` WHERE `guid`={} AND `name`='{}'", guid, escaped);
    CharacterDatabase.Execute("DELETE FROM `mod_talent_loadouts_glyphs` WHERE `guid`={} AND `name`='{}'", guid, escaped);
}

void TalentLoadoutsMgr::ResetTalents(Player* player)
{
    player->resetTalents(true);
    player->SendTalentsInfoData(false);
}

std::string TalentLoadoutsMgr::SuggestAutoName(Player* player) const
{
    std::string const base = GetClassString(player) + " " + std::to_string(player->GetActiveSpec() + 1);

    for (uint32 i = 1; i < 100000; ++i)
    {
        std::string candidate = base + "-" + std::to_string(i);
        if (candidate.size() > TALENT_LOADOUTS_NAME_MAX_LEN)
            candidate = candidate.substr(0, TALENT_LOADOUTS_NAME_MAX_LEN);

        if (!LoadoutExists(player, candidate))
            return candidate;
    }

    return base;
}

class npc_talent_loadouts : public CreatureScript
{
public:
    npc_talent_loadouts() : CreatureScript("npc_talent_loadouts") {}

    void BuildMainMenu(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();

        std::vector<TalentLoadoutInfo> const loadouts = sTalentLoadoutsMgr->ListLoadouts(player);

        for (uint32 i = 0; i < loadouts.size(); ++i)
        {
            TalentLoadoutInfo const& info = loadouts[i];
            std::string const text = "|TInterface\\icons\\Spell_Holy_MagicalSentry:30:30|t|r Load: " + info.name +
                " (" + std::to_string(info.talentCount) + "t / " + std::to_string(info.glyphCount) + "g)";
            AddGossipItemFor(player, GOSSIP_ICON_TALK, text, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TL_LOAD_BASE + i,
                "Apply loadout '" + info.name + "'? Your current talents will be reset first.", 0, false);
        }

        for (uint32 i = 0; i < loadouts.size(); ++i)
        {
            TalentLoadoutInfo const& info = loadouts[i];
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
                "|TInterface\\icons\\INV_Misc_Gear_01:30:30|t|r Manage: " + info.name,
                GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TL_MANAGE_BASE + i);
        }

        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
            "|TInterface\\icons\\Trade_Engineering:30:30|t|r Save current talents as a new loadout",
            GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TL_SAVE_NEW);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
            "|TInterface\\icons\\Ability_Marksmanship:30:30|t|r Reset talents (free)",
            GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TL_RESET, "Reset all of your talents for free?", 0, false);

        SendGossipMenuFor(player, creature->GetEntry(), creature->GetGUID());
    }

    void BuildManageMenu(Player* player, Creature* creature, uint32 index)
    {
        player->PlayerTalkClass->ClearMenus();

        std::vector<TalentLoadoutInfo> const loadouts = sTalentLoadoutsMgr->ListLoadouts(player);
        if (index >= loadouts.size())
        {
            BuildMainMenu(player, creature);
            return;
        }

        std::string const& name = loadouts[index].name;

        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
            "|TInterface\\icons\\Trade_Engineering:30:30|t|r Overwrite '" + name + "' with current talents",
            GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TL_OVERWRITE_BASE + index,
            "Overwrite loadout '" + name + "' with your current talents and glyphs?", 0, false);

        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1,
            "|TInterface\\icons\\Spell_ChargeNegative:30:30|t|r Delete '" + name + "'",
            GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TL_DELETE_BASE + index,
            "Delete loadout '" + name + "'?", 0, false);

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "|TInterface\\icons\\Ability_Spy:30:30|t|r Back",
            GOSSIP_SENDER_MAIN, GOSSIP_ACTION_TL_MAIN);

        SendGossipMenuFor(player, creature->GetEntry(), creature->GetGUID());
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!sTalentLoadoutsMgr->enable)
        {
            ChatHandler(player->GetSession()).SendSysMessage("Talent loadouts are currently disabled.");
            CloseGossipMenuFor(player);
            return true;
        }

        BuildMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (!player || !creature)
            return false;

        if (!sTalentLoadoutsMgr->enable)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        if (action == GOSSIP_ACTION_TL_MAIN)
        {
            BuildMainMenu(player, creature);
            return true;
        }

        if (action == GOSSIP_ACTION_TL_SAVE_NEW)
        {
            std::string const autoName = sTalentLoadoutsMgr->SuggestAutoName(player);
            std::string message;
            sTalentLoadoutsMgr->SaveLoadout(player, autoName, message);
            ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
            BuildMainMenu(player, creature);
            return true;
        }

        if (action == GOSSIP_ACTION_TL_RESET)
        {
            if (player->IsInCombat())
            {
                ChatHandler(player->GetSession()).SendSysMessage("You cannot reset talents while in combat.");
                CloseGossipMenuFor(player);
                return true;
            }

            sTalentLoadoutsMgr->ResetTalents(player);
            ChatHandler(player->GetSession()).SendSysMessage("Your talents have been reset.");
            CloseGossipMenuFor(player);
            return true;
        }

        if (action >= GOSSIP_ACTION_TL_DELETE_BASE)
        {
            uint32 const index = action - GOSSIP_ACTION_TL_DELETE_BASE;
            std::vector<TalentLoadoutInfo> const loadouts = sTalentLoadoutsMgr->ListLoadouts(player);
            if (index < loadouts.size())
            {
                std::string const name = loadouts[index].name;
                sTalentLoadoutsMgr->DeleteLoadout(player, name);
                ChatHandler(player->GetSession()).PSendSysMessage("Deleted loadout '{}'.", name);
            }
            BuildMainMenu(player, creature);
            return true;
        }

        if (action >= GOSSIP_ACTION_TL_OVERWRITE_BASE)
        {
            uint32 const index = action - GOSSIP_ACTION_TL_OVERWRITE_BASE;
            std::vector<TalentLoadoutInfo> const loadouts = sTalentLoadoutsMgr->ListLoadouts(player);
            if (index < loadouts.size())
            {
                std::string message;
                sTalentLoadoutsMgr->SaveLoadout(player, loadouts[index].name, message);
                ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
            }
            BuildMainMenu(player, creature);
            return true;
        }

        if (action >= GOSSIP_ACTION_TL_MANAGE_BASE)
        {
            uint32 const index = action - GOSSIP_ACTION_TL_MANAGE_BASE;
            BuildManageMenu(player, creature, index);
            return true;
        }

        if (action >= GOSSIP_ACTION_TL_LOAD_BASE)
        {
            uint32 const index = action - GOSSIP_ACTION_TL_LOAD_BASE;
            std::vector<TalentLoadoutInfo> const loadouts = sTalentLoadoutsMgr->ListLoadouts(player);
            if (index < loadouts.size())
            {
                if (player->IsInCombat())
                {
                    ChatHandler(player->GetSession()).SendSysMessage("You cannot load a loadout while in combat.");
                    CloseGossipMenuFor(player);
                    return true;
                }

                std::string message;
                sTalentLoadoutsMgr->LoadLoadout(player, loadouts[index].name, message);
                ChatHandler(player->GetSession()).SendSysMessage(message.c_str());
            }
            CloseGossipMenuFor(player);
            return true;
        }

        CloseGossipMenuFor(player);
        return true;
    }
};

class talent_loadouts_commandscript : public CommandScript
{
public:
    talent_loadouts_commandscript() : CommandScript("talent_loadouts_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable specCommandTable =
        {
            { "save",   HandleSpecSaveCommand,   SEC_PLAYER, Console::No },
            { "load",   HandleSpecLoadCommand,   SEC_PLAYER, Console::No },
            { "list",   HandleSpecListCommand,   SEC_PLAYER, Console::No },
            { "delete", HandleSpecDeleteCommand, SEC_PLAYER, Console::No },
            { "reset",  HandleSpecResetCommand,  SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "spec", specCommandTable },
        };

        return commandTable;
    }

    static bool CheckAccess(ChatHandler* handler)
    {
        if (!sTalentLoadoutsMgr->enable)
        {
            handler->SendSysMessage("Talent loadouts are disabled on this server.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (static_cast<uint32>(handler->GetSession()->GetSecurity()) < sTalentLoadoutsMgr->commandSecurity)
        {
            handler->SendSysMessage("You do not have permission to use this command.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        return true;
    }

    static bool HandleSpecSaveCommand(ChatHandler* handler, Tail nameArg)
    {
        if (!CheckAccess(handler))
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        std::string cleaned;
        std::string reason;
        if (!sTalentLoadoutsMgr->ValidateName(std::string(nameArg), cleaned, reason))
        {
            handler->SendSysMessage(reason.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string message;
        sTalentLoadoutsMgr->SaveLoadout(player, cleaned, message);
        handler->SendSysMessage(message.c_str());
        return true;
    }

    static bool HandleSpecLoadCommand(ChatHandler* handler, Tail nameArg)
    {
        if (!CheckAccess(handler))
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        if (player->IsInCombat())
        {
            handler->SendSysMessage("You cannot load a loadout while in combat.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string cleaned;
        std::string reason;
        if (!sTalentLoadoutsMgr->ValidateName(std::string(nameArg), cleaned, reason))
        {
            handler->SendSysMessage(reason.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string message;
        sTalentLoadoutsMgr->LoadLoadout(player, cleaned, message);
        handler->SendSysMessage(message.c_str());
        return true;
    }

    static bool HandleSpecListCommand(ChatHandler* handler)
    {
        if (!CheckAccess(handler))
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        std::vector<TalentLoadoutInfo> const loadouts = sTalentLoadoutsMgr->ListLoadouts(player);
        if (loadouts.empty())
        {
            handler->SendSysMessage("You have no saved loadouts. Use .spec save <name> to create one.");
            return true;
        }

        handler->PSendSysMessage("Saved loadouts ({}):", static_cast<uint32>(loadouts.size()));
        for (TalentLoadoutInfo const& info : loadouts)
            handler->PSendSysMessage("  {} - {} talents, {} glyphs", info.name, info.talentCount, info.glyphCount);

        return true;
    }

    static bool HandleSpecDeleteCommand(ChatHandler* handler, Tail nameArg)
    {
        if (!CheckAccess(handler))
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        std::string cleaned;
        std::string reason;
        if (!sTalentLoadoutsMgr->ValidateName(std::string(nameArg), cleaned, reason))
        {
            handler->SendSysMessage(reason.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!sTalentLoadoutsMgr->LoadoutExists(player, cleaned))
        {
            handler->PSendSysMessage("No loadout named '{}' was found.", cleaned);
            return true;
        }

        sTalentLoadoutsMgr->DeleteLoadout(player, cleaned);
        handler->PSendSysMessage("Deleted loadout '{}'.", cleaned);
        return true;
    }

    static bool HandleSpecResetCommand(ChatHandler* handler)
    {
        if (!CheckAccess(handler))
            return false;

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        if (player->IsInCombat())
        {
            handler->SendSysMessage("You cannot reset talents while in combat.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        sTalentLoadoutsMgr->ResetTalents(player);
        handler->SendSysMessage("Your talents have been reset for free.");
        return true;
    }
};

class talent_loadouts_playerscript : public PlayerScript
{
public:
    talent_loadouts_playerscript() : PlayerScript("talent_loadouts_playerscript", {
        PLAYERHOOK_ON_DELETE_FROM_DB
    }) {}

    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 guid) override
    {
        trans->Append("DELETE FROM `mod_talent_loadouts_talents` WHERE `guid`={}", guid);
        trans->Append("DELETE FROM `mod_talent_loadouts_glyphs` WHERE `guid`={}", guid);
        trans->Append("DELETE FROM `mod_talent_loadouts` WHERE `guid`={}", guid);
    }
};

class talent_loadouts_worldscript : public WorldScript
{
public:
    talent_loadouts_worldscript() : WorldScript("talent_loadouts_worldscript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sTalentLoadoutsMgr->enable = sConfigMgr->GetOption<bool>("TalentLoadouts.Enable", true);
        sTalentLoadoutsMgr->maxLoadouts = sConfigMgr->GetOption<uint32>("TalentLoadouts.Max", 0);
        sTalentLoadoutsMgr->commandSecurity = sConfigMgr->GetOption<uint32>("TalentLoadouts.CommandSecurity", 0);
    }
};

void AddSC_talent_loadouts()
{
    new npc_talent_loadouts();
    new talent_loadouts_commandscript();
    new talent_loadouts_playerscript();
    new talent_loadouts_worldscript();
}

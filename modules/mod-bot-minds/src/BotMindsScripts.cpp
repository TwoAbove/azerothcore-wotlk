/*
 * mod-bot-minds — script hooks: percept receivers, op drain, and periodic pulse.
 * Everything touching a Player* runs on that player's map thread (inside its own hook).
 */

#include "BotMindsCore.h"

#include "Bag.h"
#include "CharacterCache.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Group.h"
#include "GroupScript.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldScript.h"

// mod-playerbots interop (include dirs wired by the modules build, same as mod-ollama-bot-buddy)
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "LlmProbe.h"

#include <mutex>
#include <sstream>
#include <unordered_map>

using BotMinds::Core;
using BotMinds::Op;

namespace
{

// ---------------------------------------------------------------- state ----

struct WatchState
{
    uint64 lastTickMs = 0;
    uint64 lastPulseMs = 0;

    // combat episode
    bool inCombat = false;
    uint64 combatStartMs = 0;
    uint32 combatKills = 0;

    // death attribution (killer seen shortly before OnPlayerJustDied)
    std::string pendingKiller;
    uint64 pendingKillerMs = 0;
};

// Different maps may update watched bots concurrently.
std::mutex s_stateMx;
std::unordered_map<uint64, WatchState> s_states;

// ------------------------------------------------------------- helpers ----

std::string ZoneName(uint32 zoneId)
{
    if (AreaTableEntry const* e = sAreaTableStore.LookupEntry(zoneId))
        return e->area_name[0];
    return "zone " + std::to_string(zoneId);
}

uint32 FreeBagSlots(Player* bot)
{
    uint32 free = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++free;
    for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
        if (Bag* bag = bot->GetBagByPos(i))
            free += bag->GetFreeSlots();
    return free;
}

void Emit(Player* bot, std::string&& line) { sBotMinds->Push(bot->GetName(), std::move(line)); }

void EmitSayResult(Player* bot, bool ok, std::string const& detail)
{
    std::string p = Core::Base(bot->GetName(), "act");
    p += ",\"action\":\"say\",\"ok\":";
    p += ok ? "true" : "false";
    if (!detail.empty())
        p += ",\"detail\":\"" + Core::Esc(detail) + "\"";
    p += "}";
    Emit(bot, std::move(p));
}

// Playerbots command feedback ("Casting X on Y", "Cannot cast ...") that would
// otherwise be whispered to a master; surfaces silent cast failures.
void EmitTell(Player* bot, std::string const& text)
{
    if (!bot || !sBotMinds->Enabled() || !sBotMinds->IsWatched(bot->GetName()))
        return;
    std::string p = Core::Base(bot->GetName(), "tell");
    p += ",\"text\":\"" + Core::Esc(text) + "\"}";
    Emit(bot, std::move(p));
}

void EmitPulse(Player* bot)
{
    std::string p = Core::Base(bot->GetName(), "pulse");
    p += ",\"level\":" + std::to_string(bot->GetLevel());
    p += ",\"hp_pct\":" + std::to_string(static_cast<uint32>(bot->GetHealthPct()));
    p += ",\"money\":" + std::to_string(bot->GetMoney());
    p += ",\"zone\":\"" + Core::Esc(ZoneName(bot->GetZoneId())) + "\"";
    p += ",\"area\":\"" + Core::Esc(ZoneName(bot->GetAreaId())) + "\"";
    p += ",\"x\":" + std::to_string(bot->GetPositionX());
    p += ",\"y\":" + std::to_string(bot->GetPositionY());
    p += ",\"z\":" + std::to_string(bot->GetPositionZ());
    p += ",\"map\":" + std::to_string(bot->GetMapId());
    p += ",\"bag_free\":" + std::to_string(FreeBagSlots(bot));
    p += "}";
    Emit(bot, std::move(p));
}

// -------------------------------------------------------- op execution ----

void ExecuteSay(Player* bot, PlayerbotAI* ai, Op const& op)
{
    bool ok = false;
    if (op.channel == "say")
        ok = ai->Say(op.text);
    else if (op.channel == "yell")
        ok = ai->Yell(op.text);
    else if (op.channel == "party")
        ok = ai->SayToParty(op.text);
    else if (op.channel == "guild")
        ok = ai->SayToGuild(op.text);
    else if (op.channel == "whisper")
    {
        if (op.to.empty())
        {
            EmitSayResult(bot, false, "whisper needs a target");
            return;
        }
        ok = ai->Whisper(op.text, op.to);
    }
    else
    {
        EmitSayResult(bot, false, "unknown channel '" + op.channel + "'");
        return;
    }
    EmitSayResult(bot, ok, op.channel);
}

// Map thread: serialize the bot's full probe state as a correlated percept.
void EmitState(Player* bot, PlayerbotAI* ai, std::string const& id)
{
    std::ostringstream os;
    LlmProbe::AppendBotJson(os, bot, ai);
    std::string p = Core::Base(bot->GetName(), "state");
    if (!id.empty())
        p += ",\"id\":\"" + Core::Esc(id) + "\"";
    p += ",\"data\":" + os.str() + "}";
    Emit(bot, std::move(p));
}

// Map thread: route one command line down the same pipeline a master whisper
// takes. The ack means "dispatched"; the bot's chat echo carries the outcome.
void ExecuteCommand(Player* bot, PlayerbotAI* ai, Op const& op)
{
    Player* from = ai->GetMaster() ? ai->GetMaster() : bot;
    ai->HandleCommand(CHAT_MSG_WHISPER, op.text, from);
    std::string p = Core::Base(bot->GetName(), "act");
    p += ",\"action\":\"command\",\"text\":\"" + Core::Esc(op.text) + "\"";
    if (!op.id.empty())
        p += ",\"id\":\"" + Core::Esc(op.id) + "\"";
    p += ",\"ok\":true}";
    Emit(bot, std::move(p));
}

// --------------------------------------------------------------- tick ----

void Tick(Player* bot)
{
    uint64 now = Core::NowMs();
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    bool pulseDue = false;
    {
        std::lock_guard<std::mutex> lock(s_stateMx);
        WatchState& state = s_states[bot->GetGUID().GetRawValue()];
        if (now - state.lastTickMs < 1000)
            return;
        state.lastTickMs = now;

        if (!ai)
            return; // percept hooks still run for a watched non-bot, but ops and pulses do not

        uint32 const pulseMinutes = sBotMinds->PulseMinutes();
        pulseDue = pulseMinutes != 0 &&
            (state.lastPulseMs == 0 || now - state.lastPulseMs >= uint64(pulseMinutes) * 60000);
        if (pulseDue)
            state.lastPulseMs = now;
    }

    constexpr uint32 MaxOpsPerTick = 8;
    Op op;
    for (uint32 dispatched = 0; dispatched < MaxOpsPerTick && sBotMinds->PopOp(bot->GetName(), op); ++dispatched)
    {
        if (op.kind == "say")
            ExecuteSay(bot, ai, op);
        else if (op.kind == "state")
            EmitState(bot, ai, op.id);
        else // command — parser admits nothing else
            ExecuteCommand(bot, ai, op);
    }

    if (pulseDue)
        EmitPulse(bot);
}

} // namespace

// ---------------------------------------------------------------------------
// PlayerScript: life events, say-op drain, and periodic pulse
// ---------------------------------------------------------------------------

class BotMindsPlayerScript : public PlayerScript
{
public:
    BotMindsPlayerScript() : PlayerScript("BotMindsPlayerScript") {}

    bool IsWatched(Player* player) const
    {
        return player && sBotMinds->Enabled() && sBotMinds->IsWatched(player->GetName());
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (IsWatched(player) && player->IsInWorld())
            Tick(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;
        std::lock_guard<std::mutex> lock(s_stateMx);
        s_states.erase(player->GetGUID().GetRawValue());
    }

    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (!IsWatched(player) || !quest)
            return;
        std::string p = Core::Base(player->GetName(), "quest");
        p += ",\"status\":\"accepted\",\"title\":\"" + Core::Esc(quest->GetTitle()) + "\",\"id\":" +
             std::to_string(quest->GetQuestId()) + "}";
        Emit(player, std::move(p));
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!IsWatched(player) || !quest)
            return;
        std::string p = Core::Base(player->GetName(), "quest");
        p += ",\"status\":\"completed\",\"title\":\"" + Core::Esc(quest->GetTitle()) + "\",\"id\":" +
             std::to_string(quest->GetQuestId()) + "}";
        Emit(player, std::move(p));
    }

    void OnPlayerQuestAbandon(Player* player, uint32 questId) override
    {
        if (!IsWatched(player))
            return;
        std::string title = "quest " + std::to_string(questId);
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            title = quest->GetTitle();
        std::string p = Core::Base(player->GetName(), "quest");
        p += ",\"status\":\"abandoned\",\"title\":\"" + Core::Esc(title) + "\",\"id\":" +
             std::to_string(questId) + "}";
        Emit(player, std::move(p));
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!IsWatched(player))
            return;
        std::string p = Core::Base(player->GetName(), "level");
        p += ",\"from\":" + std::to_string(oldLevel) + ",\"to\":" + std::to_string(player->GetLevel()) + "}";
        Emit(player, std::move(p));
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        if (!IsWatched(player) || !amount)
            return;
        std::string p = Core::Base(player->GetName(), "xp");
        p += ",\"amount\":" + std::to_string(amount);
        if (victim)
            p += ",\"from\":\"" + Core::Esc(victim->GetName()) + "\"";
        p += ",\"source\":" + std::to_string(xpSource) + "}";
        Emit(player, std::move(p));
    }

    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (!IsWatched(player) || !amount)
            return;
        std::string p = Core::Base(player->GetName(), "money");
        p += ",\"delta\":" + std::to_string(amount);
        p += ",\"total\":" + std::to_string(static_cast<int64>(player->GetMoney()) + amount) + "}";
        Emit(player, std::move(p));
    }

    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override
    {
        if (!IsWatched(player) || !item || !item->GetTemplate())
            return;
        ItemTemplate const* t = item->GetTemplate();
        std::string p = Core::Base(player->GetName(), "loot");
        p += ",\"item\":\"" + Core::Esc(t->Name1) + "\",\"count\":" + std::to_string(count) +
             ",\"quality\":" + std::to_string(t->Quality) + "}";
        Emit(player, std::move(p));
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!IsWatched(killer) || !killed)
            return;
        {
            std::lock_guard<std::mutex> lock(s_stateMx);
            auto it = s_states.find(killer->GetGUID().GetRawValue());
            if (it != s_states.end() && it->second.inCombat)
                ++it->second.combatKills;
        }
        std::string p = Core::Base(killer->GetName(), "kill");
        p += ",\"victim\":\"" + Core::Esc(killed->GetName()) + "\",\"victim_level\":" +
             std::to_string(killed->GetLevel()) + "}";
        Emit(killer, std::move(p));
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!IsWatched(killed) || !killer)
            return;
        std::lock_guard<std::mutex> lock(s_stateMx);
        WatchState& st = s_states[killed->GetGUID().GetRawValue()];
        st.pendingKiller = killer->GetName();
        st.pendingKillerMs = Core::NowMs();
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!IsWatched(player))
            return;
        std::string killer;
        {
            std::lock_guard<std::mutex> lock(s_stateMx);
            auto it = s_states.find(player->GetGUID().GetRawValue());
            if (it != s_states.end() && !it->second.pendingKiller.empty() &&
                Core::NowMs() - it->second.pendingKillerMs < 2000)
                killer = it->second.pendingKiller;
            if (it != s_states.end())
                it->second.pendingKiller.clear();
        }
        std::string p = Core::Base(player->GetName(), "death");
        p += ",\"zone\":\"" + Core::Esc(ZoneName(player->GetAreaId())) + "\"";
        if (!killer.empty())
            p += ",\"killer\":\"" + Core::Esc(killer) + "\"";
        p += "}";
        Emit(player, std::move(p));
    }

    void OnPlayerResurrect(Player* player, float /*restorePercent*/, bool& /*applySickness*/) override
    {
        if (!IsWatched(player))
            return;
        std::string p = Core::Base(player->GetName(), "resurrect");
        p += ",\"zone\":\"" + Core::Esc(ZoneName(player->GetAreaId())) + "\"}";
        Emit(player, std::move(p));
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
    {
        if (!IsWatched(player))
            return;
        std::string p = Core::Base(player->GetName(), "zone");
        p += ",\"zone\":\"" + Core::Esc(ZoneName(newZone)) + "\",\"area\":\"" + Core::Esc(ZoneName(newArea)) + "\"}";
        Emit(player, std::move(p));
    }

    void OnPlayerEnterCombat(Player* player, Unit* enemy) override
    {
        if (!IsWatched(player))
            return;
        std::lock_guard<std::mutex> lock(s_stateMx);
        WatchState& st = s_states[player->GetGUID().GetRawValue()];
        if (st.inCombat)
            return;
        st.inCombat = true;
        st.combatStartMs = Core::NowMs();
        st.combatKills = 0;
        std::string p = Core::Base(player->GetName(), "combat");
        p += ",\"status\":\"start\"";
        if (enemy)
            p += ",\"enemy\":\"" + Core::Esc(enemy->GetName()) + "\"";
        p += "}";
        Emit(player, std::move(p));
    }

    void OnPlayerLeaveCombat(Player* player) override
    {
        if (!IsWatched(player))
            return;
        std::lock_guard<std::mutex> lock(s_stateMx);
        WatchState& st = s_states[player->GetGUID().GetRawValue()];
        if (!st.inCombat)
            return;
        st.inCombat = false;
        uint64 dur = (Core::NowMs() - st.combatStartMs) / 1000;
        std::string p = Core::Base(player->GetName(), "combat");
        p += ",\"status\":\"end\",\"seconds\":" + std::to_string(dur) +
             ",\"kills\":" + std::to_string(st.combatKills) +
             ",\"hp_pct\":" + std::to_string(static_cast<uint32>(player->GetHealthPct())) + "}";
        Emit(player, std::move(p));
    }
};

// ---------------------------------------------------------------------------
// PlayerbotScript: chat the bot hears (its client-bound packets)
// ---------------------------------------------------------------------------

class BotMindsPlayerbotScript : public PlayerbotScript
{
public:
    BotMindsPlayerbotScript() : PlayerbotScript("BotMindsPlayerbotScript") {}

    void OnPlayerbotPacketSent(Player* player, WorldPacket const* packet) override
    {
        if (!player || !packet || !sBotMinds->Enabled())
            return;
        uint16 opcode = packet->GetOpcode();
        if (opcode != SMSG_MESSAGECHAT && opcode != SMSG_GM_MESSAGECHAT)
            return;
        if (!sBotMinds->IsWatched(player->GetName()))
            return;

        try
        {
            WorldPacket p(*packet);
            p.rpos(0);
            uint8 msgtype, chatTag;
            uint32 lang, textLen, unused;
            ObjectGuid guid1, guid2;
            std::string name, chanName, message;

            p >> msgtype >> lang;
            p >> guid1 >> unused;
            if (guid1.IsEmpty() || lang == LANG_ADDON)
                return;

            if (opcode == SMSG_GM_MESSAGECHAT)
            {
                p >> textLen;
                p >> name;
            }

            char const* channel = nullptr;
            switch (msgtype)
            {
                case CHAT_MSG_CHANNEL:
                    p >> chanName;
                    [[fallthrough]];
                case CHAT_MSG_SAY:
                case CHAT_MSG_PARTY:
                case CHAT_MSG_YELL:
                case CHAT_MSG_WHISPER:
                case CHAT_MSG_GUILD:
                    p >> guid2;
                    p >> textLen >> message >> chatTag;
                    break;
                default:
                    return;
            }
            switch (msgtype)
            {
                case CHAT_MSG_SAY:     channel = "say"; break;
                case CHAT_MSG_YELL:    channel = "yell"; break;
                case CHAT_MSG_PARTY:   channel = "party"; break;
                case CHAT_MSG_WHISPER: channel = "whisper"; break;
                case CHAT_MSG_GUILD:   channel = "guild"; break;
                default:               channel = "channel"; break;
            }
            if (message.empty())
                return;
            if (message.size() > 400)
                message.resize(400);

            std::string speaker;
            if (guid1 == player->GetGUID())
                speaker = player->GetName(); // own speech echo — renderer shows "you say"
            else if (name.empty() && !sCharacterCache->GetCharacterNameByGuid(guid1, speaker))
                speaker = "someone";
            else if (!name.empty())
                speaker = name;

            std::string out = Core::Base(player->GetName(), "chat");
            out += ",\"channel\":\"";
            out += channel;
            out += "\"";
            if (msgtype == CHAT_MSG_CHANNEL && !chanName.empty())
                out += ",\"channel_name\":\"" + Core::Esc(chanName) + "\"";
            out += ",\"from\":\"" + Core::Esc(speaker) + "\",\"text\":\"" + Core::Esc(message) + "\"}";
            sBotMinds->Push(player->GetName(), std::move(out));
        }
        catch (...)
        {
            // malformed/unexpected packet layout — never take the server down over a percept
        }
    }
};

// ---------------------------------------------------------------------------
// GroupScript: invites aimed at a watched bot
// ---------------------------------------------------------------------------

class BotMindsGroupScript : public GroupScript
{
public:
    BotMindsGroupScript() : GroupScript("BotMindsGroupScript") {}

    void OnInviteMember(Group* group, ObjectGuid guid) override
    {
        if (!group || !sBotMinds->Enabled())
            return;
        std::string invited;
        if (!sCharacterCache->GetCharacterNameByGuid(guid, invited) || !sBotMinds->IsWatched(invited))
            return;
        std::string leader;
        sCharacterCache->GetCharacterNameByGuid(group->GetLeaderGUID(), leader);
        std::string p = Core::Base(invited, "invite");
        p += ",\"from\":\"" + Core::Esc(leader.empty() ? "someone" : leader) + "\"}";
        sBotMinds->Push(invited, std::move(p));
    }
};

// ---------------------------------------------------------------------------
// WorldScript: config + lifecycle
// ---------------------------------------------------------------------------

class BotMindsWorldScript : public WorldScript
{
public:
    BotMindsWorldScript() : WorldScript("BotMindsWorldScript") {}

    void OnAfterConfigLoad(bool reload) override
    {
        sBotMinds->LoadConfig(reload);
        LlmProbe::SetTellSink(&EmitTell);
    }
    void OnShutdown() override { sBotMinds->Shutdown(); }
};

void Addmod_bot_mindsScripts()
{
    new BotMindsWorldScript();
    new BotMindsPlayerScript();
    new BotMindsPlayerbotScript();
    new BotMindsGroupScript();
}

#ifndef MOD_TALENT_LOADOUTS_H
#define MOD_TALENT_LOADOUTS_H

#include "Player.h"
#include "ScriptMgr.h"

#include <string>
#include <vector>

#define TALENT_LOADOUTS_MODULE_STRING "talent-loadouts"

enum TalentLoadoutsConstants
{
    TALENT_LOADOUTS_NAME_MAX_LEN = 32,
};

// Gossip action ids.
//
// Simple actions are small constants. The per-loadout actions are computed as
// "base + index", where "index" is the position of the loadout inside the
// alphabetically ordered list returned by ListLoadouts(). The list is rebuilt
// (and re-indexed) on every gossip step, so an index is only valid for the
// exact menu it was rendered from.
enum TalentLoadoutsGossipAction
{
    GOSSIP_ACTION_TL_MAIN           = 1,
    GOSSIP_ACTION_TL_SAVE_NEW       = 2,
    GOSSIP_ACTION_TL_RESET          = 3,

    GOSSIP_ACTION_TL_LOAD_BASE      = 1000000,
    GOSSIP_ACTION_TL_MANAGE_BASE    = 2000000,
    GOSSIP_ACTION_TL_OVERWRITE_BASE = 3000000,
    GOSSIP_ACTION_TL_DELETE_BASE    = 4000000,
};

struct TalentLoadoutInfo
{
    std::string name;
    uint8       spec        = 0;
    uint8       playerClass = 0;
    uint32      created     = 0;
    uint32      talentCount = 0;
    uint32      glyphCount  = 0;
};

class TalentLoadoutsMgr
{
public:
    static TalentLoadoutsMgr* instance();

    // --- configuration (populated in WorldScript::OnAfterConfigLoad) ---
    bool   enable          = true;
    uint32 maxLoadouts     = 0;  // 0 == unlimited
    uint32 commandSecurity = 0;  // SEC_PLAYER

    // Validates the raw player-supplied name: trims surrounding whitespace,
    // rejects empty names, names longer than 32 bytes, and any control
    // characters. On success `out` receives the trimmed name; on failure
    // `reason` receives a player-facing explanation.
    bool ValidateName(std::string const& raw, std::string& out, std::string& reason) const;

    // --- persistence helpers (self / player scoped only) ---
    uint32 CountLoadouts(Player* player) const;
    bool   LoadoutExists(Player* player, std::string const& name) const;
    std::vector<TalentLoadoutInfo> ListLoadouts(Player* player) const;

    // Snapshots the current active-spec talents + glyphs under `name` (upsert).
    // Allowed in combat (it does not touch live talents). Returns false and
    // fills `message` when the per-character cap would be exceeded.
    bool SaveLoadout(Player* player, std::string const& name, std::string& message);

    // Applies a stored loadout to the active spec. The caller MUST have already
    // confirmed the player is out of combat. Returns false (and fills `message`)
    // when the loadout is missing or belongs to a different class.
    bool LoadLoadout(Player* player, std::string const& name, std::string& message);

    void DeleteLoadout(Player* player, std::string const& name);

    // Free out-of-combat talent reset (caller confirms out of combat).
    void ResetTalents(Player* player);

    // Builds a unique auto-generated name (class + spec + increment) for
    // gossip-based saves, since gossip has no free-text input.
    std::string SuggestAutoName(Player* player) const;

    std::string GetClassString(Player* player) const;

private:
    // Mirrors mod-npc-talent-template::ApplyGlyph. Passing glyphID == 0 clears
    // the slot (removes the current glyph's aura and empties the slot).
    void ApplyGlyph(Player* player, uint8 slot, uint32 glyphID);
};

#define sTalentLoadoutsMgr TalentLoadoutsMgr::instance()

#endif /* MOD_TALENT_LOADOUTS_H */

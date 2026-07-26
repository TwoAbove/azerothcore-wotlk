/*
 * mod-account-taxi
 *
 * On login, merge every same-faction character's discovered taxi nodes into
 * the current character. The normal character save path persists the result.
 */

#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldSession.h"

#include <array>
#include <string_view>

namespace
{
bool _enabled = true;

class AccountTaxiWorld final : public WorldScript
{
public:
    AccountTaxiWorld() : WorldScript("AccountTaxiWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        _enabled = sConfigMgr->GetOption<bool>("AccountTaxi.Enable", true);
    }
};

class AccountTaxiPlayer final : public PlayerScript
{
public:
    AccountTaxiPlayer() : PlayerScript("AccountTaxiPlayer", { PLAYERHOOK_ON_LOGIN }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!_enabled)
            return;

        ObjectGuid const guid = player->GetGUID();
        std::string const query = Acore::StringFormat(
            "SELECT race, taximask FROM characters WHERE account = {}",
            player->GetSession()->GetAccountId());

        player->GetSession()->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(query)
            .WithCallback([guid](QueryResult result)
        {
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player || !result)
                return;

            TaxiMask accountMask{};
            TeamId const team = player->GetTeamId();

            do
            {
                Field* fields = result->Fetch();
                if (Player::TeamIdForRace(fields[0].Get<uint8>()) != team)
                    continue;

                std::string const taxiMask = fields[1].Get<std::string>();
                std::vector<std::string_view> tokens = Acore::Tokenize(taxiMask, ' ', false);
                for (std::size_t i = 0; i < TaxiMaskSize && i < tokens.size(); ++i)
                    if (Optional<uint32> mask = Acore::StringTo<uint32>(tokens[i]))
                        accountMask[i] |= *mask;
            } while (result->NextRow());

            TaxiMask const& factionMask = team == TEAM_ALLIANCE ? sAllianceTaxiNodesMask : sHordeTaxiNodesMask;
            uint32 learned = 0;

            for (std::size_t field = 0; field < TaxiMaskSize; ++field)
            {
                uint32 const mask = accountMask[field] & factionMask[field] & sTaxiNodesMask[field];
                for (uint32 bit = 0; bit < 32; ++bit)
                {
                    uint32 const node = static_cast<uint32>(field * 32 + bit + 1);
                    if ((mask & (uint32(1) << bit)) != 0 && player->m_taxi.SetTaximaskNode(node))
                        ++learned;
                }
            }

            if (learned != 0)
                LOG_DEBUG("module", "mod-account-taxi: learned {} account flight path(s) for {}", learned, player->GetName());
        }));
    }
};
}

void AddSC_account_taxi()
{
    new AccountTaxiWorld();
    new AccountTaxiPlayer();
}

/*
 * Headless test players: socketless WorldSessions driven through GM commands (SOAP/console).
 * Part of the cata-solo fork. GPL-2.0 like the rest of TrinityCore.
 *
 * .headless create <account> <name> <raceId> <classId> [genderId]   create a character on an account (default look, no client)
 * .headless login <character>            log a character into the world without a client
 * .headless logout <character>           save and remove a headless character
 * .headless list                         list headless characters
 * .headless exec <character> <command>   run a GM command as the character, return its output
 * .headless select spawn <character> <spawnId>   set the character's target to a creature by DB spawn id
 * .headless select entry <character> <entry>     set the character's target to the nearest creature with this entry
 * .headless attack <character>           start melee attacking the current target
 * .headless loot <character>             list the loot the character sees on its selected creature
 * .headless unreward <character> <quest> forget a quest completely (active and rewarded), re-evaluate phases
 * .headless phaseupdate <character>      re-evaluate phase conditions and print current phases
 * .headless cast <character> <spell> [triggered]   cast on the selected target (or self) and report SpellCastResult
 * .headless stop <character>             stop attacking
 *
 * One headless character per game account: World::AddSession replaces an existing session of the same account.
 */

#include "ScriptMgr.h"
#include "AccountMgr.h"
#include "CharacterPackets.h"
#include "DatabaseEnv.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PhasingHandler.h"
#include "SpellDefines.h"
#include "SpellMgr.h"
#include "RealmList.h"
#include "BattlenetAccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "Loot.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "SharedDefines.h"
#include "World.h"
#include "WorldSession.h"

using namespace Trinity::ChatCommands;

namespace
{
    class HeadlessChatHandler : public ChatHandler
    {
    public:
        HeadlessChatHandler(WorldSession* session, std::string& out) : ChatHandler(session), _out(out) { }

        void SendSysMessage(std::string_view str, bool /*escapeCharacters*/) override
        {
            _out.append(str);
            _out.push_back('\n');
        }

        using ChatHandler::SendSysMessage;

    private:
        std::string& _out;
    };

    Player* FindHeadlessPlayer(ChatHandler* handler, std::string const& name)
    {
        Player* player = ObjectAccessor::FindConnectedPlayerByName(name);
        if (!player)
        {
            handler->PSendSysMessage("Headless: character '%s' is not online.", name.c_str());
            handler->SetSentErrorMessage(true);
            return nullptr;
        }
        if (!player->GetSession()->IsHeadless())
        {
            handler->PSendSysMessage("Headless: character '%s' is a real client session, refusing.", name.c_str());
            handler->SetSentErrorMessage(true);
            return nullptr;
        }
        return player;
    }
}

class headless_commandscript : public CommandScript
{
public:
    headless_commandscript() : CommandScript("headless_commandscript") { }

    std::span<ChatCommandBuilder const> GetCommands() const override
    {
        static ChatCommandTable selectCommandTable =
        {
            { "spawn", HandleHeadlessSelectSpawn, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "entry", HandleHeadlessSelectEntry, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };
        static ChatCommandTable headlessCommandTable =
        {
            { "create", HandleHeadlessCreate, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "login",  HandleHeadlessLogin,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "logout", HandleHeadlessLogout, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "list",   HandleHeadlessList,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "exec",   HandleHeadlessExec,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "select", selectCommandTable },
            { "attack", HandleHeadlessAttack, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "loot",   HandleHeadlessLoot,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "unreward", HandleHeadlessUnreward, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "phaseupdate", HandleHeadlessPhaseUpdate, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "cast",   HandleHeadlessCast,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "stop",   HandleHeadlessStop,   rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "headless", headlessCommandTable },
        };
        return commandTable;
    }

    static bool HandleHeadlessCreate(ChatHandler* handler, std::string accountName, std::string charName, uint8 race, uint8 playerClass, Optional<uint8> gender)
    {
        uint32 accountId = AccountMgr::GetId(accountName);
        if (!accountId)
        {
            handler->PSendSysMessage("Headless: account '%s' does not exist.", accountName.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (!normalizePlayerName(charName) || ObjectMgr::CheckPlayerName(charName, LOCALE_enUS, true) != CHAR_NAME_SUCCESS)
        {
            handler->PSendSysMessage("Headless: name '%s' is not valid.", charName.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (sCharacterCache->GetCharacterCacheByName(charName))
        {
            handler->PSendSysMessage("Headless: name '%s' is already in use.", charName.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string sessionAccountName = accountName;
        uint32 bnetAccountId = Battlenet::AccountMgr::GetIdByGameAccount(accountId);
        std::unique_ptr<WorldSession> session = std::make_unique<WorldSession>(accountId, std::move(sessionAccountName), bnetAccountId, nullptr, SEC_ADMINISTRATOR,
            uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "Win", Minutes(0), 0, ClientBuild::VariantId{}, LOCALE_enUS, 0, false);
        session->SetHeadless();

        WorldPackets::Character::CharacterCreateInfo createInfo;
        createInfo.Race = race;
        createInfo.Class = playerClass;
        createInfo.Sex = gender.value_or(GENDER_MALE);
        createInfo.Name = charName;

        ObjectGuid guid;
        {
            std::shared_ptr<Player> newChar(new Player(session.get()), [](Player* ptr)
            {
                ptr->CleanupsBeforeDelete();
                delete ptr;
            });
            newChar->GetMotionMaster()->Initialize();
            if (!newChar->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &createInfo))
            {
                handler->PSendSysMessage("Headless: Player::Create failed for race %u class %u (invalid combination?).", uint32(race), uint32(playerClass));
                handler->SetSentErrorMessage(true);
                return false;
            }
            newChar->setCinematic(1);
            newChar->SetAtLoginFlag(AT_LOGIN_FIRST);

            uint32 charCount = 0;
            if (QueryResult countResult = CharacterDatabase.PQuery("SELECT COUNT(guid) FROM characters WHERE account = {}", accountId))
                charCount = uint32((*countResult)[0].GetUInt64());

            CharacterDatabaseTransaction characterTransaction = CharacterDatabase.BeginTransaction();
            LoginDatabaseTransaction loginTransaction = LoginDatabase.BeginTransaction();
            newChar->SaveToDB(loginTransaction, characterTransaction, true);

            LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_REP_REALM_CHARACTERS);
            stmt->setUInt32(0, charCount + 1);
            stmt->setUInt32(1, accountId);
            stmt->setUInt32(2, sRealmList->GetCurrentRealmId().Realm);
            loginTransaction->Append(stmt);

            // prepared statements of the character save are CONNECTION_ASYNC: commit through the async worker like HandleCharCreateCallback does
            CharacterDatabase.CommitTransaction(characterTransaction);
            LoginDatabase.CommitTransaction(loginTransaction);

            guid = newChar->GetGUID();
            sCharacterCache->AddCharacterCacheEntry(guid, accountId, newChar->GetName(), newChar->GetNativeGender(), newChar->GetRace(), newChar->GetClass(), newChar->GetLevel(), false);
        }

        handler->PSendSysMessage("Headless: created '%s' (%s) on account %u, race %u class %u. Save is queued, wait a few seconds before login.", charName.c_str(), guid.ToString().c_str(), accountId, uint32(race), uint32(playerClass));
        return true;
    }

    static bool HandleHeadlessLogin(ChatHandler* handler, std::string name)
    {
        ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
        if (guid.IsEmpty())
        {
            handler->PSendSysMessage("Headless: character '%s' does not exist.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (ObjectAccessor::FindConnectedPlayerByName(name))
        {
            handler->PSendSysMessage("Headless: character '%s' is already online.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
        if (sWorld->FindSession(accountId))
        {
            handler->PSendSysMessage("Headless: account %u of '%s' already has a session, use a character on a dedicated account.", accountId, name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string accountName;
        AccountMgr::GetName(accountId, accountName);
        uint32 bnetAccountId = Battlenet::AccountMgr::GetIdByGameAccount(accountId);

        WorldSession* session = new WorldSession(accountId, std::move(accountName), bnetAccountId, nullptr, SEC_ADMINISTRATOR,
            uint8(sWorld->getIntConfig(CONFIG_EXPANSION)), 0, "Win", Minutes(0), 0, ClientBuild::VariantId{}, LOCALE_enUS, 0, false);
        session->SetHeadless();
        session->HeadlessLogin(guid);
        sWorld->AddSession(session);

        handler->PSendSysMessage("Headless: login of '%s' (account %u) queued.", name.c_str(), accountId);
        return true;
    }

    static bool HandleHeadlessLogout(ChatHandler* handler, std::string name)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        player->GetSession()->RequestHeadlessLogout();
        handler->PSendSysMessage("Headless: logout of '%s' requested.", name.c_str());
        return true;
    }

    static bool HandleHeadlessList(ChatHandler* handler)
    {
        uint32 count = 0;
        for (auto const& [accountId, session] : sWorld->GetAllSessions())
        {
            if (!session->IsHeadless())
                continue;

            ++count;
            if (Player* player = session->GetPlayer())
                handler->PSendSysMessage("%s account %u map %u area %u pos %.1f %.1f %.1f target %s",
                    player->GetName().c_str(), accountId, player->GetMapId(), player->GetAreaId(),
                    player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetTarget().ToString().c_str());
            else
                handler->PSendSysMessage("(loading) account %u", accountId);
        }
        handler->PSendSysMessage("Headless sessions: %u", count);
        return true;
    }

    static bool HandleHeadlessExec(ChatHandler* handler, std::string name, Tail command)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        std::string text(command);
        if (text.empty())
        {
            handler->SendSysMessage("Headless: empty command.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (text[0] != '.' && text[0] != '!')
            text.insert(text.begin(), '.');

        std::string output;
        HeadlessChatHandler botHandler(player->GetSession(), output);
        bool ok = botHandler.ParseCommands(text);

        if (output.empty())
            output = ok ? "ok" : "command not found or failed";
        handler->SendSysMessage(output);
        return ok;
    }

    static bool HandleHeadlessSelectSpawn(ChatHandler* handler, std::string name, uint64 spawnId)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        Creature* creature = player->GetMap()->GetCreatureBySpawnId(spawnId);
        if (!creature)
        {
            handler->PSendSysMessage("Headless: no creature with spawn id " UI64FMTD " on map %u.", spawnId, player->GetMapId());
            handler->SetSentErrorMessage(true);
            return false;
        }
        player->SetSelection(creature->GetGUID());
        handler->PSendSysMessage("Headless: '%s' targets %s (%s) at %.1f yd.", name.c_str(), creature->GetName().c_str(),
            creature->GetGUID().ToString().c_str(), player->GetDistance(creature));
        return true;
    }

    static bool HandleHeadlessSelectEntry(ChatHandler* handler, std::string name, uint32 entry)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        Creature* creature = player->FindNearestCreature(entry, 200.0f);
        if (!creature)
        {
            handler->PSendSysMessage("Headless: no creature with entry %u within 200 yd.", entry);
            handler->SetSentErrorMessage(true);
            return false;
        }
        player->SetSelection(creature->GetGUID());
        handler->PSendSysMessage("Headless: '%s' targets %s (%s) at %.1f yd.", name.c_str(), creature->GetName().c_str(),
            creature->GetGUID().ToString().c_str(), player->GetDistance(creature));
        return true;
    }

    static bool HandleHeadlessAttack(ChatHandler* handler, std::string name)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        Unit* target = ObjectAccessor::GetUnit(*player, player->GetTarget());
        if (!target)
        {
            handler->SendSysMessage("Headless: no target selected.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        bool started = player->Attack(target, true);
        handler->PSendSysMessage("Headless: '%s' attack %s -> %s.", name.c_str(), target->GetName().c_str(), started ? "started" : "not started");
        return started;
    }

    static bool HandleHeadlessLoot(ChatHandler* handler, std::string name)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        Creature* creature = ObjectAccessor::GetCreature(*player, player->GetTarget());
        if (!creature)
        {
            handler->SendSysMessage("Headless: no creature selected.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        Loot* loot = creature->GetLootForPlayer(player);
        if (!loot)
        {
            handler->PSendSysMessage("Headless: %s (%u) has no loot for '%s' (alive: %u).", creature->GetName().c_str(), creature->GetEntry(), name.c_str(), uint32(creature->IsAlive()));
            return true;
        }
        handler->PSendSysMessage("Headless: loot of %s (%u) for '%s': gold %u, items %u", creature->GetName().c_str(), creature->GetEntry(), name.c_str(), loot->gold, uint32(loot->items.size()));
        for (LootItem const& item : loot->items)
            handler->PSendSysMessage("  item %u x%u%s", item.itemid, uint32(item.count), item.is_looted ? " (looted)" : "");
        return true;
    }

    static bool HandleHeadlessUnreward(ChatHandler* handler, std::string name, uint32 questId)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        if (!sObjectMgr->GetQuestTemplate(questId))
        {
            handler->PSendSysMessage("Headless: quest %u does not exist.", questId);
            handler->SetSentErrorMessage(true);
            return false;
        }
        bool wasRewarded = player->IsQuestRewarded(questId);
        uint32 wasStatus = uint32(player->GetQuestStatus(questId));
        player->RemoveActiveQuest(questId);
        player->RemoveRewardedQuest(questId);
        PhasingHandler::OnConditionChange(player);
        player->SaveToDB();
        handler->PSendSysMessage("Headless: quest %u forgotten for '%s' (was rewarded %u, status %u -> rewarded %u, status %u).", questId, name.c_str(),
            uint32(wasRewarded), wasStatus, uint32(player->IsQuestRewarded(questId)), uint32(player->GetQuestStatus(questId)));
        return true;
    }

    static bool HandleHeadlessPhaseUpdate(ChatHandler* handler, std::string name)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        PhasingHandler::OnConditionChange(player);
        handler->PSendSysMessage("Headless: phases of '%s' re-evaluated: %s", name.c_str(), PhasingHandler::FormatPhases(player->GetPhaseShift()).c_str());
        return true;
    }

    static bool HandleHeadlessCast(ChatHandler* handler, std::string name, uint32 spellId, Optional<bool> triggered)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        if (!sSpellMgr->GetSpellInfo(spellId, DIFFICULTY_NONE))
        {
            handler->PSendSysMessage("Headless: spell %u does not exist.", spellId);
            handler->SetSentErrorMessage(true);
            return false;
        }
        Unit* target = ObjectAccessor::GetUnit(*player, player->GetTarget());
        if (!target)
            target = player;
        CastSpellExtraArgs args(triggered.value_or(false) ? TRIGGERED_FULL_MASK : TRIGGERED_NONE);
        SpellCastResult result = player->CastSpell(target, spellId, args);
        handler->PSendSysMessage("Headless: '%s' cast %u on %s -> SpellCastResult %u (%s), distance %.1f, facing %u, alive %u, in combat %u.",
            name.c_str(), spellId, target->GetName().c_str(), uint32(result), result == SPELL_CAST_OK ? "ok" : "failed",
            player->GetDistance(target), uint32(player->HasInArc(float(M_PI), target)), uint32(player->IsAlive()), uint32(player->IsInCombat()));
        return result == SPELL_CAST_OK;
    }

    static bool HandleHeadlessStop(ChatHandler* handler, std::string name)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        player->AttackStop();
        handler->PSendSysMessage("Headless: '%s' stopped attacking.", name.c_str());
        return true;
    }
};

void AddSC_headless_commandscript()
{
    new headless_commandscript();
}

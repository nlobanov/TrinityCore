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
 * .headless stats <character>            dump the character sheet (stats, ratings, spell power, crit, haste, mastery, equipment)
 * .headless equip <character> <itemId> [randomPropertiesId]   equip an item (adds it first if the character has none);
 *                                        randomPropertiesId > 0 = ItemRandomProperties, < 0 = -ItemRandomSuffix (WoWSims randomSuffix negated)
 * .headless unequip <character> <slot|all>  destroy the equipped item in slot (0..18) or all
 * .headless enchant <character> <slot> <enchantId|0> [perm|prismatic]   set the permanent (or belt-buckle prismatic) enchant of the equipped item
 * .headless gem <character> <slot> <gemItemId> [gemItemId] [gemItemId]  socket gems (item ids) into the equipped item, in socket order
 * .headless reforge <character> <slot> <itemReforgeId|0>   apply an ItemReforge.db2 row to the equipped item
 * .headless talents <character> <string> reset talents and learn a WoWSims talent string ("003-230330221120121213231-03"),
 *                                        set the primary tree (most points) and the class mastery passive
 * .headless glyph <character> <index> <glyphPropertiesId|0>   put a glyph into glyph slot index (0..8), 0 removes
 * .headless burst <character> <spell> <count>   cast <count> triggered (instant, free) casts on the selected target in one tick
 * .headless select none <character>      clear the selection
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
#include "DB2Stores.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "PhasingHandler.h"
#include "SpellDefines.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "StringConvert.h"
#include "Util.h"
#include <tuple>
#include "RealmList.h"
#include "BattlenetAccountMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "Item.h"
#include "ItemTemplate.h"
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
            { "none",  HandleHeadlessSelectNone,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
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
            { "stats",  HandleHeadlessStats,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "equip",  HandleHeadlessEquip,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "unequip", HandleHeadlessUnequip, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "enchant", HandleHeadlessEnchant, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "gem",    HandleHeadlessGem,    rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "reforge", HandleHeadlessReforge, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "talents", HandleHeadlessTalents, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "glyph",  HandleHeadlessGlyph,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "burst",  HandleHeadlessBurst,  rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
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

    static bool HandleHeadlessStats(ChatHandler* handler, std::string name)
    {
        Player* p = FindHeadlessPlayer(handler, name);
        if (!p)
            return false;

        handler->PSendSysMessage("stats %s level %u class %u race %u talentgroup %u", p->GetName().c_str(), uint32(p->GetLevel()), uint32(p->GetClass()), uint32(p->GetRace()), uint32(p->GetActiveTalentGroup()));
        handler->PSendSysMessage("  str %.0f agi %.0f sta %.0f int %.0f spi %.0f", p->GetStat(STAT_STRENGTH), p->GetStat(STAT_AGILITY), p->GetStat(STAT_STAMINA), p->GetStat(STAT_INTELLECT), p->GetStat(STAT_SPIRIT));
        handler->PSendSysMessage("  hp %u mana %d armor %u", uint32(p->GetMaxHealth()), p->GetMaxPower(POWER_MANA), p->GetArmor());
        handler->PSendSysMessage("  spellpower base %u fire %d frost %d arcane %d nature %d shadow %d holy %d healing %d", p->GetBaseSpellPowerBonus(),
            p->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE), p->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FROST), p->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_ARCANE),
            p->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_NATURE), p->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW), p->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY), int32(p->m_activePlayerData->ModHealingDonePos));
        handler->PSendSysMessage("  attackpower melee %.0f ranged %.0f", p->GetTotalAttackPowerValue(BASE_ATTACK), p->GetTotalAttackPowerValue(RANGED_ATTACK));
        handler->PSendSysMessage("  crit melee %.2f ranged %.2f spell(fire) %.2f spell(frost) %.2f spell(arcane) %.2f", float(p->m_activePlayerData->CritPercentage), float(p->m_activePlayerData->RangedCritPercentage),
            float(p->m_activePlayerData->SpellCritPercentage[SPELL_SCHOOL_FIRE]), float(p->m_activePlayerData->SpellCritPercentage[SPELL_SCHOOL_FROST]), float(p->m_activePlayerData->SpellCritPercentage[SPELL_SCHOOL_ARCANE]));
        handler->PSendSysMessage("  haste mod melee %.4f spell %.4f ranged %.4f mastery %.2f dodge %.2f parry %.2f block %.2f", float(p->m_unitData->ModHaste), float(p->m_unitData->ModSpellHaste), float(p->m_unitData->ModRangedHaste),
            float(p->m_activePlayerData->Mastery), float(p->m_activePlayerData->DodgePercentage), float(p->m_activePlayerData->ParryPercentage), float(p->m_activePlayerData->BlockPercentage));
        handler->PSendSysMessage("  ratings hit(melee/ranged/spell) %d/%d/%d crit(melee/ranged/spell) %d/%d/%d haste(melee/ranged/spell) %d/%d/%d expertise %d mastery %d",
            int32(p->m_activePlayerData->CombatRatings[CR_HIT_MELEE]), int32(p->m_activePlayerData->CombatRatings[CR_HIT_RANGED]), int32(p->m_activePlayerData->CombatRatings[CR_HIT_SPELL]),
            int32(p->m_activePlayerData->CombatRatings[CR_CRIT_MELEE]), int32(p->m_activePlayerData->CombatRatings[CR_CRIT_RANGED]), int32(p->m_activePlayerData->CombatRatings[CR_CRIT_SPELL]),
            int32(p->m_activePlayerData->CombatRatings[CR_HASTE_MELEE]), int32(p->m_activePlayerData->CombatRatings[CR_HASTE_RANGED]), int32(p->m_activePlayerData->CombatRatings[CR_HASTE_SPELL]),
            int32(p->m_activePlayerData->CombatRatings[CR_EXPERTISE]), int32(p->m_activePlayerData->CombatRatings[CR_MASTERY]));
        std::string equip;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                equip += Trinity::StringFormat("{}{}:{}", equip.empty() ? "" : " ", uint32(slot), item->GetTemplate()->GetId());
        handler->PSendSysMessage("  equipment %s", equip.empty() ? "(none)" : equip.c_str());
        std::string items;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                items += Trinity::StringFormat("{}{}:{}:ench={}:gems={}/{}/{}:bonus={}:prismatic={}:reforge={}:random={}", items.empty() ? "" : " ", uint32(slot), item->GetEntry(),
                    item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT), item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT), item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_2), item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_3),
                    item->GetEnchantmentId(BONUS_ENCHANTMENT_SLOT), item->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT), item->GetModifier(ITEM_MODIFIER_REFORGE), item->GetRandomPropertiesId());
        handler->PSendSysMessage("  items %s", items.empty() ? "(none)" : items.c_str());
        std::string glyphs;
        for (uint32 i = 0; i < p->m_activePlayerData->Glyphs.size(); ++i)
            glyphs += Trinity::StringFormat("{}{}", i ? "/" : "", uint32(p->m_activePlayerData->Glyphs[i]));
        uint32 masterySpell = ClassMasterySpell(p->GetClass());
        handler->PSendSysMessage("  talents primary %u points_left %d mastery_spell %u known %u can_use_mastery %u glyphs %s",
            p->GetPrimaryTalentTree(), int32(p->m_activePlayerData->CharacterPoints), masterySpell, uint32(p->HasSpell(masterySpell)), uint32(p->CanUseMastery()), glyphs.c_str());
        return true;
    }

    static bool HandleHeadlessSelectNone(ChatHandler* handler, std::string name)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        player->SetSelection(ObjectGuid::Empty);
        handler->PSendSysMessage("Headless: '%s' selection cleared.", name.c_str());
        return true;
    }

    static bool HandleHeadlessEquip(ChatHandler* handler, std::string name, uint32 itemId, Optional<int32> randomPropertiesId)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        if (!sObjectMgr->GetItemTemplate(itemId))
        {
            handler->PSendSysMessage("Headless: item %u does not exist.", itemId);
            handler->SetSentErrorMessage(true);
            return false;
        }
        Item* item = player->GetItemByEntry(itemId, ItemSearchLocation::Inventory);
        if (!item)
        {
            ItemPosCountVec dest;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, 1) != EQUIP_ERR_OK)
            {
                handler->PSendSysMessage("Headless: cannot add item %u (bags full?).", itemId);
                handler->SetSentErrorMessage(true);
                return false;
            }
            item = player->StoreNewItem(dest, itemId, true, GenerateItemRandomBonusListId(itemId), randomPropertiesId.value_or(0));
            if (!item)
            {
                handler->PSendSysMessage("Headless: StoreNewItem failed for %u.", itemId);
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        uint16 equipDest = 0;
        InventoryResult res = player->CanEquipItem(NULL_SLOT, equipDest, item, true);
        if (res != EQUIP_ERR_OK)
        {
            handler->PSendSysMessage("Headless: cannot equip item %u, InventoryResult %u.", itemId, uint32(res));
            handler->SetSentErrorMessage(true);
            return false;
        }
        // unequip whatever sits in the destination slot first (destroy: test characters only)
        uint8 slot = equipDest & 255;
        if (Item* old = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (old != item)
                player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        player->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        Item* equipped = player->EquipItem(equipDest, item, true);
        player->AutoUnequipOffhandIfNeed();
        handler->PSendSysMessage("Headless: '%s' equipped %u in slot %u -> %s, randomProperties %d.", name.c_str(), itemId, uint32(slot), equipped ? "ok" : "failed", equipped ? equipped->GetRandomPropertiesId() : 0);
        return equipped != nullptr;
    }

    static Item* FindEquippedItem(ChatHandler* handler, Player* player, uint32 slot)
    {
        if (slot >= EQUIPMENT_SLOT_END)
        {
            handler->SendSysMessage("Headless: slot must be 0..18.");
            handler->SetSentErrorMessage(true);
            return nullptr;
        }
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(slot));
        if (!item)
        {
            handler->PSendSysMessage("Headless: nothing equipped in slot %u.", slot);
            handler->SetSentErrorMessage(true);
        }
        return item;
    }

    // Same sequence as Spell::EffectEnchantItemPerm, without the spell. "prismatic" is the belt-buckle socket enchant.
    static bool HandleHeadlessEnchant(ChatHandler* handler, std::string name, uint32 slot, uint32 enchantId, Optional<std::string> kind)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        Item* item = FindEquippedItem(handler, player, slot);
        if (!item)
            return false;
        if (enchantId && !sSpellItemEnchantmentStore.LookupEntry(enchantId))
        {
            handler->PSendSysMessage("Headless: enchant %u does not exist.", enchantId);
            handler->SetSentErrorMessage(true);
            return false;
        }
        EnchantmentSlot enchSlot = PERM_ENCHANTMENT_SLOT;
        if (kind && *kind == "prismatic")
            enchSlot = PRISMATIC_ENCHANTMENT_SLOT;
        else if (kind && *kind != "perm")
        {
            handler->SendSysMessage("Headless: enchant kind must be perm or prismatic.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        player->ApplyEnchantment(item, enchSlot, false);
        item->SetEnchantment(enchSlot, enchantId, 0, 0, player->GetGUID());
        player->ApplyEnchantment(item, enchSlot, true);
        handler->PSendSysMessage("Headless: '%s' slot %u item %u enchant[%u] = %u.", name.c_str(), slot, item->GetEntry(), uint32(enchSlot), item->GetEnchantmentId(enchSlot));
        return true;
    }

    // Same sequence as WorldSession::HandleSocketGems, without the gem items in the bags and without the colour checks:
    // the test decides what to socket, the sheet shows what came out. Gems are given in socket order; a belt-buckle
    // (prismatic) socket is the first index without a colour and needs the prismatic enchant set first.
    static bool HandleHeadlessGem(ChatHandler* handler, std::string name, uint32 slot, uint32 gem1, Optional<uint32> gem2, Optional<uint32> gem3)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        Item* item = FindEquippedItem(handler, player, slot);
        if (!item)
            return false;

        std::array<uint32, MAX_GEM_SOCKETS> gems = { gem1, gem2.value_or(0), gem3.value_or(0) };
        std::array<GemPropertiesEntry const*, MAX_GEM_SOCKETS> gemProperties = { };
        std::array<ItemDynamicFieldGems, MAX_GEM_SOCKETS> gemData = { };
        uint32 firstPrismatic = 0;
        while (firstPrismatic < MAX_GEM_SOCKETS && item->GetSocketColor(firstPrismatic))
            ++firstPrismatic;
        for (uint32 i = 0; i < MAX_GEM_SOCKETS; ++i)
        {
            if (!gems[i])
                continue;
            ItemTemplate const* gemTemplate = sObjectMgr->GetItemTemplate(gems[i]);
            if (!gemTemplate || !gemTemplate->GetGemProperties())
            {
                handler->PSendSysMessage("Headless: %u is not a gem.", gems[i]);
                handler->SetSentErrorMessage(true);
                return false;
            }
            gemProperties[i] = sGemPropertiesStore.LookupEntry(gemTemplate->GetGemProperties());
            if (!gemProperties[i])
            {
                handler->PSendSysMessage("Headless: gem %u has no GemProperties row.", gems[i]);
                handler->SetSentErrorMessage(true);
                return false;
            }
            if (!item->GetSocketColor(i) && (i != firstPrismatic || !item->GetEnchantmentId(PRISMATIC_ENCHANTMENT_SLOT)))
            {
                handler->PSendSysMessage("Headless: item %u has no socket %u (set the prismatic enchant first for a belt buckle).", item->GetEntry(), i);
                handler->SetSentErrorMessage(true);
                return false;
            }
            gemData[i].ItemId = gems[i];
            gemData[i].Context = 0;
        }

        bool socketBonusActivated = item->GemsFitSockets();
        player->ToggleMetaGemsActive(uint8(slot), false);
        player->_ApplyItemMods(item, uint8(slot), false);
        for (uint16 i = 0; i < MAX_GEM_SOCKETS; ++i)
        {
            if (!gems[i])
                continue;
            item->SetGem(i, &gemData[i], player->GetLevel());
            if (gemProperties[i]->EnchantID)
                item->SetEnchantment(EnchantmentSlot(SOCK_ENCHANTMENT_SLOT + i), gemProperties[i]->EnchantID, 0, 0, player->GetGUID());
        }
        player->_ApplyItemMods(item, uint8(slot), true);
        bool socketBonusToBeActivated = item->GemsFitSockets();
        if (socketBonusActivated != socketBonusToBeActivated)
        {
            player->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, false);
            item->SetEnchantment(BONUS_ENCHANTMENT_SLOT, socketBonusToBeActivated ? item->GetTemplate()->GetSocketBonus() : 0, 0, 0, player->GetGUID());
            player->ApplyEnchantment(item, BONUS_ENCHANTMENT_SLOT, true);
        }
        player->ToggleMetaGemsActive(uint8(slot), true);
        handler->PSendSysMessage("Headless: '%s' slot %u item %u gems %u/%u/%u socketBonus %u.", name.c_str(), slot, item->GetEntry(),
            item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT), item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_2), item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT_3), item->GetEnchantmentId(BONUS_ENCHANTMENT_SLOT));
        return true;
    }

    // Same sequence as WorldSession::HandleReforgeItem, without the NPC and the fee.
    static bool HandleHeadlessReforge(ChatHandler* handler, std::string name, uint32 slot, uint32 reforgeId)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        Item* item = FindEquippedItem(handler, player, slot);
        if (!item)
            return false;
        if (reforgeId && !sItemReforgeStore.LookupEntry(reforgeId))
        {
            handler->PSendSysMessage("Headless: ItemReforge %u does not exist.", reforgeId);
            handler->SetSentErrorMessage(true);
            return false;
        }
        player->ApplyReforgedStats(item, false);
        item->SetReforgeId(reforgeId);
        player->ApplyReforgedStats(item, true);
        handler->PSendSysMessage("Headless: '%s' slot %u item %u reforge = %u.", name.c_str(), slot, item->GetEntry(), item->GetModifier(ITEM_MODIFIER_REFORGE));
        return true;
    }

    // Class mastery passives (Player.cpp keeps the same table private to CanUseMastery).
    static uint32 ClassMasterySpell(uint8 classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR: return 87500;
            case CLASS_PALADIN: return 87494;
            case CLASS_HUNTER: return 87493;
            case CLASS_ROGUE: return 87496;
            case CLASS_PRIEST: return 87495;
            case CLASS_DEATH_KNIGHT: return 87492;
            case CLASS_SHAMAN: return 87497;
            case CLASS_MAGE: return 86467;
            case CLASS_WARLOCK: return 87498;
            case CLASS_DRUID: return 87491;
            default: return 0;
        }
    }

    // WoWSims talent string: one block per talent tab in TalentTab.OrderIndex order, one digit per talent in
    // (TierID, ColumnIndex) order, digit = ranks taken. Goes through Player::LearnTalent so that the ranks are
    // real talents (respec-able, counted against CharacterPoints), and through SetPrimaryTalentTree so that the
    // tree's primary spells and the mastery spell arrive the way they do for a client.
    static bool HandleHeadlessTalents(ChatHandler* handler, std::string name, std::string talentString)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;

        std::vector<std::string_view> blocks = Trinity::Tokenize(talentString, '-', true);
        struct TabPlan { TalentTabEntry const* Tab; std::vector<TalentEntry const*> Talents; std::string_view Digits; uint32 Points; };
        std::vector<TabPlan> plan;
        for (int32 tabIndex = 0; tabIndex < int32(blocks.size()); ++tabIndex)
        {
            TalentTabEntry const* tab = sDB2Manager.GetTalentTabByIndex(player->GetClass(), tabIndex);
            if (!tab)
            {
                handler->PSendSysMessage("Headless: class %u has no talent tab with index %d.", uint32(player->GetClass()), tabIndex);
                handler->SetSentErrorMessage(true);
                return false;
            }
            TabPlan& tp = plan.emplace_back();
            tp.Tab = tab;
            tp.Digits = blocks[tabIndex];
            tp.Points = 0;
            for (TalentEntry const* talent : sTalentStore)
                if (talent->TabID == tab->ID)
                    tp.Talents.push_back(talent);
            std::sort(tp.Talents.begin(), tp.Talents.end(), [](TalentEntry const* a, TalentEntry const* b)
            {
                return std::tie(a->TierID, a->ColumnIndex) < std::tie(b->TierID, b->ColumnIndex);
            });
            if (tp.Digits.size() > tp.Talents.size())
            {
                handler->PSendSysMessage("Headless: tab %u (%s) has %u talents but the string block has %u digits.", tab->ID, tab->Name[handler->GetSessionDbcLocale()], uint32(tp.Talents.size()), uint32(tp.Digits.size()));
                handler->SetSentErrorMessage(true);
                return false;
            }
            for (char c : tp.Digits)
            {
                if (c < '0' || c > '9')
                {
                    handler->PSendSysMessage("Headless: talent string block %d is not all digits.", tabIndex);
                    handler->SetSentErrorMessage(true);
                    return false;
                }
                tp.Points += uint32(c - '0');
            }
        }

        player->ResetTalents(true);

        // primary tree: the tab with the most points (Cataclysm rule: 31 points there before any other tab)
        TabPlan const* primary = nullptr;
        for (TabPlan const& tp : plan)
            if (tp.Points && (!primary || tp.Points > primary->Points))
                primary = &tp;

        uint32 masterySpell = ClassMasterySpell(player->GetClass());
        if (masterySpell && !player->HasSpell(masterySpell))
            player->LearnSpell(masterySpell, false);

        if (primary)
            player->SetPrimaryTalentTree(primary->Tab->ID);

        uint32 learned = 0, failed = 0;
        std::string failedList;
        for (TabPlan const& tp : plan)
        {
            for (std::size_t i = 0; i < tp.Digits.size(); ++i)
            {
                uint32 ranks = uint32(tp.Digits[i] - '0');
                if (!ranks)
                    continue;
                TalentEntry const* talent = tp.Talents[i];
                if (player->LearnTalent(talent->ID, uint8(ranks - 1)))
                    learned += ranks;
                else
                {
                    ++failed;
                    failedList += Trinity::StringFormat(" {}(tab {} #{} x{})", talent->ID, tp.Tab->ID, uint32(i), ranks);
                }
            }
        }

        handler->PSendSysMessage("Headless: '%s' talents: %u ranks learned, %u talents failed%s; primary tree %u, points left %d, mastery spell %u known %u, CanUseMastery %u.",
            name.c_str(), learned, failed, failedList.c_str(), player->GetPrimaryTalentTree(), int32(player->m_activePlayerData->CharacterPoints),
            masterySpell, uint32(player->HasSpell(masterySpell)), uint32(player->CanUseMastery()));
        return failed == 0;
    }

    static bool HandleHeadlessGlyph(ChatHandler* handler, std::string name, uint32 index, uint32 glyphPropertiesId)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        if (index >= player->m_activePlayerData->Glyphs.size())
        {
            handler->PSendSysMessage("Headless: glyph index must be 0..%u.", uint32(player->m_activePlayerData->Glyphs.size() - 1));
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (glyphPropertiesId && !sGlyphPropertiesStore.LookupEntry(glyphPropertiesId))
        {
            handler->PSendSysMessage("Headless: GlyphProperties %u does not exist.", glyphPropertiesId);
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (player->m_activePlayerData->Glyphs[index])
            player->RemoveGlyph(uint8(index));
        if (glyphPropertiesId)
            player->ApplyGlyph(uint8(index), glyphPropertiesId);
        std::string glyphs;
        for (uint32 i = 0; i < player->m_activePlayerData->Glyphs.size(); ++i)
            glyphs += Trinity::StringFormat("{}{}", i ? "/" : "", uint32(player->m_activePlayerData->Glyphs[i]));
        handler->PSendSysMessage("Headless: '%s' glyph[%u] = %u, glyphs %s.", name.c_str(), index, uint32(player->m_activePlayerData->Glyphs[index]), glyphs.c_str());
        return true;
    }

    // Damage sampling without cast time, GCD or mana: N triggered casts in one world tick, procs allowed. The recorder sees
    // cast_start/cast_go/spell_damage for each. Not for auras/DoTs (a re-cast refreshes the aura, it does not
    // add ticks) and not a check of cast time or cost, that is what .headless cast is for.
    static bool HandleHeadlessBurst(ChatHandler* handler, std::string name, uint32 spellId, uint32 count)
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
        if (!count || count > 1000)
        {
            handler->SendSysMessage("Headless: count must be 1..1000.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        Unit* target = ObjectAccessor::GetUnit(*player, player->GetTarget());
        if (!target)
            target = player;
        uint32 ok = 0;
        SpellCastResult last = SPELL_CAST_OK;
        for (uint32 i = 0; i < count; ++i)
        {
            SpellCastResult result = player->CastSpell(target, spellId, CastSpellExtraArgs(TriggerCastFlags(TRIGGERED_FULL_MASK & ~TRIGGERED_DISALLOW_PROC_EVENTS)));
            if (result == SPELL_CAST_OK)
                ++ok;
            else
                last = result;
        }
        handler->PSendSysMessage("Headless: '%s' burst %u x%u on %s -> %u ok, %u failed, last failure SpellCastResult %u.",
            name.c_str(), spellId, count, target->GetName().c_str(), ok, count - ok, uint32(last));
        return ok == count;
    }

    static bool HandleHeadlessUnequip(ChatHandler* handler, std::string name, std::string slotArg)
    {
        Player* player = FindHeadlessPlayer(handler, name);
        if (!player)
            return false;
        uint32 count = 0;
        if (slotArg == "all")
        {
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                {
                    player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                    ++count;
                }
        }
        else
        {
            Optional<uint32> slot = Trinity::StringTo<uint32>(slotArg);
            if (!slot || *slot >= EQUIPMENT_SLOT_END)
            {
                handler->SendSysMessage("Headless: slot must be 0..18 or 'all'.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, uint8(*slot)))
            {
                player->DestroyItem(INVENTORY_SLOT_BAG_0, uint8(*slot), true);
                ++count;
            }
        }
        handler->PSendSysMessage("Headless: '%s' unequipped (destroyed) %u item(s).", name.c_str(), count);
        return true;
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

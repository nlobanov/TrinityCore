/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Port provenance
 * ---------------
 * Base source : Firelands-Core, GPL-2.0, snapshot ~/projects/wow-kb/src/Firelands-Core
 *               (files dated 2026-09-21), scripts/EasternKingdoms/BlackrockMountain/
 *               BlackrockCaverns/boss_romogg_bonecrusher.cpp.
 * Grafted from: Starfall 4.3.4, GPL-2.0, snapshot ~/projects/wow-kb/src/starfall-434,
 *               scripts/EasternKingdoms/BlackrockCaverns/boss_romogg_bonecrusher.cpp:236-251
 *               - Chains of Woe fires at 66% and 33% health, not on a timer.
 * API port to TrinityCore cata_classic 4.4.2.60895 (branch feature/headless).
 *
 * Spell / creature id review against 4.4.2.60895 (tools/kb/kb.py):
 *   - No 4.3.4 heroic clone ids were carried over; this script had none.
 *   - 75539 Chains of Woe summons NPC 40447 at TARGET_DEST_CASTER_BACK (48) in 4.4.2,
 *     not TARGET_DEST_CASTER_FRONT as in the Firelands-Core snapshot; the destination
 *     hook is registered on the 4.4.2 target.
 *   - 75437 Chains of Woe (teleport) and 82189 Chains of Woe (root) keep the script
 *     names TDB 442 already ships (spell_chains_of_woe_1 / spell_chains_of_woe_4 in
 *     blackrock_caverns.cpp); they are not duplicated here.
 *   - The Quake chain is data driven in 4.4.2: 75272 effect 1 -> 75372 (summons 40401),
 *     40401 carries 75379 Burrow Visual, 75379 triggers 75347 Ground Rupture, and
 *     75347 summons 50376 Angered Earth on difficulty 2/4 only. The missing
 *     creature_template_addon row for 40401 is restored by sql/custom/0009.
 *
 * Timers (oracle: docs/oracles/wcl/heroic-dungeons/blackrock-caverns.md medians,
 * docs/findings/brc-timeline.md):
 *   - Call for Help 82137 at aggro                      (WCL first 0.0 s)
 *   - Wounding Strike 75571 first 17.5 s, then 22 s     (WCL 17.5 / median 22.1 s)
 *   - Quake 75272 first 26 s, then 23-34 s              (WCL 26.0 s; cycles 23, 29, 32, 34 s)
 *   - Chains of Woe 75539 at 66% and 33% health         (wiki, BigWigs, WCL: 2 casts/fight)
 *   - The Skullcracker 75543 3 s after Chains of Woe    (WCL 3.3 s, bench 3.0 s)
 */

#include "ScriptMgr.h"
#include "blackrock_caverns.h"
#include "InstanceScript.h"
#include "Map.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "SpellScript.h"

enum RomoggTexts
{
    SAY_AGGRO                       = 0,
    SAY_SLAY                        = 1,
    SAY_CHAINS_OF_WOE               = 2,
    SAY_DEATH                       = 3,
    SAY_EMOTE_CALL_FOR_HELP         = 4,
    SAY_ANNOUNCE_SKULLCRACKER       = 5
};

enum RomoggSpells
{
    SPELL_CALL_FOR_HELP             = 82137,
    SPELL_CHAINS_OF_WOE             = 75539,
    SPELL_QUAKE                     = 75272,
    SPELL_THE_SKULLCRACKER          = 75543,
    SPELL_WOUNDING_STRIKE           = 75571
};

enum RomoggEvents
{
    EVENT_CHAINS_OF_WOE             = 1,
    EVENT_QUAKE,
    EVENT_SKULLCRACKER,
    EVENT_WOUNDING_STRIKE
};

enum RomoggData
{
    DATA_CRUSHING_BONES_AND_CRACKING_SKULLS = 0
};

enum RomoggMisc
{
    TYPE_RAZ                        = 1,
    DATA_ROMOGG_DEAD                = 1,

    // Achievement 5281 "Crushing Bones and Cracking Skulls" (Criteria 15943)
    ANGERED_EARTH_KILLS_REQUIRED    = 10,

    // Chains of Woe teleport destination, 3 yards in front of the chains (DBC radius index 28)
    CHAINS_OF_WOE_TELEPORT_DISTANCE = 3
};

Position const RazTheCrazedSummonPos = { 249.2639f, 949.1614f, 191.7866f, 3.141593f };

struct boss_romogg_bonecrusher : public BossAI
{
    boss_romogg_bonecrusher(Creature* creature) : BossAI(creature, DATA_ROMOGG_BONECRUSHER), _killedElementals(0), _chainsOfWoePhase(0)
    {
        // Raz hangs in his cage above the room for the whole encounter; npc_raz_the_crazed
        // (blackrock_caverns.cpp) only reacts when Rom'ogg is the summoner.
        me->SummonCreature(NPC_RAZ_THE_CRAZED, RazTheCrazedSummonPos, TEMPSUMMON_MANUAL_DESPAWN, 200s);
    }

    void Reset() override
    {
        _Reset();
        _killedElementals = 0;
        _chainsOfWoePhase = 0;
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_ENGAGE, me);
        Talk(SAY_AGGRO);
        Talk(SAY_EMOTE_CALL_FOR_HELP);
        DoCastSelf(SPELL_CALL_FOR_HELP);

        events.ScheduleEvent(EVENT_WOUNDING_STRIKE, 17500ms);
        events.ScheduleEvent(EVENT_QUAKE, 26s);
    }

    void KilledUnit(Unit* who) override
    {
        if (who->IsPlayer())
            Talk(SAY_SLAY);

        if (who->GetEntry() == NPC_ANGERED_EARTH)
            ++_killedElementals;
    }

    void JustDied(Unit* /*killer*/) override
    {
        // Before _JustDied(): it despawns the summon list and Raz must outlive it.
        if (Creature* raz = instance->GetCreature(DATA_RAZ_THE_CRAZED))
            raz->AI()->SetData(TYPE_RAZ, DATA_ROMOGG_DEAD);

        _JustDied();
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
        Talk(SAY_DEATH);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        _EnterEvadeMode();
        summons.DespawnAll();
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
        _DespawnAtEvade();
    }

    void JustSummoned(Creature* summon) override
    {
        // Raz belongs to the instance, not to the encounter: he must survive the wipe cleanup.
        if (summon->GetEntry() == NPC_RAZ_THE_CRAZED)
            return;

        BossAI::JustSummoned(summon);
    }

    void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* /*spellInfo*/) override
    {
        // Chains of Woe is a health trigger, not a timer: 66% and 33%.
        if (_chainsOfWoePhase == 0 && me->HealthBelowPctDamaged(66, damage))
        {
            ++_chainsOfWoePhase;
            events.ScheduleEvent(EVENT_CHAINS_OF_WOE, 1ms);
        }
        else if (_chainsOfWoePhase == 1 && me->HealthBelowPctDamaged(33, damage))
        {
            ++_chainsOfWoePhase;
            events.ScheduleEvent(EVENT_CHAINS_OF_WOE, 1ms);
        }
    }

    uint32 GetData(uint32 type) const override
    {
        if (type == DATA_CRUSHING_BONES_AND_CRACKING_SKULLS)
            return _killedElementals >= ANGERED_EARTH_KILLS_REQUIRED;

        return 0;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        events.Update(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_CHAINS_OF_WOE:
                    Talk(SAY_CHAINS_OF_WOE);
                    me->AttackStop();
                    me->SetReactState(REACT_PASSIVE);
                    DoCastSelf(SPELL_CHAINS_OF_WOE);
                    events.ScheduleEvent(EVENT_SKULLCRACKER, 3s);
                    break;
                case EVENT_SKULLCRACKER:
                    Talk(SAY_ANNOUNCE_SKULLCRACKER);
                    DoCastSelf(SPELL_THE_SKULLCRACKER);
                    me->SetReactState(REACT_AGGRESSIVE);
                    break;
                case EVENT_QUAKE:
                    DoCastSelf(SPELL_QUAKE);
                    events.Repeat(23s, 34s);
                    break;
                case EVENT_WOUNDING_STRIKE:
                    DoCastVictim(SPELL_WOUNDING_STRIKE);
                    events.Repeat(22s);
                    break;
                default:
                    break;
            }
        }
    }

private:
    uint8 _killedElementals;
    uint8 _chainsOfWoePhase;
};

// 75272 - Quake
class spell_romogg_quake : public SpellScript
{
    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellEffect({ { spellInfo->Id, EFFECT_1 } })
            && ValidateSpellInfo({ static_cast<uint32>(spellInfo->GetEffect(EFFECT_1).BasePoints) });
    }

    void HandleScriptEffect(SpellEffIndex effIndex)
    {
        Unit* target = GetHitUnit();
        target->CastSpell(target, static_cast<uint32>(GetSpellInfo()->GetEffect(effIndex).BasePoints), true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_romogg_quake::HandleScriptEffect, EFFECT_1, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// 75539 - Chains of Woe
class spell_romogg_chains_of_woe : public SpellScript
{
    void SetDest(SpellDestination& dest)
    {
        // The chains model sits in the floor without the lift.
        Position offset = { 0.0f, 0.0f, 1.6f, 0.0f };
        dest.RelocateOffset(offset);
    }

    void Register() override
    {
        OnDestinationTargetSelect += SpellDestinationTargetSelectFn(spell_romogg_chains_of_woe::SetDest, EFFECT_0, TARGET_DEST_CASTER_BACK);
    }
};

// 75464 - Chains of Woe
class spell_romogg_chains_of_woe_teleport_dest : public SpellScript
{
    void SetDest(SpellDestination& dest)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        Position pos = dest._position;
        float angle = pos.GetAbsoluteAngle(caster);
        pos.m_positionX += std::cos(angle) * float(CHAINS_OF_WOE_TELEPORT_DISTANCE);
        pos.m_positionY += std::sin(angle) * float(CHAINS_OF_WOE_TELEPORT_DISTANCE);
        pos.m_positionZ = caster->GetMap()->GetHeight(caster->GetPhaseShift(), pos.GetPositionX(), pos.GetPositionY(), caster->GetPositionZ() + 5.0f, true);
        dest.Relocate(pos);
    }

    void Register() override
    {
        OnDestinationTargetSelect += SpellDestinationTargetSelectFn(spell_romogg_chains_of_woe_teleport_dest::SetDest, EFFECT_0, TARGET_DEST_TARGET_RADIUS);
    }
};

// 82137 - Call for Help
class spell_romogg_call_for_help : public SpellScript
{
    void HandleScriptEffect(SpellEffIndex /*effIndex*/)
    {
        if (Creature* creature = GetHitCreature())
            if (creature->IsAIEnabled())
                creature->AI()->DoZoneInCombat();
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_romogg_call_for_help::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// Achievement 5281 - Crushing Bones and Cracking Skulls (Criteria 15943)
class achievement_crushing_bones_and_cracking_skulls : public AchievementCriteriaScript
{
public:
    achievement_crushing_bones_and_cracking_skulls() : AchievementCriteriaScript("achievement_crushing_bones_and_cracking_skulls") { }

    bool OnCheck(Player* /*source*/, Unit* target) override
    {
        if (!target)
            return false;

        if (target->IsAIEnabled())
            return target->GetAI()->GetData(DATA_CRUSHING_BONES_AND_CRACKING_SKULLS) != 0;

        return false;
    }
};

void AddSC_boss_romogg_bonecrusher()
{
    RegisterBlackrockCavernsCreatureAI(boss_romogg_bonecrusher);
    RegisterSpellScript(spell_romogg_quake);
    RegisterSpellScript(spell_romogg_chains_of_woe);
    RegisterSpellScript(spell_romogg_chains_of_woe_teleport_dest);
    RegisterSpellScript(spell_romogg_call_for_help);
    new achievement_crushing_bones_and_cracking_skulls();
}

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
 *               BlackrockCaverns/boss_corla_herald_of_twilight.cpp.
 * API port to TrinityCore cata_classic 4.4.2.60895 (branch feature/headless).
 *
 * Id replacements against 4.4.2.60895 (tools/kb/kb.py spell):
 *   - 87376 Aura of Acceleration (4.3.4 heroic clone, absent)  -> 75817.
 *   - 87378 Evolution heroic stacks (absent)                   -> 75697. 4.4.2 ships 75697
 *     with CumulativeAura = 100 on difficulties 0, 2 and 4 alike, so there is no separate
 *     heroic stack aura and the snapshot's "apply the stack twice on heroic" branch is
 *     dropped; DB2 gives no per-difficulty stack rate to replace it with.
 *   - RAID_MODE(75823, 93462) Dark Command (93462 absent)      -> plain 75823.
 *   - 61400 Wear Christmas Hat (Winter Veil gag in the snapshot) dropped: it is cosmetic
 *     and not part of the encounter.
 *   - The nether beam script belongs on 75676 Nether Beam (the cone SCRIPT_EFFECT that
 *     75706 Nether Beam Periodic triggers every 400 ms), not on 75706 itself.
 *   - 75649/75650/75653/75654 Nether Dragon Essence keep the script names TDB 442 already
 *     ships (spell_nether_dragon_essence_1 / _2 in blackrock_caverns.cpp); not duplicated.
 *
 * Zealots: static spawns, not summons. TDB 442 spawns three Twilight Zealots (50284) at
 * exactly the three positions the Firelands-Core snapshot summons them at, and the third
 * one (guid 250021, 573.484 978.585 155.437) carries spawnDifficulties = 2, so the data
 * already encodes the Journal's 2 normal / 3 heroic split. The script therefore picks the
 * zealots up from the grid instead of summoning them.
 *
 * Achievement 5282 "Arrested Development" reads, in 4.4.2 DB2: "Allow all three of Corla's
 * zealots to evolve, then defeat Corla after slaying the evolved zealots." The check is
 * three evolved zealots slain, not three zealots killed before any evolves.
 *
 * Timers (oracle: docs/oracles/wcl/heroic-dungeons/blackrock-caverns.md medians,
 * docs/findings/brc-timeline.md, DBM-Party-Cataclysm/BlackrockCaverns/Corla.lua):
 *   - Out of combat, on a 20.5 s cycle: Evolution 75610 at +0 s and +1 s, Drain Essence
 *     75645 at +2 s and +3 s (WCL series 0 20 21 41 42 ... and 2 3 23 24 ...).
 *   - Dark Command 75823 first 22 s (DBM bar), then every 24 s (WCL median 24.3 s).
 *   - Aura of Acceleration 75817 on aggro (WCL 2 casts/fight, one per pull).
 */

#include "ScriptMgr.h"
#include "blackrock_caverns.h"
#include "InstanceScript.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ScriptedCreature.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"

enum CorlaTexts
{
    SAY_AGGRO                           = 0,
    SAY_SLAY                            = 1,
    SAY_EVOLUTION_COMPLETE              = 2,
    SAY_DEATH                           = 3,
    SAY_ANNOUNCE_ZEALOT_EVOLVED         = 4
};

enum CorlaSpells
{
    // Corla, Herald of Twilight
    SPELL_EVOLUTION                     = 75610,
    SPELL_DRAIN_ESSENCE                 = 75645,
    SPELL_AURA_OF_ACCELERATION          = 75817,
    SPELL_DARK_COMMAND                  = 75823,
    SPELL_TWILIGHT_EVOLUTION            = 75732,

    // Twilight Zealot
    SPELL_KNEELING_SUPPLICATION         = 75608,
    SPELL_NETHER_BEAM_PERIODIC          = 75706,
    SPELL_NETHER_BEAM_VISUAL            = 75677,
    SPELL_EVOLUTION_STACKS              = 75697,

    // Evolved Twilight Zealot
    SPELL_GRIEVOUS_WHIRL                = 76524,
    SPELL_SHADOW_STRIKE                 = 82362,
    SPELL_INVISIBILITY_AND_STEALTH_DETECTION = 18950,
    SPELL_FORCE_BLAST                   = 76522,
    SPELL_GRAVITY_STRIKE                = 76561
};

enum CorlaEvents
{
    // Corla, Herald of Twilight
    EVENT_DRAIN_ESSENCE                 = 1,
    EVENT_EVOLUTION,
    EVENT_DARK_COMMAND,

    // Evolved Twilight Zealot
    EVENT_GRIEVOUS_WHIRL,
    EVENT_SHADOW_STRIKE,
    EVENT_INVISIBILITY_AND_STEALTH_DETECTION,
    EVENT_GRAVITY_STRIKE,
    EVENT_FORCE_BLAST
};

enum CorlaPhases
{
    PHASE_OUT_OF_COMBAT                 = 1,
    PHASE_IN_COMBAT                     = 2
};

enum CorlaActions
{
    ACTION_START_EVOLUTION              = 0,
    ACTION_EVOLVE                       = 1,
    ACTION_RESET_ZEALOT                 = 2,
    ACTION_EVOLVED_ZEALOT_SLAIN         = 3
};

enum CorlaData
{
    DATA_ARRESTED_DEVELOPMENT           = 0
};

enum CorlaMisc
{
    SPELL_VISUAL_KIT_EVOLUTION_WARNING  = 16957,
    EVOLUTION_WARNING_STACKS            = 75,
    EVOLVED_ZEALOTS_REQUIRED            = 3,
    ZEALOT_SEARCH_RANGE                 = 40
};

struct boss_corla : public BossAI
{
    boss_corla(Creature* creature) : BossAI(creature, DATA_CORLA), _slainEvolvedZealots(0), _evolutionCastsInPair(0), _drainCastsInPair(0) { }

    void Reset() override
    {
        _Reset();
        _slainEvolvedZealots = 0;
        _evolutionCastsInPair = 0;
        _drainCastsInPair = 0;

        events.SetPhase(PHASE_OUT_OF_COMBAT);
        events.ScheduleEvent(EVENT_EVOLUTION, 1ms, 0, PHASE_OUT_OF_COMBAT);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        instance->SendEncounterUnit(ENCOUNTER_FRAME_ENGAGE, me);
        Talk(SAY_AGGRO);
        me->CastStop();
        DoCastSelf(SPELL_AURA_OF_ACCELERATION);

        events.SetPhase(PHASE_IN_COMBAT);
        events.ScheduleEvent(EVENT_DARK_COMMAND, 22s, 0, PHASE_IN_COMBAT);

        for (Creature* zealot : GetZealots())
            if (zealot->IsAIEnabled())
                zealot->AI()->DoAction(ACTION_START_EVOLUTION);
    }

    void KilledUnit(Unit* who) override
    {
        if (who->IsPlayer())
            Talk(SAY_SLAY);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        _EnterEvadeMode();
        summons.DespawnAll();
        ClearAuras();
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);

        for (Creature* zealot : GetZealots())
            if (zealot->IsAIEnabled())
                zealot->AI()->DoAction(ACTION_RESET_ZEALOT);

        _DespawnAtEvade();
    }

    void JustDied(Unit* /*killer*/) override
    {
        _JustDied();
        Talk(SAY_DEATH);
        ClearAuras();
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
    }

    void DoAction(int32 action) override
    {
        if (action == ACTION_EVOLVED_ZEALOT_SLAIN)
            ++_slainEvolvedZealots;
    }

    uint32 GetData(uint32 type) const override
    {
        if (type == DATA_ARRESTED_DEVELOPMENT)
            return _slainEvolvedZealots >= EVOLVED_ZEALOTS_REQUIRED;

        return 0;
    }

    void UpdateAI(uint32 diff) override
    {
        bool const inCombat = !events.IsInPhase(PHASE_OUT_OF_COMBAT);

        if (inCombat && !UpdateVictim())
            return;

        events.Update(diff);

        if (inCombat && me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_EVOLUTION:
                    // Two casts one second apart, the pair repeating every 20.5 seconds.
                    me->CastStop();
                    DoCastSelf(SPELL_EVOLUTION);
                    if (++_evolutionCastsInPair < 2)
                        events.ScheduleEvent(EVENT_EVOLUTION, 1s, 0, PHASE_OUT_OF_COMBAT);
                    else
                    {
                        _evolutionCastsInPair = 0;
                        _drainCastsInPair = 0;
                        events.ScheduleEvent(EVENT_DRAIN_ESSENCE, 1s, 0, PHASE_OUT_OF_COMBAT);
                        events.ScheduleEvent(EVENT_EVOLUTION, 19s + 500ms, 0, PHASE_OUT_OF_COMBAT);
                    }
                    break;
                case EVENT_DRAIN_ESSENCE:
                    DoCastSelf(SPELL_DRAIN_ESSENCE);
                    if (++_drainCastsInPair < 2)
                        events.ScheduleEvent(EVENT_DRAIN_ESSENCE, 1s, 0, PHASE_OUT_OF_COMBAT);
                    break;
                case EVENT_DARK_COMMAND:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, NonTankTargetSelector(me)))
                        DoCast(target, SPELL_DARK_COMMAND);
                    events.Repeat(24s);
                    break;
                default:
                    break;
            }
        }
    }

private:
    // The zealots are world spawns, so they are looked up by guid once and then followed
    // through the entry swap that Evolution does to them.
    std::vector<Creature*> GetZealots()
    {
        std::vector<Creature*> zealots;

        if (_zealotGuids.empty())
        {
            std::list<Creature*> found;
            GetCreatureListWithEntryInGrid(found, me, NPC_TWILIGHT_ZEALOT, float(ZEALOT_SEARCH_RANGE));
            for (Creature* zealot : found)
                _zealotGuids.push_back(zealot->GetGUID());
        }

        for (ObjectGuid const& guid : _zealotGuids)
            if (Creature* zealot = ObjectAccessor::GetCreature(*me, guid))
                zealots.push_back(zealot);

        return zealots;
    }

    void ClearAuras()
    {
        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_DARK_COMMAND);
        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_EVOLUTION_STACKS);
        instance->DoRemoveAurasDueToSpellOnPlayers(SPELL_TWILIGHT_EVOLUTION);
    }

    GuidVector _zealotGuids;
    uint8 _slainEvolvedZealots;
    uint8 _evolutionCastsInPair;
    uint8 _drainCastsInPair;
};

// 50284 - Twilight Zealot (turns into 39987 - Evolved Twilight Zealot)
struct npc_corla_twilight_zealot : public ScriptedAI
{
    npc_corla_twilight_zealot(Creature* creature) : ScriptedAI(creature), _instance(creature->GetInstanceScript()), _evolved(false) { }

    void Reset() override
    {
        _events.Reset();

        // An evolved zealot is despawned and respawned as 50284 by ACTION_RESET_ZEALOT;
        // until that happens it must not be put back on its knees.
        if (me->GetEntry() != NPC_TWILIGHT_ZEALOT)
            return;

        _evolved = false;
        me->SetReactState(REACT_PASSIVE);
        DoCastSelf(SPELL_KNEELING_SUPPLICATION);
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        if (!_instance)
            return;

        if (Creature* corla = _instance->GetCreature(DATA_CORLA))
            if (!corla->IsInCombat())
                corla->AI()->DoZoneInCombat();
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (!_evolved || !_instance)
            return;

        if (Creature* corla = _instance->GetCreature(DATA_CORLA))
            corla->AI()->DoAction(ACTION_EVOLVED_ZEALOT_SLAIN);
    }

    void DoAction(int32 action) override
    {
        switch (action)
        {
            case ACTION_START_EVOLUTION:
                DoCastSelf(SPELL_NETHER_BEAM_PERIODIC);
                break;
            case ACTION_EVOLVE:
                _evolved = true;
                me->RemoveAllAuras();
                me->SetReactState(REACT_AGGRESSIVE);
                me->UpdateEntry(NPC_EVOLVED_TWILIGHT_ZEALOT);
                DoZoneInCombat();
                _events.ScheduleEvent(EVENT_GRIEVOUS_WHIRL, 1ms);
                _events.ScheduleEvent(EVENT_SHADOW_STRIKE, 50ms);
                _events.ScheduleEvent(EVENT_INVISIBILITY_AND_STEALTH_DETECTION, 2s);
                _events.ScheduleEvent(EVENT_FORCE_BLAST, 2s);
                _events.ScheduleEvent(EVENT_GRAVITY_STRIKE, 3s);
                break;
            case ACTION_RESET_ZEALOT:
                _events.Reset();
                // An evolved zealot carries entry 39987; despawn it so the world spawn
                // comes back as 50284 and Reset() puts it on its knees again.
                if (_evolved)
                {
                    _evolved = false;
                    me->DespawnOrUnsummon(0s, 10s);
                }
                break;
            default:
                break;
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;

        _events.Update(diff);

        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_GRIEVOUS_WHIRL:
                    DoCastAOE(SPELL_GRIEVOUS_WHIRL);
                    _events.Repeat(16s, 19s);
                    break;
                case EVENT_SHADOW_STRIKE:
                    DoCastVictim(SPELL_SHADOW_STRIKE);
                    _events.Repeat(14s, 17s);
                    break;
                case EVENT_INVISIBILITY_AND_STEALTH_DETECTION:
                    DoCastAOE(SPELL_INVISIBILITY_AND_STEALTH_DETECTION, true);
                    break;
                case EVENT_FORCE_BLAST:
                    if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0, 100.0f, true))
                        DoCast(target, SPELL_FORCE_BLAST);
                    _events.Repeat(14s, 17s);
                    break;
                case EVENT_GRAVITY_STRIKE:
                    DoCastVictim(SPELL_GRAVITY_STRIKE);
                    _events.Repeat(10s, 11s);
                    break;
                default:
                    break;
            }
        }
    }

private:
    EventMap _events;
    InstanceScript* _instance;
    bool _evolved;
};

// 75676 - Nether Beam
class spell_corla_nether_beam : public SpellScript
{
    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_NETHER_BEAM_VISUAL, SPELL_EVOLUTION_STACKS });
    }

    // A player standing in the cone between the zealot and the drake takes the stack instead.
    void FilterTargets(std::list<WorldObject*>& targets)
    {
        if (targets.size() > 1)
        {
            if (Unit* caster = GetCaster())
            {
                targets.sort(Trinity::ObjectDistanceOrderPred(caster, true));
                targets.resize(1);
            }
        }
    }

    void HandleScriptEffect(SpellEffIndex /*effIndex*/)
    {
        ApplyEvolutionStack(GetHitUnit());
    }

    void HandleAfterCast()
    {
        // Nobody blocked the beam: the zealot keeps its own stack.
        if (!_stolen)
            ApplyEvolutionStack(GetCaster());
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_corla_nether_beam::FilterTargets, EFFECT_0, TARGET_UNIT_CONE_ENTRY);
        OnEffectHitTarget += SpellEffectFn(spell_corla_nether_beam::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        AfterCast += SpellCastFn(spell_corla_nether_beam::HandleAfterCast);
    }

private:
    void ApplyEvolutionStack(Unit* target)
    {
        if (!target)
            return;

        _stolen = true;
        target->CastSpell(target, SPELL_NETHER_BEAM_VISUAL, true);
        target->CastSpell(target, SPELL_EVOLUTION_STACKS, true);

        if (Aura const* aura = target->GetAura(SPELL_EVOLUTION_STACKS))
            if (aura->GetStackAmount() >= EVOLUTION_WARNING_STACKS)
                target->SendPlaySpellVisualKit(SPELL_VISUAL_KIT_EVOLUTION_WARNING, 0, 0);
    }

    bool _stolen = false;
};

// 75697 - Evolution
class spell_corla_evolution : public SpellScript
{
    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_TWILIGHT_EVOLUTION });
    }

    void HandleEvolve()
    {
        Unit* target = GetHitUnit();
        if (!target)
            return;

        Aura* aura = target->GetAura(GetSpellInfo()->Id);
        if (!aura || aura->GetStackAmount() < GetSpellInfo()->StackAmount)
            return;

        InstanceScript* instance = target->GetInstanceScript();
        if (!instance)
            return;

        if (Creature* corla = instance->GetCreature(DATA_CORLA))
        {
            corla->AI()->Talk(SAY_EVOLUTION_COMPLETE);

            if (Creature* creature = target->ToCreature())
            {
                if (creature->IsAIEnabled())
                {
                    corla->AI()->Talk(SAY_ANNOUNCE_ZEALOT_EVOLVED);
                    creature->AI()->DoAction(ACTION_EVOLVE);
                }
            }
            else if (target->IsPlayer())
                corla->CastSpell(target, SPELL_TWILIGHT_EVOLUTION, true);
        }

        aura->Remove();
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_corla_evolution::HandleEvolve);
    }
};

// 76524 - Grievous Whirl
class spell_corla_grievous_whirl : public AuraScript
{
    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        if (GetTarget()->GetHealth() == GetTarget()->GetMaxHealth())
            Remove();
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_corla_grievous_whirl::HandleProc, EFFECT_1, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

// Achievement 5282 - Arrested Development (Criteria 15944)
class achievement_arrested_development : public AchievementCriteriaScript
{
public:
    achievement_arrested_development() : AchievementCriteriaScript("achievement_arrested_development") { }

    bool OnCheck(Player* /*source*/, Unit* target) override
    {
        if (!target)
            return false;

        if (target->IsAIEnabled())
            return target->GetAI()->GetData(DATA_ARRESTED_DEVELOPMENT) != 0;

        return false;
    }
};

void AddSC_boss_corla()
{
    RegisterBlackrockCavernsCreatureAI(boss_corla);
    RegisterBlackrockCavernsCreatureAI(npc_corla_twilight_zealot);
    RegisterSpellScript(spell_corla_nether_beam);
    RegisterSpellScript(spell_corla_evolution);
    RegisterSpellScript(spell_corla_grievous_whirl);
    new achievement_arrested_development();
}

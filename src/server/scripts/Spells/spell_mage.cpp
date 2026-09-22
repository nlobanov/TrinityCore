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
 * Scripts for spells with SPELLFAMILY_MAGE and SPELLFAMILY_GENERIC spells used by mage players.
 * Ordered alphabetically using scriptname.
 * Scriptnames of files in this file should be prefixed with "spell_mage_".
 */

#include "ScriptMgr.h"
#include "Creature.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

namespace Scripts::Spells::Mage
{
    enum MageSpells
    {
        SPELL_MAGE_ARCANE_MISSILES              = 5143,
        SPELL_MAGE_ARCANE_MISSILES_AURASTATE    = 79808,
        SPELL_MAGE_IGNITE_DOT                   = 12654,
        SPELL_MAGE_LIVING_BOMB_EXPLOSION        = 44461
    };

    // 71761 - Deep Freeze Immunity State
    class spell_mage_deep_freeze_immunity_state : public AuraScript
    {
        bool CheckEffectProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
        {
            // Though the tooltip states that any target that is permanently stun immune can take damage, only creatures can actually achieve such a state
            // So we save ourselves some checks by filtering for creatures first
            if (!eventInfo.GetActionTarget() || !eventInfo.GetActionTarget()->IsCreature())
                return false;

            // We check for permanent immunities first by checking for a DB set immunities value.
            // If that check passes, we will check for the creature's current stun immunity to determine if we may proc or not.
            if (CreatureImmunities const* immunities = SpellMgr::GetCreatureImmunities(eventInfo.GetActionTarget()->ToCreature()->GetCreatureTemplate()->CreatureImmunitiesId))
                if (immunities->Mechanic[MECHANIC_STUN])
                    if ((eventInfo.GetActionTarget()->GetMechanicImmunityMask() & (1 << MECHANIC_STUN)) != 0)
                        return true;

            return false;
        }

        void Register() override
        {
            DoCheckEffectProc += AuraCheckEffectProcFn(spell_mage_deep_freeze_immunity_state::CheckEffectProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        }
    };

    // 79684 - Offensive State (DND)
    class spell_mage_offensive_state_dnd : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_MAGE_ARCANE_MISSILES });
        }

        bool CheckEffectProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
        {
            // Only allow Arcane Missiles! to proc when the player has learned the according spell
            if (!GetTarget()->HasSpell(SPELL_MAGE_ARCANE_MISSILES))
                return false;

            // Arcane Missiles cannot trigger itself
            if ((eventInfo.GetSpellInfo()->SpellFamilyFlags[0] & (0x800 | 0x200000)) != 0)
                return false;

            return roll_chance(aurEff->GetAmount());
        }

        void Register() override
        {
            DoCheckEffectProc += AuraCheckEffectProcFn(spell_mage_offensive_state_dnd::CheckEffectProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        }
    };

    // 79683 - Arcane Missiles!
    class spell_mage_arcane_missiles : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_MAGE_ARCANE_MISSILES_AURASTATE });
        }

        void AfterApply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
        {
            GetTarget()->CastSpell(nullptr, SPELL_MAGE_ARCANE_MISSILES_AURASTATE, aurEff);
        }

        void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            GetTarget()->RemoveAurasDueToSpell(SPELL_MAGE_ARCANE_MISSILES_AURASTATE, GetCasterGUID());
        }

        void Register() override
        {
            AfterEffectApply += AuraEffectApplyFn(spell_mage_arcane_missiles::AfterApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
            AfterEffectRemove += AuraEffectApplyFn(spell_mage_arcane_missiles::AfterRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // -11119 - Ignite (ranks 11119, 11120, 12846)
    // The talent is a SPELL_AURA_DUMMY with ProcTypeMask PROC_FLAG_DEAL_HARMFUL_SPELL and no explicit `spell_proc` row,
    // so SpellMgr::LoadSpellProcs generates a default entry that procs on every landed magic hit: the script has to do
    // the crit / school / source filtering itself.
    //
    // Cataclysm rolling rule (WoWSims cata sim/common/cata/other_effects.go RegisterIgniteEffect,
    // mop-classic-bugs #1033 "refreshed Ignite must tick 3 times" and #1103 "the bank is spread evenly"):
    //   bank        = pct of the crit damage + damage still owed by a running 12654 from this caster
    //   tick count  = 2 on a fresh application, 3 on a refresh
    //   per tick    = bank / tick count, and the DoT duration is set to tick count x period
    //
    // The script banks the raw percentage without mastery: Flashburn (76595, SPELL_AURA_ADD_PCT_MODIFIER,
    // SpellModOp::PeriodicHealingAndDamage, class mask 0x8400000) matches 12654 (family 3, mask 0x8000000), so the core
    // multiplies every tick by mastery in Unit::SpellDamageBonusDone. 12654 carries SPELL_ATTR6_IGNORE_CASTER_DAMAGE_MODIFIERS,
    // so Unit::SpellDamagePctDone returns 1.0 for it and no other done-percent modifier is counted twice.
    // Ported to 4.4.2 for the fork; TDB 442 has no spell_script_names row for it (wow-data sql/custom/0008).
    class spell_mage_ignite : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_MAGE_IGNITE_DOT });
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            if (!eventInfo.GetActionTarget())
                return false;

            DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
            if (!damageInfo || !damageInfo->GetDamage())
                return false;

            // only critical strikes ignite
            if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
                return false;

            // "your critical strikes from non-periodic Fire damage spells" - periodic ticks never ignite
            if (eventInfo.GetTypeMask() & (PROC_FLAG_DEAL_HARMFUL_PERIODIC | PROC_FLAG_DEAL_HELPFUL_PERIODIC))
                return false;

            if (!(eventInfo.GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE))
                return false;

            SpellInfo const* procSpell = eventInfo.GetSpellInfo();
            if (!procSpell)
                return false;

            // Ignite does not ignite itself, and the Living Bomb explosion does not either (mop-classic-bugs #1206)
            if (procSpell->Id == SPELL_MAGE_IGNITE_DOT || procSpell->Id == SPELL_MAGE_LIVING_BOMB_EXPLOSION)
                return false;

            return true;
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();

            Unit* caster = GetTarget();
            Unit* target = eventInfo.GetActionTarget();

            SpellInfo const* igniteInfo = sSpellMgr->AssertSpellInfo(SPELL_MAGE_IGNITE_DOT, GetCastDifficulty());
            int32 period = int32(igniteInfo->GetEffect(EFFECT_0).ApplyAuraPeriod);
            if (period <= 0)
                return;

            int32 ticks = igniteInfo->GetDuration() / period;
            if (ticks <= 0)
                return;

            double bank = CalculatePct(double(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmount());

            // rolling Ignite: what the running DoT still owes is added to the bank and the total is spread over one extra tick
            if (AuraEffect const* running = target->GetAuraEffect(SPELL_MAGE_IGNITE_DOT, EFFECT_0, caster->GetGUID()))
            {
                bank += double(running->GetRemainingTicks()) * running->GetAmount();
                ++ticks;
            }

            double amount = bank / double(ticks);
            if (amount <= 0.0)
                return;

            CastSpellExtraArgs args(aurEff);
            args.AddSpellMod(SPELLVALUE_BASE_POINT0, amount);
            args.AddSpellMod(SPELLVALUE_DURATION, ticks * period);
            caster->CastSpell(target, SPELL_MAGE_IGNITE_DOT, args);
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_mage_ignite::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_mage_ignite::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 44457 - Living Bomb: the explosion (EFFECT_1 dummy value, 44461) fires when the aura runs out, not when it is dispelled or the target dies.
    // Ported from TrinityCore 4.3.4 (Firelands-Core snapshot, GPL-2.0); TDB 442 has no spell_script_names row for it (wow-data sql/custom/0007).
    class spell_mage_living_bomb : public AuraScript
    {
        bool Validate(SpellInfo const* spellInfo) override
        {
            return ValidateSpellInfo({ uint32(spellInfo->GetEffect(EFFECT_1).CalcValue()) });
        }

        void AfterRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
        {
            if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
                return;

            if (Unit* caster = GetCaster())
                caster->CastSpell(GetTarget(), uint32(aurEff->GetAmount()), aurEff);
        }

        void Register() override
        {
            AfterEffectRemove += AuraEffectRemoveFn(spell_mage_living_bomb::AfterRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        }
    };
}

void AddSC_mage_spell_scripts()
{
    using namespace Scripts::Spells::Mage;
    RegisterSpellScript(spell_mage_ignite);
    RegisterSpellScript(spell_mage_living_bomb);
    RegisterSpellScript(spell_mage_deep_freeze_immunity_state);
    RegisterSpellScript(spell_mage_offensive_state_dnd);
    RegisterSpellScript(spell_mage_arcane_missiles);
}

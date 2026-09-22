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
 * Scripts for spells with SPELLFAMILY_WARLOCK and SPELLFAMILY_GENERIC spells used by warlock players.
 * Ordered alphabetically using scriptname.
 * Scriptnames of files in this file should be prefixed with "spell_warl_".
 *
 * ---------------------------------------------------------------------------------------------------
 * Provenance
 * ---------------------------------------------------------------------------------------------------
 * Ported from Firelands-Core (TCPP lineage, 4.3.4), scripts/Spells/spell_warlock.cpp, GPL-2.0,
 * snapshot in wow-kb src/Firelands-Core. Three scripts come from ShatterCore (same lineage, newer
 * TrinityCore API, GPL-2.0): spell_warl_bane_of_havoc, spell_warl_bane_of_havoc_tracking_aura,
 * spell_warl_shadowflame. Every id below was checked against DB2 4.4.2.60895 with
 * `tools/kb/kb.py spell <id>`; bindings live in wow-data sql/custom/0013.
 *
 * Ids dropped or replaced for 4.4.2:
 *  - 62388 Demonic Circle: Allow Cast does not exist in 4.4.2. fl-core used it to gate the teleport
 *    button from the summon aura's periodic tick. Replaced by a CheckCast on 48020 that measures the
 *    distance to the circle directly, and the periodic dummy handler of 48018 was dropped with it.
 *  - Icon lookups (Unit::GetDummyAuraEffect + SpellIconID) do not exist in this core. Replaced by
 *    explicit family-mask lookups: Fire and Brimstone (icon 3173 -> family 5 mask 0x4 on EFFECT_0,
 *    spells 47266-47270), Improved Life Tap (icon 208 -> family 5 mask 0x40000, spells 18182/18183),
 *    Jinx (icon 5002 -> spells 18179/85479 EFFECT_1).
 *  - Mana Feed (icon 1982, 32553 Life Tap energize) dropped from spell_warl_life_tap. In 4.4.2 Mana
 *    Feed is 85174/85175 and restores the warlock's mana from pet crits, not from Life Tap.
 *  - Death's Embrace (icon 3223) dropped from spell_warl_drain_life. In 4.4.2 Death's Embrace
 *    (47198-47200) is +4%/rank shadow damage below 25% health and 89653 Drain Life (heal) is a single
 *    SPELL_EFFECT_HEAL_PCT effect of 2%; fl-core read its EFFECT_2, which does not exist in 4.4.2.
 *  - spell_warl_chaos_bolt dropped: it only added the Fire and Brimstone bonus through
 *    AURA_STATE_CONFLAGRATE, an aura state this core no longer has, and 50796 has no other script
 *    work in 4.4.2. The Conflagrate crit half of Fire and Brimstone is a real ADD_FLAT_MODIFIER
 *    (CritChance) effect in 4.4.2 and needs no script.
 *  - Soulburn: Seed of Corruption (86664) is not implemented. fl-core routed it through
 *    Player::SetLastSoulburnSpell / GetLastSoulburnSpell, a core extension this fork does not have.
 *    The other three Soulburn variants (79437 Healthstone, 79438 Demonic Circle, 79440 Searing Pain)
 *    are ported; the Drain Life and summon parts of 74434 are real aura effects in 4.4.2
 *    (EFFECT_0 ADD_PCT_MODIFIER, EFFECT_1 OVERRIDE_ACTIONBAR_SPELLS -> 89420) and need no script.
 *  - SpellInfo::GetRank() returns 1 for every talent in 4.4.2: SkillLineAbility.SupercedesSpell is 0
 *    for all of them, so SpellMgr::LoadSpellRanks builds no chain. Two fl-core scripts keyed off the
 *    rank and were rewritten to switch on the spell id instead (Burning Embers 91986/85112, Jinx
 *    18179/85479). For the same reason sql/custom/0013 lists every rank explicitly instead of using
 *    the negative spell_script_names ids of TDB 434.
 *
 * Not portable from this file (recorded, not implemented here):
 *  - Pandemic carry-over on channelled spells, Drain Soul / Drain Life / Hellfire / Rain of Fire
 *    (class-changes-442 #1209, #1089). This is the core's DoT refresh path
 *    (Aura::RefreshDuration / AuraEffect::CalculatePeriodic), not a spell script; it needs its own
 *    ticket against Spells/Auras.
 *  - Mastery floor for Affliction and Demonology (#1256) is already satisfied: the int truncation in
 *    SpellAuraEffects.cpp:751 is kept, as decided in the W0 plan for 0008.
 *  - 713 Summon Incubus, 85767 Dark Intent, 71521 Hand of Gul'dan: out of scope for this ticket
 *    (Incubus needs pet data; Dark Intent and Hand of Gul'dan only exist in Molten and Starfall).
 */

#include "ScriptMgr.h"
#include "Creature.h"
#include "GameObject.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "ThreatManager.h"

namespace Scripts::Spells::Warlock
{
    enum WarlockSpells
    {
        SPELL_WARLOCK_AFTERMATH_STUN                    = 85387,
        SPELL_WARLOCK_BANE_OF_DOOM_EFFECT               = 18662,
        SPELL_WARLOCK_BANE_OF_HAVOC                     = 80240,
        SPELL_WARLOCK_BANE_OF_HAVOC_TRACKING_AURA       = 85466,
        SPELL_WARLOCK_BANE_OF_HAVOC_DAMAGE              = 85455,
        SPELL_WARLOCK_BURNING_EMBERS_DAMAGE             = 85421,
        SPELL_WARLOCK_BURNING_EMBERS_R1                 = 91986,
        SPELL_WARLOCK_BURNING_EMBERS_R2                 = 85112,
        SPELL_WARLOCK_CREATE_HEALTHSTONE                = 34130,
        SPELL_WARLOCK_CORRUPTION_TRIGGERED              = 87389,
        SPELL_WARLOCK_DEMONIC_CIRCLE_SUMMON             = 48018,
        SPELL_WARLOCK_DEMONIC_CIRCLE_TELEPORT           = 48020,
        SPELL_WARLOCK_DEMONIC_EMPOWERMENT_FELGUARD      = 54508,
        SPELL_WARLOCK_DEMONIC_EMPOWERMENT_FELHUNTER     = 54509,
        SPELL_WARLOCK_DEMONIC_EMPOWERMENT_IMP           = 54444,
        SPELL_WARLOCK_DEMONIC_EMPOWERMENT_SUCCUBUS      = 54435,
        SPELL_WARLOCK_DEMONIC_EMPOWERMENT_VOIDWALKER    = 54443,
        SPELL_WARLOCK_DEMON_SOUL_IMP                    = 79459,
        SPELL_WARLOCK_DEMON_SOUL_FELHUNTER              = 79460,
        SPELL_WARLOCK_DEMON_SOUL_FELGUARD               = 79462,
        SPELL_WARLOCK_DEMON_SOUL_SUCCUBUS               = 79463,
        SPELL_WARLOCK_DEMON_SOUL_VOIDWALKER             = 79464,
        SPELL_WARLOCK_DRAIN_LIFE_HEAL                   = 89653,
        SPELL_WARLOCK_FEL_ARMOR_HEAL                    = 96379,
        SPELL_WARLOCK_FEL_SYNERGY_HEAL                  = 54181,
        SPELL_WARLOCK_GLYPH_OF_SHADOWFLAME              = 63311,
        SPELL_WARLOCK_GLYPH_OF_SOUL_SWAP                = 56226,
        SPELL_WARLOCK_GLYPH_OF_SUCCUBUS                 = 56250,
        SPELL_WARLOCK_HAUNT_HEAL                        = 48210,
        SPELL_WARLOCK_HEALTHSTONE                       = 6262,
        SPELL_WARLOCK_IMMOLATE                          = 348,
        SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R1    = 60955,
        SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R2    = 60956,
        SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_R1         = 18703,
        SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_R2         = 18704,
        SPELL_WARLOCK_IMPROVED_SOUL_FIRE_PCT            = 85383,
        SPELL_WARLOCK_IMPROVED_SOUL_FIRE_STATE          = 85385,
        SPELL_WARLOCK_JINX_AOE_R1                       = 85547,
        SPELL_WARLOCK_JINX_AOE_R2                       = 86105,
        SPELL_WARLOCK_JINX_TRIGGERED_RAGE               = 85539,
        SPELL_WARLOCK_JINX_TRIGGERED_ENERGY             = 85540,
        SPELL_WARLOCK_JINX_TRIGGERED_RUNIC_POWER        = 85541,
        SPELL_WARLOCK_JINX_TRIGGERED_FOCUS              = 85542,
        SPELL_WARLOCK_JINX_R1                           = 18179,
        SPELL_WARLOCK_JINX_R2                           = 85479,
        SPELL_WARLOCK_LIFE_TAP_ENERGIZE                 = 31818,
        SPELL_WARLOCK_NETHER_WARD                       = 91711,
        SPELL_WARLOCK_NETHER_TALENT                     = 91713,
        SPELL_WARLOCK_PANDEMIC_SCRIPT_EFFECT            = 92931,
        SPELL_WARLOCK_RAIN_OF_FIRE                      = 42223,
        SPELL_WARLOCK_SHADOW_TRANCE                     = 17941,
        SPELL_WARLOCK_SEARING_PAIN                      = 5676,
        SPELL_WARLOCK_SEED_OF_CORRUPTION                = 27243,
        SPELL_WARLOCK_SEED_OF_CORRUPTION_TRIGGERED      = 87385,
        SPELL_WARLOCK_SEED_OF_CORRUPTION_VISUAL         = 37826,
        SPELL_WARLOCK_SHADOW_WARD                       = 6229,
        SPELL_WARLOCK_SHADOWFLAME_PERIODIC              = 47960,
        SPELL_WARLOCK_SOULBURN_HEALTHSTONE              = 79437,
        SPELL_WARLOCK_SOULBURN_DEMONIC_CIRCLE           = 79438,
        SPELL_WARLOCK_SOULBURN_SEARING_PAIN             = 79440,
        SPELL_WARLOCK_SOUL_HARVEST_ENERGIZE             = 101977,
        SPELL_WARLOCK_SOUL_SHARD                        = 87388,
        SPELL_WARLOCK_SOUL_SHARD_ENERGIZE               = 95810,
        SPELL_WARLOCK_SOULSHATTER_EFFECT                = 32835,
        SPELL_WARLOCK_SOUL_SWAP_CD_MARKER               = 94229,
        SPELL_WARLOCK_SOUL_SWAP_OVERRIDE                = 86211,
        SPELL_WARLOCK_SOUL_SWAP_MOD_COST                = 92794,
        SPELL_WARLOCK_SOUL_SWAP_DOT_MARKER              = 92795,
        SPELL_WARLOCK_UNSTABLE_AFFLICTION               = 30108,
        SPELL_WARLOCK_UNSTABLE_AFFLICTION_DISPEL        = 31117
    };

    enum MiscSpells
    {
        SPELL_GEN_REPLENISHMENT                         = 57669,
        SPELL_PRIEST_SHADOW_WORD_DEATH                  = 32409
    };

    // Fire and Brimstone (47266-47270) EFFECT_0: SPELL_AURA_DUMMY, family 5, class mask 0x4.
    // Replaces fl-core's GetDummyAuraEffect(SPELLFAMILY_WARLOCK, 3173, EFFECT_0).
    static constexpr flag128 FireAndBrimstoneMask = flag128(0x4, 0, 0, 0);
    // Improved Life Tap (18182/18183) EFFECT_0: SPELL_AURA_DUMMY, family 5, class mask 0x40000.
    static constexpr flag128 ImprovedLifeTapMask = flag128(0x40000, 0, 0, 0);

    // -85113 - Aftermath (85113, 85114)
    // EFFECT_0 is SPELL_AURA_PROC_TRIGGER_SPELL -> 18118 (the Conflagrate daze), which the core casts.
    // The Rain of Fire stun (85387) has no effect of its own, so the script adds it.
    class spell_warl_aftermath : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_AFTERMATH_STUN, SPELL_WARLOCK_RAIN_OF_FIRE });
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            if (!eventInfo.GetSpellInfo() || eventInfo.GetSpellInfo()->Id != SPELL_WARLOCK_RAIN_OF_FIRE)
                return;

            PreventDefaultAction();
            if (eventInfo.GetActionTarget() && roll_chance(aurEff->GetAmountAsInt()))
                GetTarget()->CastSpell(eventInfo.GetActionTarget(), SPELL_WARLOCK_AFTERMATH_STUN, aurEff);
        }

        void Register() override
        {
            OnEffectProc += AuraEffectProcFn(spell_warl_aftermath::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        }
    };

    // 603 - Bane of Doom
    class spell_warl_bane_of_doom : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_BANE_OF_DOOM_EFFECT });
        }

        bool Load() override
        {
            return GetCaster() && GetCaster()->IsPlayer();
        }

        void OnRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
        {
            Unit* caster = GetCaster();
            if (!caster)
                return;

            if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
                return;

            if (caster->ToPlayer()->isHonorOrXPTarget(GetTarget()))
                caster->CastSpell(GetTarget(), SPELL_WARLOCK_BANE_OF_DOOM_EFFECT, aurEff);
        }

        void Register() override
        {
            AfterEffectRemove += AuraEffectRemoveFn(spell_warl_bane_of_doom::OnRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // 80240 - Bane of Havoc (ShatterCore)
    class spell_warl_bane_of_havoc : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_BANE_OF_HAVOC_TRACKING_AURA });
        }

        void ApplyTrackingAura(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            if (Unit* caster = GetCaster())
                caster->CastSpell(nullptr, SPELL_WARLOCK_BANE_OF_HAVOC_TRACKING_AURA, CastSpellExtraArgs(TRIGGERED_FULL_MASK));
        }

        void Register() override
        {
            AfterEffectApply += AuraEffectApplyFn(spell_warl_bane_of_havoc::ApplyTrackingAura, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // 85466 - Bane of Havoc, the tracking aura on the warlock (ShatterCore)
    class spell_warl_bane_of_havoc_tracking_aura : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_BANE_OF_HAVOC_DAMAGE, SPELL_WARLOCK_BANE_OF_HAVOC });
        }

        void StoreTrackingAuraTargetGuid(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            for (Aura const* aura : GetTarget()->GetSingleCastAuras())
                if (aura->GetId() == SPELL_WARLOCK_BANE_OF_HAVOC && aura->GetCasterGUID() == GetTarget()->GetGUID())
                    if (Unit const* owner = aura->GetUnitOwner())
                        _trackedTargetGUID = owner->GetGUID();
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            // Nothing to track
            if (_trackedTargetGUID.IsEmpty() || !eventInfo.GetActionTarget() || eventInfo.GetActionTarget()->GetGUID() == _trackedTargetGUID)
                return false;

            // No damage to share
            if (!eventInfo.GetDamageInfo() || eventInfo.GetDamageInfo()->GetDamage() == 0)
                return false;

            if (!PrepareProc(eventInfo))
            {
                _trackedTargetGUID = ObjectGuid::Empty;
                _trackedTarget = nullptr;
                _trackedDamage = 0;
                return false;
            }

            return true;
        }

        void HandleProc(AuraEffect* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
        {
            GetTarget()->CastSpell(_trackedTarget, SPELL_WARLOCK_BANE_OF_HAVOC_DAMAGE, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(_trackedDamage));
        }

        void Register() override
        {
            AfterEffectApply += AuraEffectApplyFn(spell_warl_bane_of_havoc_tracking_aura::StoreTrackingAuraTargetGuid, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
            DoCheckProc += AuraCheckProcFn(spell_warl_bane_of_havoc_tracking_aura::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_warl_bane_of_havoc_tracking_aura::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        }

    private:
        ObjectGuid _trackedTargetGUID;
        Unit* _trackedTarget = nullptr;
        int32 _trackedDamage = 0;

        bool PrepareProc(ProcEventInfo const& eventInfo)
        {
            _trackedTarget = ObjectAccessor::GetUnit(*GetTarget(), _trackedTargetGUID);
            if (!_trackedTarget)
                return false;

            AuraEffect const* baneEffect = _trackedTarget->GetAuraEffect(SPELL_WARLOCK_BANE_OF_HAVOC, EFFECT_0, GetCasterGUID());
            if (!baneEffect)
                return false;

            _trackedDamage = CalculatePct(int32(eventInfo.GetDamageInfo()->GetDamage()), baneEffect->GetAmountAsInt());
            return true;
        }
    };

    // 710 - Banish
    class spell_warl_banish : public SpellScript
    {
        void HandleBanish(SpellMissInfo /*missInfo*/)
        {
            // Casting Banish on a banished target cancels the effect instead of reapplying it.
            if (Unit* target = GetHitUnit())
            {
                if (target->GetAuraEffect(SPELL_AURA_SCHOOL_IMMUNITY, SPELLFAMILY_WARLOCK, flag128(0, 0x08000000, 0, 0)))
                {
                    // No need to remove the old aura, it does not stack with the one this cast would apply
                    PreventHitDefaultEffect(EFFECT_0);
                    PreventHitDefaultEffect(EFFECT_1);
                    PreventHitDefaultEffect(EFFECT_2);
                    _removed = true;
                }
            }
        }

        void RemoveAura()
        {
            if (_removed)
                PreventHitAura();
        }

        void Register() override
        {
            BeforeHit += BeforeSpellHitFn(spell_warl_banish::HandleBanish);
            AfterHit += SpellHitFn(spell_warl_banish::RemoveAura);
        }

    private:
        bool _removed = false;
    };

    // 91986, 85112 - Burning Embers
    // WoWSims cata sim/warlock/talents_destruction.go registerBurningEmbers: 7 ticks of 1 s,
    // per tick = min(damage * 0.25 * rank / 7, (scaling(0.0735 * rank) + 0.7 * rank * SP) / 7).
    // The flat half of the cap is EFFECT_1 of the talent, which the core scales; the SP half is the
    // 0.7 per rank. GetRank() is unusable here (see the file header), so the rank comes from the id.
    class spell_warl_burning_embers : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_BURNING_EMBERS_DAMAGE });
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            return eventInfo.GetDamageInfo() && eventInfo.GetActionTarget();
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();
            Unit* target = GetTarget();

            SpellInfo const* emberInfo = sSpellMgr->AssertSpellInfo(SPELL_WARLOCK_BURNING_EMBERS_DAMAGE, GetCastDifficulty());
            int32 period = int32(emberInfo->GetEffect(EFFECT_0).ApplyAuraPeriod);
            if (period <= 0)
                return;

            int32 maxTicks = emberInfo->GetDuration() / period;
            if (maxTicks <= 0)
                return;

            int32 damageBp = CalculatePct(int32(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmountAsInt()) / maxTicks;

            float coefficient = GetId() == SPELL_WARLOCK_BURNING_EMBERS_R2 ? 1.4f : 0.7f;
            int32 damageCap = int32(target->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE) * coefficient
                + GetEffectInfo(EFFECT_1).CalcValue(target)) / maxTicks;

            target->CastSpell(eventInfo.GetActionTarget(), SPELL_WARLOCK_BURNING_EMBERS_DAMAGE,
                CastSpellExtraArgs(aurEff).AddSpellBP0(std::min(damageBp, damageCap)));
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_warl_burning_embers::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_warl_burning_embers::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 17962 - Conflagrate
    // EFFECT_0 of 17962 has no coefficient in 4.4.2: the damage is EFFECT_1 percent (60) of what the
    // caster's Immolate (348 EFFECT_2) deals over its whole duration.
    class spell_warl_conflagrate : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_IMMOLATE });
        }

        void HandleHit(SpellEffIndex /*effIndex*/)
        {
            if (AuraEffect const* aurEff = GetHitUnit()->GetAuraEffect(SPELL_WARLOCK_IMMOLATE, EFFECT_2, GetCaster()->GetGUID()))
            {
                int32 damage = aurEff->GetAmountAsInt() * int32(aurEff->GetTotalTicks());
                SetEffectValue(CalculatePct(damage, GetEffectInfo(EFFECT_1).CalcValueAsInt(GetCaster())));
            }
        }

        void Register() override
        {
            OnEffectLaunchTarget += SpellEffectFn(spell_warl_conflagrate::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        }
    };

    // 6201 - Create Healthstone
    class spell_warl_create_healthstone : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_CREATE_HEALTHSTONE });
        }

        bool Load() override
        {
            return GetCaster()->IsPlayer();
        }

        void HandleScriptEffect(SpellEffIndex /*effIndex*/)
        {
            GetCaster()->CastSpell(GetCaster(), SPELL_WARLOCK_CREATE_HEALTHSTONE, true);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_create_healthstone::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    // 702 - Curse of Weakness
    // Jinx EFFECT_1 adds a resource-cost debuff picked by the victim's class.
    class spell_warl_curse_of_weakness : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo(
                {
                    SPELL_WARLOCK_JINX_R1,
                    SPELL_WARLOCK_JINX_R2,
                    SPELL_WARLOCK_JINX_TRIGGERED_ENERGY,
                    SPELL_WARLOCK_JINX_TRIGGERED_FOCUS,
                    SPELL_WARLOCK_JINX_TRIGGERED_RAGE,
                    SPELL_WARLOCK_JINX_TRIGGERED_RUNIC_POWER
                });
        }

        void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            Unit* caster = GetCaster();
            if (!caster)
                return;

            AuraEffect const* jinx = caster->GetAuraEffect(SPELL_WARLOCK_JINX_R1, EFFECT_1);
            if (!jinx)
                jinx = caster->GetAuraEffect(SPELL_WARLOCK_JINX_R2, EFFECT_1);

            if (!jinx)
                return;

            Player* target = GetTarget()->ToPlayer();
            if (!target)
                return;

            switch (target->GetClass())
            {
                case CLASS_WARRIOR:
                    _debuffSpellId = SPELL_WARLOCK_JINX_TRIGGERED_RAGE;
                    break;
                case CLASS_ROGUE:
                    _debuffSpellId = SPELL_WARLOCK_JINX_TRIGGERED_ENERGY;
                    break;
                case CLASS_DEATH_KNIGHT:
                    _debuffSpellId = SPELL_WARLOCK_JINX_TRIGGERED_RUNIC_POWER;
                    break;
                case CLASS_HUNTER:
                    _debuffSpellId = SPELL_WARLOCK_JINX_TRIGGERED_FOCUS;
                    break;
                default:
                    break;
            }

            if (_debuffSpellId)
                caster->CastSpell(target, _debuffSpellId, CastSpellExtraArgs(true).AddSpellBP0(jinx->GetAmount()));
        }

        void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            Unit* target = GetTarget();
            target->RemoveAurasDueToSpell(SPELL_WARLOCK_JINX_TRIGGERED_RAGE);
            target->RemoveAurasDueToSpell(SPELL_WARLOCK_JINX_TRIGGERED_ENERGY);
            target->RemoveAurasDueToSpell(SPELL_WARLOCK_JINX_TRIGGERED_RUNIC_POWER);
            target->RemoveAurasDueToSpell(SPELL_WARLOCK_JINX_TRIGGERED_FOCUS);
        }

        void Register() override
        {
            AfterEffectApply += AuraEffectApplyFn(spell_warl_curse_of_weakness::HandleApply, EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, AURA_EFFECT_HANDLE_REAL);
            AfterEffectRemove += AuraEffectRemoveFn(spell_warl_curse_of_weakness::HandleRemove, EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_DONE, AURA_EFFECT_HANDLE_REAL);
        }

    private:
        uint32 _debuffSpellId = 0;
    };

    // -63156 - Decimation (63156, 63158)
    // EFFECT_0 is a real SPELL_AURA_PROC_TRIGGER_SPELL (63165 / 63167); only the health threshold in
    // EFFECT_1 (25%) needs a script.
    class spell_warl_decimation : public AuraScript
    {
        bool CheckProc(ProcEventInfo& eventInfo)
        {
            if (AuraEffect const* healthThresholdEffect = GetEffect(EFFECT_1))
                return eventInfo.GetActionTarget() && eventInfo.GetActionTarget()->GetHealthPct() <= healthThresholdEffect->GetAmount();

            return false;
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_warl_decimation::CheckProc);
        }
    };

    // 77801 - Demon Soul
    // WoWSims cata sim/warlock/demon_soul.go: the buff is picked by the active pet's family and the
    // numbers live in the 794xx auras, so the script only routes the cast.
    class spell_warl_demon_soul : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo(
                {
                    SPELL_WARLOCK_DEMON_SOUL_IMP,
                    SPELL_WARLOCK_DEMON_SOUL_FELHUNTER,
                    SPELL_WARLOCK_DEMON_SOUL_FELGUARD,
                    SPELL_WARLOCK_DEMON_SOUL_SUCCUBUS,
                    SPELL_WARLOCK_DEMON_SOUL_VOIDWALKER
                });
        }

        void OnHitTarget(SpellEffIndex /*effIndex*/)
        {
            Unit* caster = GetCaster();
            Creature* targetCreature = GetHitCreature();
            if (!targetCreature || !targetCreature->IsPet())
                return;

            switch (targetCreature->GetCreatureTemplate()->family)
            {
                case CREATURE_FAMILY_SUCCUBUS:
                    caster->CastSpell(caster, SPELL_WARLOCK_DEMON_SOUL_SUCCUBUS, true);
                    break;
                case CREATURE_FAMILY_VOIDWALKER:
                    caster->CastSpell(caster, SPELL_WARLOCK_DEMON_SOUL_VOIDWALKER, true);
                    break;
                case CREATURE_FAMILY_FELGUARD:
                    caster->CastSpell(caster, SPELL_WARLOCK_DEMON_SOUL_FELGUARD, true);
                    break;
                case CREATURE_FAMILY_FELHUNTER:
                    caster->CastSpell(caster, SPELL_WARLOCK_DEMON_SOUL_FELHUNTER, true);
                    break;
                case CREATURE_FAMILY_IMP:
                    caster->CastSpell(caster, SPELL_WARLOCK_DEMON_SOUL_IMP, true);
                    break;
                default:
                    break;
            }
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_demon_soul::OnHitTarget, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    // 48018 - Demonic Circle: Summon
    // fl-core also toggled 62388 Demonic Circle: Allow Cast from the periodic tick; that spell does
    // not exist in 4.4.2, so the range check moved into the teleport's CheckCast and only the
    // gameobject cleanup is left here.
    class spell_warl_demonic_circle_summon : public AuraScript
    {
        void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes mode)
        {
            // If effect is removed by expire remove the summoned demonic circle too.
            if (!(mode & AURA_EFFECT_HANDLE_REAPPLY))
                GetTarget()->RemoveGameObject(GetId(), true);
        }

        void Register() override
        {
            OnEffectRemove += AuraEffectRemoveFn(spell_warl_demonic_circle_summon::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        }
    };

    // 48020 - Demonic Circle: Teleport
    class spell_warl_demonic_circle_teleport : public SpellScript
    {
        SpellCastResult CheckCast()
        {
            Unit* caster = GetCaster();
            GameObject* circle = caster->GetGameObject(SPELL_WARLOCK_DEMONIC_CIRCLE_SUMMON);
            if (!circle)
                return SPELL_FAILED_NO_VALID_TARGETS;

            if (!caster->IsWithinDist(circle, GetSpellInfo()->GetMaxRange(true, caster)))
                return SPELL_FAILED_OUT_OF_RANGE;

            return SPELL_CAST_OK;
        }

        void Register() override
        {
            OnCheckCast += SpellCheckCastFn(spell_warl_demonic_circle_teleport::CheckCast);
        }
    };

    class spell_warl_demonic_circle_teleport_aura : public AuraScript
    {
        void HandleTeleport(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            if (Player* player = GetTarget()->ToPlayer())
            {
                if (GameObject* circle = player->GetGameObject(SPELL_WARLOCK_DEMONIC_CIRCLE_SUMMON))
                {
                    player->NearTeleportTo(circle->GetPositionX(), circle->GetPositionY(), circle->GetPositionZ(), circle->GetOrientation());
                    player->RemoveMovementImpairingAuras(false);
                }
            }
        }

        void Register() override
        {
            OnEffectApply += AuraEffectApplyFn(spell_warl_demonic_circle_teleport_aura::HandleTeleport, EFFECT_0, SPELL_AURA_MECHANIC_IMMUNITY, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // 47193 - Demonic Empowerment
    class spell_warl_demonic_empowerment : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo(
                {
                    SPELL_WARLOCK_DEMONIC_EMPOWERMENT_SUCCUBUS,
                    SPELL_WARLOCK_DEMONIC_EMPOWERMENT_VOIDWALKER,
                    SPELL_WARLOCK_DEMONIC_EMPOWERMENT_FELGUARD,
                    SPELL_WARLOCK_DEMONIC_EMPOWERMENT_FELHUNTER,
                    SPELL_WARLOCK_DEMONIC_EMPOWERMENT_IMP
                });
        }

        void HandleScriptEffect(SpellEffIndex /*effIndex*/)
        {
            Creature* targetCreature = GetHitCreature();
            if (!targetCreature || !targetCreature->IsPet())
                return;

            switch (targetCreature->GetCreatureTemplate()->family)
            {
                case CREATURE_FAMILY_SUCCUBUS:
                    targetCreature->CastSpell(targetCreature, SPELL_WARLOCK_DEMONIC_EMPOWERMENT_SUCCUBUS, true);
                    break;
                case CREATURE_FAMILY_VOIDWALKER:
                {
                    SpellInfo const* spellInfo = sSpellMgr->AssertSpellInfo(SPELL_WARLOCK_DEMONIC_EMPOWERMENT_VOIDWALKER, GetCastDifficulty());
                    int32 hp = int32(targetCreature->CountPctFromMaxHealth(spellInfo->GetEffect(EFFECT_0).CalcValueAsInt(GetCaster(), nullptr, targetCreature)));
                    targetCreature->CastSpell(targetCreature, SPELL_WARLOCK_DEMONIC_EMPOWERMENT_VOIDWALKER, CastSpellExtraArgs(true).AddSpellBP0(hp));
                    break;
                }
                case CREATURE_FAMILY_FELGUARD:
                    targetCreature->CastSpell(targetCreature, SPELL_WARLOCK_DEMONIC_EMPOWERMENT_FELGUARD, true);
                    break;
                case CREATURE_FAMILY_FELHUNTER:
                    targetCreature->CastSpell(targetCreature, SPELL_WARLOCK_DEMONIC_EMPOWERMENT_FELHUNTER, true);
                    break;
                case CREATURE_FAMILY_IMP:
                    targetCreature->CastSpell(targetCreature, SPELL_WARLOCK_DEMONIC_EMPOWERMENT_IMP, true);
                    break;
                default:
                    break;
            }
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_demonic_empowerment::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    // 689, 89420 - Drain Life
    // 89653 Drain Life (heal) is a single SPELL_EFFECT_HEAL_PCT of 2% in 4.4.2, so the script only has
    // to trigger it once per tick and lets the core read the amount.
    class spell_warl_drain_life : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_DRAIN_LIFE_HEAL });
        }

        void HandlePeriodic(AuraEffect const* aurEff)
        {
            if (Unit* caster = GetCaster())
                caster->CastSpell(caster, SPELL_WARLOCK_DRAIN_LIFE_HEAL, aurEff);
        }

        void Register() override
        {
            OnEffectPeriodic += AuraEffectPeriodicFn(spell_warl_drain_life::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE);
        }
    };

    // 1120 - Drain Soul
    class spell_warl_drain_soul : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SOUL_SHARD_ENERGIZE });
        }

        void OnAuraRemoveHandler(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
                return;

            Unit* caster = GetCaster();
            if (!caster || !caster->IsPlayer())
                return;

            if (caster->ToPlayer()->isHonorOrXPTarget(GetTarget()))
                caster->CastSpell(caster, SPELL_WARLOCK_SOUL_SHARD_ENERGIZE, true);
        }

        void Register() override
        {
            AfterEffectRemove += AuraEffectRemoveFn(spell_warl_drain_soul::OnAuraRemoveHandler, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // 47422 - Everlasting Affliction
    class spell_warl_everlasting_affliction : public SpellScript
    {
        void HandleScriptEffect(SpellEffIndex /*effIndex*/)
        {
            Unit* caster = GetCaster();
            if (Unit* target = GetHitUnit())
            {
                // Refresh Corruption on the target
                if (AuraEffect* aurEff = target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_WARLOCK, flag128(0x2, 0, 0, 0), caster->GetGUID()))
                {
                    aurEff->RecalculateAmount(caster);
                    aurEff->CalculatePeriodic(caster, false, false);
                    aurEff->GetBase()->RefreshDuration();
                }
            }
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_everlasting_affliction::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    // 28176 - Fel Armor
    class spell_warl_fel_armor : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_FEL_ARMOR_HEAL });
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();
            if (!eventInfo.GetDamageInfo())
                return;

            int32 bp = CalculatePct(int32(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmountAsInt());
            if (Unit* caster = GetCaster())
                caster->CastSpell(caster, SPELL_WARLOCK_FEL_ARMOR_HEAL, CastSpellExtraArgs(true).AddSpellBP0(bp));
        }

        void Register() override
        {
            OnEffectProc += AuraEffectProcFn(spell_warl_fel_armor::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
        }
    };

    // 77799 - Fel Flame
    // WoWSims cata sim/warlock/fel_flame.go: extends Immolate and Unstable Affliction by EFFECT_1
    // seconds (6), never past the aura's own maximum duration.
    class spell_warl_fel_flame : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_IMMOLATE, SPELL_WARLOCK_UNSTABLE_AFFLICTION });
        }

        void OnHitTarget(SpellEffIndex /*effIndex*/)
        {
            Unit* caster = GetCaster();
            Unit* target = GetHitUnit();
            Aura* aura = target->GetAura(SPELL_WARLOCK_UNSTABLE_AFFLICTION, caster->GetGUID());
            if (!aura)
                aura = target->GetAura(SPELL_WARLOCK_IMMOLATE, caster->GetGUID());

            if (!aura)
                return;

            int32 newDuration = aura->GetDuration() + GetEffectInfo(EFFECT_1).CalcValueAsInt() * IN_MILLISECONDS;
            aura->SetDuration(std::min(newDuration, aura->GetMaxDuration()));
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_fel_flame::OnHitTarget, EFFECT_1, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    // -47230 - Fel Synergy (47230, 47231)
    class spell_warl_fel_synergy : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_FEL_SYNERGY_HEAL });
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            return GetTarget()->GetGuardianPet() && eventInfo.GetDamageInfo() && eventInfo.GetDamageInfo()->GetDamage();
        }

        void OnProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();

            int32 heal = CalculatePct(int32(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmountAsInt());
            GetTarget()->CastSpell(nullptr, SPELL_WARLOCK_FEL_SYNERGY_HEAL, CastSpellExtraArgs(aurEff).AddSpellBP0(heal)); // TARGET_UNIT_PET
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_warl_fel_synergy::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_warl_fel_synergy::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 63310 - Glyph of Shadowflame
    class spell_warl_glyph_of_shadowflame : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_GLYPH_OF_SHADOWFLAME });
        }

        void OnProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();
            GetTarget()->CastSpell(eventInfo.GetActionTarget(), SPELL_WARLOCK_GLYPH_OF_SHADOWFLAME, aurEff);
        }

        void Register() override
        {
            OnEffectProc += AuraEffectProcFn(spell_warl_glyph_of_shadowflame::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 48181 - Haunt
    // EFFECT_1 stores the heal as a percent of the damage the spell dealt; it is paid out when the
    // aura is removed.
    class spell_warl_haunt : public SpellScript
    {
        void HandleAfterHit()
        {
            if (Aura* aura = GetHitAura())
                if (AuraEffect* aurEff = aura->GetEffect(EFFECT_1))
                    aurEff->SetAmount(CalculatePct(GetHitDamage(), aurEff->GetAmountAsInt()));
        }

        void Register() override
        {
            AfterHit += SpellHitFn(spell_warl_haunt::HandleAfterHit);
        }
    };

    class spell_warl_haunt_aura : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_HAUNT_HEAL });
        }

        void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
        {
            if (Unit* caster = GetCaster())
                GetTarget()->CastSpell(caster, SPELL_WARLOCK_HAUNT_HEAL,
                    CastSpellExtraArgs(aurEff).SetOriginalCaster(GetCasterGUID()).AddSpellBP0(aurEff->GetAmount()));
        }

        void Register() override
        {
            OnEffectRemove += AuraEffectRemoveFn(spell_warl_haunt_aura::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL_OR_REAPPLY_MASK);
        }
    };

    // 755 - Health Funnel
    class spell_warl_health_funnel : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo(
                {
                    SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_R1,
                    SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_R2,
                    SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R1,
                    SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R2
                });
        }

        void ApplyEffect(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            Unit* caster = GetCaster();
            if (!caster)
                return;

            Unit* target = GetTarget();
            if (caster->HasAura(SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_R2))
                target->CastSpell(target, SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R2, true);
            else if (caster->HasAura(SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_R1))
                target->CastSpell(target, SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R1, true);
        }

        void RemoveEffect(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            Unit* target = GetTarget();
            target->RemoveAurasDueToSpell(SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R1);
            target->RemoveAurasDueToSpell(SPELL_WARLOCK_IMPROVED_HEALTH_FUNNEL_BUFF_R2);
        }

        void OnPeriodic(AuraEffect const* aurEff)
        {
            Unit* caster = GetCaster();
            if (!caster)
                return;

            // The health the warlock pays is a percentage of his own maximum health and is not a spell hit.
            uint32 damage = caster->CountPctFromMaxHealth(int32(aurEff->GetBaseAmount()));

            if (Player* modOwner = caster->GetSpellModOwner())
                modOwner->ApplySpellMod(GetSpellInfo(), SpellModOp::PowerCost0, damage);

            SpellNonMeleeDamage damageInfo(caster, caster, GetSpellInfo(), GetAura()->GetSpellVisual(), GetSpellInfo()->SchoolMask, GetAura()->GetCastId());
            damageInfo.damage = damage;
            caster->SendSpellNonMeleeDamageLog(&damageInfo);
            caster->DealSpellDamage(&damageInfo, false);
        }

        void Register() override
        {
            OnEffectApply += AuraEffectApplyFn(spell_warl_health_funnel::ApplyEffect, EFFECT_0, SPELL_AURA_OBS_MOD_HEALTH, AURA_EFFECT_HANDLE_REAL);
            OnEffectRemove += AuraEffectRemoveFn(spell_warl_health_funnel::RemoveEffect, EFFECT_0, SPELL_AURA_OBS_MOD_HEALTH, AURA_EFFECT_HANDLE_REAL);
            OnEffectPeriodic += AuraEffectPeriodicFn(spell_warl_health_funnel::OnPeriodic, EFFECT_0, SPELL_AURA_OBS_MOD_HEALTH);
        }
    };

    // 6262 - Healthstone
    class spell_warl_healthstone_heal : public SpellScript
    {
        void HandleOnHit()
        {
            SetHitHeal(CalculatePct(int32(GetCaster()->GetCreateHealth()), GetHitHeal()));
        }

        void Register() override
        {
            OnHit += SpellHitFn(spell_warl_healthstone_heal::HandleOnHit);
        }
    };

    // -18119 - Improved Soul Fire (18119, 18120)
    class spell_warl_improved_soul_fire : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_IMPROVED_SOUL_FIRE_PCT, SPELL_WARLOCK_IMPROVED_SOUL_FIRE_STATE });
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& /*eventInfo*/)
        {
            PreventDefaultAction();
            Unit* target = GetTarget();
            target->CastSpell(target, SPELL_WARLOCK_IMPROVED_SOUL_FIRE_PCT, CastSpellExtraArgs(aurEff).AddSpellBP0(aurEff->GetAmount()));
            target->CastSpell(target, SPELL_WARLOCK_IMPROVED_SOUL_FIRE_STATE, aurEff);
        }

        void Register() override
        {
            OnEffectProc += AuraEffectProcFn(spell_warl_improved_soul_fire::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 29722 - Incinerate
    // WoWSims cata sim/warlock/incinerate.go: x7/6 damage while the target carries Immolate.
    // Fire and Brimstone adds its EFFECT_0 percent on top of that (sim/warlock/immolate.go).
    class spell_warl_incinerate : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_IMMOLATE });
        }

        void HandleDamageBonus(SpellEffIndex /*effIndex*/)
        {
            int32 bp = GetEffectValueAsInt();
            Unit* caster = GetCaster();
            if (Unit* target = GetHitUnit())
            {
                if (target->GetAuraEffect(SPELL_WARLOCK_IMMOLATE, EFFECT_2, caster->GetGUID()))
                {
                    bp += bp / 6;
                    if (AuraEffect const* aurEff = caster->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_WARLOCK, FireAndBrimstoneMask))
                        AddPct(bp, aurEff->GetAmount());
                }
            }

            SetEffectValue(bp);
        }

        void Register() override
        {
            OnEffectLaunchTarget += SpellEffectFn(spell_warl_incinerate::HandleDamageBonus, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        }
    };

    // -18179 - Jinx (18179, 85479)
    // The number of extra targets is EFFECT_2 of rank 1 (15); rank 2 has no third effect.
    class spell_warl_jinx : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_JINX_R1, SPELL_WARLOCK_JINX_AOE_R1, SPELL_WARLOCK_JINX_AOE_R2 });
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            return eventInfo.GetActionTarget() != nullptr;
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();
            int32 targets = sSpellMgr->AssertSpellInfo(SPELL_WARLOCK_JINX_R1, GetCastDifficulty())->GetEffect(EFFECT_2).CalcValueAsInt();
            uint32 spellId = GetId() == SPELL_WARLOCK_JINX_R1 ? SPELL_WARLOCK_JINX_AOE_R1 : SPELL_WARLOCK_JINX_AOE_R2;
            GetTarget()->CastSpell(eventInfo.GetActionTarget(), spellId, CastSpellExtraArgs(aurEff).AddSpellMod(SPELLVALUE_MAX_TARGETS, targets));
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_warl_jinx::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_warl_jinx::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 1454 - Life Tap
    // WoWSims cata sim/warlock/lifetap.go: mana = 15% of maximum health (EFFECT_2) x 120% (EFFECT_1),
    // then x (1 + 0.1 per point of Improved Life Tap).
    class spell_warl_life_tap : public SpellScript
    {
        bool Load() override
        {
            return GetCaster()->IsPlayer();
        }

        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_LIFE_TAP_ENERGIZE });
        }

        void HandleDummy(SpellEffIndex /*effIndex*/)
        {
            Player* caster = GetCaster()->ToPlayer();
            if (Unit* target = GetHitUnit())
            {
                int32 damage = int32(caster->CountPctFromMaxHealth(GetEffectInfo(EFFECT_2).CalcValueAsInt()));
                int32 mana = CalculatePct(damage, GetEffectInfo(EFFECT_1).CalcValueAsInt());

                // Shouldn't appear in the combat log
                target->ModifyHealth(-damage);

                if (AuraEffect const* aurEff = caster->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_WARLOCK, ImprovedLifeTapMask))
                    AddPct(mana, aurEff->GetAmount());

                caster->CastSpell(target, SPELL_WARLOCK_LIFE_TAP_ENERGIZE, CastSpellExtraArgs(true).AddSpellBP0(mana));
            }
        }

        SpellCastResult CheckCast()
        {
            if (int32(GetCaster()->GetHealth()) > int32(GetCaster()->CountPctFromMaxHealth(GetEffectInfo(EFFECT_2).CalcValueAsInt())))
                return SPELL_CAST_OK;

            return SPELL_FAILED_FIZZLE;
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_life_tap::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
            OnCheckCast += SpellCheckCastFn(spell_warl_life_tap::CheckCast);
        }
    };

    // 687 - Demon Armor
    // 28176 - Fel Armor
    // EFFECT_2 is SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS and Unit::GetCastSpellInfo reads its amount as
    // a spell id. The 4.4.2 base value is 6112, which is not a spell, so the script writes the right
    // ward: 91711 Nether Ward with the talent, 6229 Shadow Ward without it.
    class spell_warl_nether_ward_overrride : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_NETHER_TALENT, SPELL_WARLOCK_NETHER_WARD, SPELL_WARLOCK_SHADOW_WARD });
        }

        void CalculateAmount(AuraEffect const* /*aurEff*/, SpellEffectValue& amount, bool& /*canBeRecalculated*/)
        {
            if (GetUnitOwner()->HasAura(SPELL_WARLOCK_NETHER_TALENT))
                amount = SPELL_WARLOCK_NETHER_WARD;
            else
                amount = SPELL_WARLOCK_SHADOW_WARD;
        }

        void Register() override
        {
            DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_warl_nether_ward_overrride::CalculateAmount, EFFECT_2, SPELL_AURA_OVERRIDE_ACTIONBAR_SPELLS);
        }
    };

    // -85099 - Pandemic (85099, 85100)
    // 4.4.2 tooltip: "Your Drain Soul has a $s1% chance to refresh the duration of your Unstable
    // Affliction when dealing damage on targets below 25% health"; WoWSims cata
    // sim/warlock/talents_affliction.go registerPandemic uses 50% / 100%.
    // EFFECT_1 (the Bane and Curse global cooldown cut) is a real spell modifier and needs no script.
    class spell_warl_pandemic : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_PANDEMIC_SCRIPT_EFFECT });
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            if (!eventInfo.GetActionTarget())
                return false;

            return eventInfo.GetActionTarget()->GetHealthPct() <= 25.0f && roll_chance(GetEffectInfo(EFFECT_0).CalcValueAsInt());
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();
            GetTarget()->CastSpell(eventInfo.GetActionTarget(), SPELL_WARLOCK_PANDEMIC_SCRIPT_EFFECT, aurEff);
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_warl_pandemic::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_warl_pandemic::HandleProc, EFFECT_1, SPELL_AURA_ADD_FLAT_MODIFIER);
        }
    };

    // 92931 - Pandemic
    class spell_warl_pandemic_script : public SpellScript
    {
        void HandleScriptEffect(SpellEffIndex /*effIndex*/)
        {
            Unit* caster = GetCaster();
            if (!caster)
                return;

            Unit* target = GetHitUnit();
            if (AuraEffect* aurEff = target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_WARLOCK, flag128(0, 0x100, 0, 0), caster->GetGUID()))
            {
                aurEff->RecalculateAmount(caster);
                aurEff->CalculatePeriodic(caster, false, false);
                aurEff->GetBase()->RefreshDuration();
            }
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_pandemic_script::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    // 6358 - Seduction (Special Ability)
    class spell_warl_seduction : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_GLYPH_OF_SUCCUBUS, SPELL_PRIEST_SHADOW_WORD_DEATH });
        }

        void HandleScriptEffect(SpellEffIndex /*effIndex*/)
        {
            Unit* caster = GetCaster();
            if (Unit* target = GetHitUnit())
            {
                if (caster->GetOwner() && caster->GetOwner()->HasAura(SPELL_WARLOCK_GLYPH_OF_SUCCUBUS))
                {
                    target->RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE, ObjectGuid::Empty, target->GetAura(SPELL_PRIEST_SHADOW_WORD_DEATH)); // SW:D shall not be removed
                    target->RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
                    target->RemoveAurasByType(SPELL_AURA_PERIODIC_LEECH);
                }
            }
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_seduction::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_APPLY_AURA);
        }
    };

    // 27243 - Seed of Corruption
    // WoWSims cata sim/warlock/seed.go: the seed detonates once the damage it has absorbed passes the
    // EFFECT_1 threshold, or when the target dies.
    class spell_warl_seed_of_corruption : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SEED_OF_CORRUPTION_TRIGGERED, SPELL_WARLOCK_SEED_OF_CORRUPTION_VISUAL });
        }

        void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            Unit* caster = GetCaster();
            if (!caster)
                return;

            if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
                return;

            Unit* target = GetTarget();
            caster->CastSpell(target, SPELL_WARLOCK_SEED_OF_CORRUPTION_TRIGGERED, true);
            target->CastSpell(target, SPELL_WARLOCK_SEED_OF_CORRUPTION_VISUAL, true);
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            return eventInfo.GetDamageInfo() != nullptr;
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();
            _takenDamage += eventInfo.GetDamageInfo()->GetDamage();

            if (_takenDamage < uint32(aurEff->GetAmountAsInt()))
                return;

            Unit* caster = GetCaster();
            if (!caster)
                return;

            caster->CastSpell(GetTarget(), SPELL_WARLOCK_SEED_OF_CORRUPTION_TRIGGERED, true);
            Remove();
        }

        void Register() override
        {
            AfterEffectRemove += AuraEffectRemoveFn(spell_warl_seed_of_corruption::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
            DoCheckProc += AuraCheckProcFn(spell_warl_seed_of_corruption::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_warl_seed_of_corruption::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
        }

    private:
        uint32 _takenDamage = 0;
    };

    // 87385 - Seed of Corruption (detonation)
    class spell_warl_seed_of_corruption_aoe : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_CORRUPTION_TRIGGERED, SPELL_WARLOCK_SEED_OF_CORRUPTION });
        }

        void HandleHit(SpellEffIndex /*effIndex*/)
        {
            if (GetSpell()->IsTriggeredByAura(sSpellMgr->AssertSpellInfo(SPELL_WARLOCK_SEED_OF_CORRUPTION, GetCastDifficulty())))
                if (Unit* caster = GetCaster())
                    caster->CastSpell(GetHitUnit(), SPELL_WARLOCK_CORRUPTION_TRIGGERED, true);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_seed_of_corruption_aoe::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        }
    };

    // -18094 - Nightfall (18094, 18095)
    // 56218 - Glyph of Corruption
    class spell_warl_shadow_trance_proc : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SHADOW_TRANCE });
        }

        void OnProc(AuraEffect* aurEff, ProcEventInfo& /*eventInfo*/)
        {
            PreventDefaultAction();
            GetTarget()->CastSpell(GetTarget(), SPELL_WARLOCK_SHADOW_TRANCE, aurEff);
        }

        void Register() override
        {
            OnEffectProc += AuraEffectProcFn(spell_warl_shadow_trance_proc::OnProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 6229 - Shadow Ward
    // 6229 has no spell power coefficient in 4.4.2 DB2 and this core has no spell_bonus_data, so the
    // +80.7% of spell power stays in the script.
    class spell_warl_shadow_ward : public AuraScript
    {
        void CalculateAmount(AuraEffect const* /*aurEff*/, SpellEffectValue& amount, bool& canBeRecalculated)
        {
            canBeRecalculated = false;
            if (Unit* caster = GetCaster())
                amount += 0.807f * caster->SpellBaseHealingBonusDone(GetSpellInfo()->GetSchoolMask());
        }

        void Register() override
        {
            DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_warl_shadow_ward::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        }
    };

    // 29341 - Shadowburn
    // The aura sits on the victim for 5 s; if it dies the warlock is refunded EFFECT_0 soul shards.
    class spell_warl_shadowburn : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SOUL_SHARD });
        }

        void OnAuraRemoveHandler(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
        {
            Unit* caster = GetCaster();
            if (!caster || !caster->IsPlayer())
                return;

            if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
                return;

            if (caster->ToPlayer()->isHonorOrXPTarget(GetTarget()))
                caster->CastSpell(caster, SPELL_WARLOCK_SOUL_SHARD, CastSpellExtraArgs(aurEff).AddSpellBP0(aurEff->GetAmount()));
        }

        void Register() override
        {
            AfterEffectRemove += AuraEffectRemoveFn(spell_warl_shadowburn::OnAuraRemoveHandler, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // 47897 - Shadowflame (ShatterCore)
    class spell_warl_shadowflame : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SHADOWFLAME_PERIODIC });
        }

        void HandlePeriodicDamageEffect(SpellEffIndex /*effIndex*/)
        {
            if (Unit* caster = GetCaster())
                caster->CastSpell(GetHitUnit(), SPELL_WARLOCK_SHADOWFLAME_PERIODIC, true);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_shadowflame::HandlePeriodicDamageEffect, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        }
    };

    // 79268 - Soul Harvest
    // 9 ticks of 1 s, one shard on every third tick: 3 shards, as the 4.4.2 tooltip ($101977m1*3) and
    // WoWSims cata sim/warlock/soul_harvest.go both say.
    class spell_warl_soul_harvest : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SOUL_HARVEST_ENERGIZE });
        }

        void HandleDummy(AuraEffect const* aurEff)
        {
            if (!(aurEff->GetTickNumber() % 3))
                GetTarget()->CastSpell(GetTarget(), SPELL_WARLOCK_SOUL_HARVEST_ENERGIZE, aurEff);
        }

        void Register() override
        {
            OnEffectPeriodic += AuraEffectPeriodicFn(spell_warl_soul_harvest::HandleDummy, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        }
    };

    // -30293 - Soul Leech (30293, 30295)
    // EFFECT_0 is SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE -> 30294 (health and mana), handled by the
    // core; the script only adds Replenishment.
    class spell_warl_soul_leech : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_GEN_REPLENISHMENT });
        }

        void OnProc(AuraEffect* aurEff, ProcEventInfo& /*eventInfo*/)
        {
            GetTarget()->CastSpell(nullptr, SPELL_GEN_REPLENISHMENT, aurEff);
        }

        void Register() override
        {
            OnEffectProc += AuraEffectProcFn(spell_warl_soul_leech::OnProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE);
        }
    };

    // 86211 - Soul Swap Override, also the container for the copied dots
    class spell_warl_soul_swap_override : public AuraScript
    {
        // Pure virtual, needs a body to link
        void Register() override { }

    public:
        void AddDot(uint32 id) { _dotList.push_back(id); }
        std::vector<uint32> const& GetDotList() const { return _dotList; }
        Unit* GetOriginalSwapSource() const { return _swapCaster; }
        void SetOriginalSwapSource(Unit* victim) { _swapCaster = victim; }

    private:
        std::vector<uint32> _dotList;
        Unit* _swapCaster = nullptr;
    };

    // 86121 - Soul Swap
    class spell_warl_soul_swap : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SOUL_SWAP_OVERRIDE, SPELL_WARLOCK_SOUL_SWAP_DOT_MARKER });
        }

        void HandleHit(SpellEffIndex /*effIndex*/)
        {
            GetCaster()->CastSpell(GetCaster(), SPELL_WARLOCK_SOUL_SWAP_OVERRIDE, true);
            GetHitUnit()->CastSpell(GetCaster(), SPELL_WARLOCK_SOUL_SWAP_DOT_MARKER, true);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_soul_swap::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        }
    };

    // 92795 - Soul Swap, copies the dot ids into the override aura
    class spell_warl_soul_swap_dot_marker : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SOUL_SWAP_OVERRIDE });
        }

        void HandleHit(SpellEffIndex effIndex)
        {
            Unit* swapVictim = GetCaster();
            Unit* warlock = GetHitUnit();
            if (!warlock || !swapVictim)
                return;

            flag128 classMask = GetEffectInfo(effIndex).SpellClassMask;

            Aura* swapOverrideAura = warlock->GetAura(SPELL_WARLOCK_SOUL_SWAP_OVERRIDE);
            if (!swapOverrideAura)
                return;

            spell_warl_soul_swap_override* swapSpellScript = swapOverrideAura->GetScript<spell_warl_soul_swap_override>();
            if (!swapSpellScript)
                return;

            for (auto const& [spellId, application] : swapVictim->GetAppliedAuras())
            {
                SpellInfo const* spellProto = application->GetBase()->GetSpellInfo();
                if (application->GetBase()->GetCaster() == warlock)
                    if (spellProto->SpellFamilyName == SPELLFAMILY_WARLOCK && (spellProto->SpellFamilyFlags & classMask))
                        swapSpellScript->AddDot(spellId);
            }

            swapSpellScript->SetOriginalSwapSource(swapVictim);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_soul_swap_dot_marker::HandleHit, EFFECT_0, SPELL_EFFECT_DUMMY);
        }
    };

    // 86213 - Soul Swap Exhale
    class spell_warl_soul_swap_exhale : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo(
                {
                    SPELL_WARLOCK_SOUL_SWAP_MOD_COST,
                    SPELL_WARLOCK_SOUL_SWAP_OVERRIDE,
                    SPELL_WARLOCK_SOUL_SWAP_CD_MARKER,
                    SPELL_WARLOCK_GLYPH_OF_SOUL_SWAP
                });
        }

        SpellCastResult CheckCast()
        {
            Unit* currentTarget = GetExplTargetUnit();
            Unit* swapTarget = nullptr;
            if (Aura const* swapOverride = GetCaster()->GetAura(SPELL_WARLOCK_SOUL_SWAP_OVERRIDE))
                if (spell_warl_soul_swap_override* swapScript = swapOverride->GetScript<spell_warl_soul_swap_override>())
                    swapTarget = swapScript->GetOriginalSwapSource();

            // Soul Swap Exhale can't be cast on the same target as Soul Swap
            if (swapTarget && currentTarget && swapTarget == currentTarget)
                return SPELL_FAILED_BAD_TARGETS;

            return SPELL_CAST_OK;
        }

        void OnEffectHit(SpellEffIndex /*effIndex*/)
        {
            Unit* caster = GetCaster();
            caster->CastSpell(caster, SPELL_WARLOCK_SOUL_SWAP_MOD_COST, true);
            bool hasGlyph = caster->HasAura(SPELL_WARLOCK_GLYPH_OF_SOUL_SWAP);

            std::vector<uint32> dotList;
            Unit* swapSource = nullptr;
            if (Aura const* swapOverride = caster->GetAura(SPELL_WARLOCK_SOUL_SWAP_OVERRIDE))
            {
                spell_warl_soul_swap_override* swapScript = swapOverride->GetScript<spell_warl_soul_swap_override>();
                if (!swapScript)
                    return;

                dotList = swapScript->GetDotList();
                swapSource = swapScript->GetOriginalSwapSource();
            }

            if (dotList.empty())
                return;

            for (uint32 spellId : dotList)
            {
                caster->AddAura(spellId, GetHitUnit());
                if (!hasGlyph && swapSource)
                    swapSource->RemoveAurasDueToSpell(spellId);
            }

            caster->RemoveAurasDueToSpell(SPELL_WARLOCK_SOUL_SWAP_OVERRIDE);

            if (hasGlyph) // the glyph puts Soul Swap on cooldown instead of refunding it
                caster->CastSpell(caster, SPELL_WARLOCK_SOUL_SWAP_CD_MARKER, false);
        }

        void Register() override
        {
            OnCheckCast += SpellCheckCastFn(spell_warl_soul_swap_exhale::CheckCast);
            OnEffectHitTarget += SpellEffectFn(spell_warl_soul_swap_exhale::OnEffectHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        }
    };

    // 74434 - Soulburn
    // EFFECT_0 (summon and Soul Fire cast time) and EFFECT_1 (89420 Drain Life override) are real
    // aura effects in 4.4.2; only the three secondary buffs need the script.
    class spell_warl_soulburn : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo(
                {
                    SPELL_WARLOCK_SOULBURN_HEALTHSTONE,
                    SPELL_WARLOCK_SOULBURN_SEARING_PAIN,
                    SPELL_WARLOCK_SOULBURN_DEMONIC_CIRCLE,
                    SPELL_WARLOCK_HEALTHSTONE,
                    SPELL_WARLOCK_DEMONIC_CIRCLE_TELEPORT,
                    SPELL_WARLOCK_SEARING_PAIN
                });
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            return eventInfo.GetSpellInfo() && GetTarget()->IsPlayer();
        }

        void HandleProc(AuraEffect* aurEff, ProcEventInfo& eventInfo)
        {
            Unit* target = GetTarget();

            switch (eventInfo.GetSpellInfo()->Id)
            {
                case SPELL_WARLOCK_HEALTHSTONE:
                    target->CastSpell(target, SPELL_WARLOCK_SOULBURN_HEALTHSTONE, aurEff);
                    break;
                case SPELL_WARLOCK_DEMONIC_CIRCLE_TELEPORT:
                    target->CastSpell(target, SPELL_WARLOCK_SOULBURN_DEMONIC_CIRCLE, aurEff);
                    break;
                case SPELL_WARLOCK_SEARING_PAIN:
                    target->CastSpell(target, SPELL_WARLOCK_SOULBURN_SEARING_PAIN, aurEff);
                    break;
                default:
                    break;
            }
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_warl_soulburn::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_warl_soulburn::HandleProc, EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER);
        }
    };

    // 29858 - Soulshatter
    class spell_warl_soulshatter : public SpellScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_SOULSHATTER_EFFECT });
        }

        void HandleDummy(SpellEffIndex /*effIndex*/)
        {
            Unit* caster = GetCaster();
            if (Unit* target = GetHitUnit())
                if (target->GetThreatManager().IsThreatenedBy(caster, true))
                    caster->CastSpell(target, SPELL_WARLOCK_SOULSHATTER_EFFECT, true);
        }

        void Register() override
        {
            OnEffectHitTarget += SpellEffectFn(spell_warl_soulshatter::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
        }
    };

    // 30108, 34438, 34439, 35183 - Unstable Affliction
    class spell_warl_unstable_affliction : public AuraScript
    {
        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_WARLOCK_UNSTABLE_AFFLICTION_DISPEL });
        }

        void HandleDispel(DispelInfo* dispelInfo)
        {
            if (Unit* caster = GetCaster())
                if (AuraEffect const* aurEff = GetEffect(EFFECT_1))
                {
                    // backfire damage and silence
                    SpellEffectValue damage = aurEff->GetAmount() * 9;
                    caster->CastSpell(dispelInfo->GetDispeller()->ToUnit(), SPELL_WARLOCK_UNSTABLE_AFFLICTION_DISPEL,
                        CastSpellExtraArgs(aurEff).AddSpellBP0(damage));
                }
        }

        void Register() override
        {
            AfterDispel += AuraDispelFn(spell_warl_unstable_affliction::HandleDispel);
        }
    };
}

void AddSC_warlock_spell_scripts()
{
    using namespace Scripts::Spells::Warlock;
    RegisterSpellScript(spell_warl_aftermath);
    RegisterSpellScript(spell_warl_bane_of_doom);
    RegisterSpellScript(spell_warl_bane_of_havoc);
    RegisterSpellScript(spell_warl_bane_of_havoc_tracking_aura);
    RegisterSpellScript(spell_warl_banish);
    RegisterSpellScript(spell_warl_burning_embers);
    RegisterSpellScript(spell_warl_conflagrate);
    RegisterSpellScript(spell_warl_create_healthstone);
    RegisterSpellScript(spell_warl_curse_of_weakness);
    RegisterSpellScript(spell_warl_decimation);
    RegisterSpellScript(spell_warl_demon_soul);
    RegisterSpellScript(spell_warl_demonic_circle_summon);
    RegisterSpellAndAuraScriptPair(spell_warl_demonic_circle_teleport, spell_warl_demonic_circle_teleport_aura);
    RegisterSpellScript(spell_warl_demonic_empowerment);
    RegisterSpellScript(spell_warl_drain_life);
    RegisterSpellScript(spell_warl_drain_soul);
    RegisterSpellScript(spell_warl_everlasting_affliction);
    RegisterSpellScript(spell_warl_fel_armor);
    RegisterSpellScript(spell_warl_fel_flame);
    RegisterSpellScript(spell_warl_fel_synergy);
    RegisterSpellScript(spell_warl_glyph_of_shadowflame);
    RegisterSpellAndAuraScriptPair(spell_warl_haunt, spell_warl_haunt_aura);
    RegisterSpellScript(spell_warl_health_funnel);
    RegisterSpellScript(spell_warl_healthstone_heal);
    RegisterSpellScript(spell_warl_improved_soul_fire);
    RegisterSpellScript(spell_warl_incinerate);
    RegisterSpellScript(spell_warl_jinx);
    RegisterSpellScript(spell_warl_life_tap);
    RegisterSpellScript(spell_warl_nether_ward_overrride);
    RegisterSpellScript(spell_warl_pandemic);
    RegisterSpellScript(spell_warl_pandemic_script);
    RegisterSpellScript(spell_warl_seduction);
    RegisterSpellScript(spell_warl_seed_of_corruption);
    RegisterSpellScript(spell_warl_seed_of_corruption_aoe);
    RegisterSpellScript(spell_warl_shadow_trance_proc);
    RegisterSpellScript(spell_warl_shadow_ward);
    RegisterSpellScript(spell_warl_shadowburn);
    RegisterSpellScript(spell_warl_shadowflame);
    RegisterSpellScript(spell_warl_soul_harvest);
    RegisterSpellScript(spell_warl_soul_leech);
    RegisterSpellScript(spell_warl_soul_swap);
    RegisterSpellScript(spell_warl_soul_swap_dot_marker);
    RegisterSpellScript(spell_warl_soul_swap_exhale);
    RegisterSpellScript(spell_warl_soul_swap_override);
    RegisterSpellScript(spell_warl_soulburn);
    RegisterSpellScript(spell_warl_soulshatter);
    RegisterSpellScript(spell_warl_unstable_affliction);
}

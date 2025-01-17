#include "nexus.h"
#include "LFGMgr.h"
#include "Group.h"

enum Spells
{
    //Spells
    SPELL_FROZEN_PRISON                           = 47854,
    SPELL_TAIL_SWEEP                              = 50155,
    SPELL_CRYSTAL_CHAINS                          = 50997,
    SPELL_ENRAGE                                  = 8599,
    SPELL_CRYSTALFIRE_BREATH                      = 48096,
    H_SPELL_CRYSTALFIRE_BREATH                    = 57091,
    SPELL_CRYSTALIZE                              = 48179,
    SPELL_INTENSE_COLD                            = 48094,
    SPELL_INTENSE_COLD_TRIGGERED                  = 48095
};

enum Texts
{
    SAY_AGGRO                                     = 0,
    SAY_SLAY                                      = 1,
    SAY_ENRAGE                                    = 2,
    SAY_DEATH                                     = 3,
    SAY_CRYSTAL_NOVA                              = 4,
    SAY_FRENZY                                    = 5
};

enum Misc
{
    DATA_INTENSE_COLD                             = 1,
    DATA_CONTAINMENT_SPHERES                      = 3,
};

class boss_keristrasza : public CreatureScript
{
public:
    boss_keristrasza() : CreatureScript("boss_keristrasza") { }

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new boss_keristraszaAI (creature);
    }

    struct boss_keristraszaAI : public ScriptedAI
    {
        boss_keristraszaAI(Creature* creature) : ScriptedAI(creature)
        {
            instance = creature->GetInstanceScript();
            me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC);
            me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
            DoCast(SPELL_FROZEN_PRISON);
        }

        InstanceScript* instance;

        GuidList intenseColdList;
        ObjectGuid auiContainmentSphereGUIDs[DATA_CONTAINMENT_SPHERES];
        uint32 uiCrystalfireBreathTimer;
        uint32 uiCrystalChainsCrystalizeTimer;
        uint32 uiTailSweepTimer;
        bool intenseCold;
        bool bEnrage;

        void Reset() override
        {
            uiCrystalfireBreathTimer = 14*IN_MILLISECONDS;
            uiCrystalChainsCrystalizeTimer = DUNGEON_MODE(30*IN_MILLISECONDS, 11*IN_MILLISECONDS);
            uiTailSweepTimer = 5*IN_MILLISECONDS;
            bEnrage = false;
            intenseCold = true;
            intenseColdList.clear();
            RemovePrison(CheckContainmentSpheres());
            if (instance)
                instance->SetData(DATA_KERISTRASZA_EVENT, NOT_STARTED);
        }

        void JustReachedHome() override
        {
            if (!me->HasAura(SPELL_FROZEN_PRISON))
            {
                me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC);
                me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
            }
        }

        void EnterCombat(Unit* /*who*/) override
        {
            Talk(SAY_AGGRO);
            DoCast(SPELL_INTENSE_COLD);

            if (instance)
                instance->SetData(DATA_KERISTRASZA_EVENT, IN_PROGRESS);
        }

        void JustDied(Unit* /*killer*/) override
        {
            Talk(SAY_DEATH);

            if (instance)
            {
                instance->SetData(DATA_KERISTRASZA_EVENT, DONE);
                Map::PlayerList const& players = me->GetMap()->GetPlayers();
                if (!players.isEmpty())
                {
                    Player* pPlayer = players.begin()->getSource();
                    if (pPlayer && pPlayer->GetGroup())
                        if (sLFGMgr->GetQueueId(995))
                            sLFGMgr->FinishDungeon(pPlayer->GetGroup()->GetGUID(), 995, me->GetMap());
                }
            }
        }

        void KilledUnit(Unit* /*victim*/) override
        {
            Talk(SAY_SLAY);
        }

        bool CheckContainmentSpheres(bool remove_prison = false)
        {
            if (!instance)
                return false;

            auiContainmentSphereGUIDs[0] = instance->GetGuidData(ANOMALUS_CONTAINMET_SPHERE);
            auiContainmentSphereGUIDs[1] = instance->GetGuidData(ORMOROKS_CONTAINMET_SPHERE);
            auiContainmentSphereGUIDs[2] = instance->GetGuidData(TELESTRAS_CONTAINMET_SPHERE);

            GameObject* ContainmentSpheres[DATA_CONTAINMENT_SPHERES];

            for (uint8 i = 0; i < DATA_CONTAINMENT_SPHERES; ++i)
            {
                ContainmentSpheres[i] = instance->instance->GetGameObject(auiContainmentSphereGUIDs[i]);
                if (!ContainmentSpheres[i])
                    return false;
                if (ContainmentSpheres[i]->GetGoState() != GO_STATE_ACTIVE)
                    return false;
            }
            if (remove_prison)
                RemovePrison(true);
            return true;
        }

        void RemovePrison(bool remove)
        {
            if (remove)
            {
                me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC);
                me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
                if (me->HasAura(SPELL_FROZEN_PRISON))
                    me->RemoveAurasDueToSpell(SPELL_FROZEN_PRISON);
            }
            else
            {
                me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC);
                me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
                DoCast(me, SPELL_FROZEN_PRISON, false);
            }
        }

        void SetGUID(ObjectGuid const& guid, int32 id/* = 0 */) override
        {
            if (id == DATA_INTENSE_COLD)
                intenseColdList.push_back(guid);
        }

        void UpdateAI(uint32 diff) override
        {
            if (!UpdateVictim())
                return;

            if (!bEnrage && HealthBelowPct(25))
            {
                Talk(SAY_ENRAGE);
                DoCast(me, SPELL_ENRAGE);
                bEnrage = true;
            }

            if (uiCrystalfireBreathTimer <= diff)
            {
                DoCast(me->getVictim(), SPELL_CRYSTALFIRE_BREATH);
                uiCrystalfireBreathTimer = 14*IN_MILLISECONDS;
            } else uiCrystalfireBreathTimer -= diff;

            if (uiTailSweepTimer <= diff)
            {
                DoCast(me, SPELL_TAIL_SWEEP);
                uiTailSweepTimer = 5*IN_MILLISECONDS;
            } else uiTailSweepTimer -= diff;

            if (uiCrystalChainsCrystalizeTimer <= diff)
            {
                Talk(SAY_CRYSTAL_NOVA);
                if (IsHeroic())
                    DoCast(me, SPELL_CRYSTALIZE);
                else if (Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 0, 100, true))
                    DoCast(target, SPELL_CRYSTAL_CHAINS);
                uiCrystalChainsCrystalizeTimer = DUNGEON_MODE(30*IN_MILLISECONDS, 11*IN_MILLISECONDS);
            } else uiCrystalChainsCrystalizeTimer -= diff;

            DoMeleeAttackIfReady();
        }
    };

};

class containment_sphere : public GameObjectScript
{
public:
    containment_sphere() : GameObjectScript("containment_sphere") { }

    bool OnGossipHello(Player* /*player*/, GameObject* go) override
    {
        InstanceScript* instance = go->GetInstanceScript();

        Creature* pKeristrasza = Unit::GetCreature(*go, instance ? instance->GetGuidData(DATA_KERISTRASZA) : ObjectGuid::Empty);
        if (pKeristrasza && pKeristrasza->isAlive())
        {
            // maybe these are hacks :(
            go->SetFlag(GAMEOBJECT_FIELD_FLAGS, GO_FLAG_NOT_SELECTABLE);
            go->SetGoState(GO_STATE_ACTIVE);

            CAST_AI(boss_keristrasza::boss_keristraszaAI, pKeristrasza->AI())->CheckContainmentSpheres(true);
        }
        return true;
    }

};

class spell_intense_cold : public SpellScriptLoader
{
    public:
        spell_intense_cold() : SpellScriptLoader("spell_intense_cold") { }

        class spell_intense_cold_AuraScript : public AuraScript
        {
            PrepareAuraScript(spell_intense_cold_AuraScript);

            void HandlePeriodicTick(AuraEffect const* aurEff)
            {
                if (aurEff->GetBase()->GetStackAmount() < 2)
                    return;
                Unit* caster = GetCaster();
                //TODO: the caster should be boss but not the player
                if (!caster || !caster->GetAI())
                    return;
                caster->GetAI()->SetGUID(GetTarget()->GetGUID(), DATA_INTENSE_COLD);
            }

            void Register() override
            {
                OnEffectPeriodic += AuraEffectPeriodicFn(spell_intense_cold_AuraScript::HandlePeriodicTick, EFFECT_1, SPELL_AURA_PERIODIC_DAMAGE);
            }
        };

        AuraScript* GetAuraScript() const override
        {
            return new spell_intense_cold_AuraScript();
        }
};

class achievement_intense_cold : public AchievementCriteriaScript
{
    public:
        achievement_intense_cold() : AchievementCriteriaScript("achievement_intense_cold")
        {
        }

        bool OnCheck(Player* player, Unit* target) override
        {
            if (!target)
                return false;

            GuidList intenseColdList = CAST_AI(boss_keristrasza::boss_keristraszaAI, target->ToCreature()->AI())->intenseColdList;
            if (!intenseColdList.empty())
                for (GuidList::iterator itr = intenseColdList.begin(); itr != intenseColdList.end(); ++itr)
                    if (player->GetGUID() == *itr)
                        return false;

            return true;
        }
};

void AddSC_boss_keristrasza()
{
    new boss_keristrasza();
    new containment_sphere();
    new achievement_intense_cold();
    new spell_intense_cold();
}

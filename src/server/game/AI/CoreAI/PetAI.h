/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
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

#ifndef TRINITY_PETAI_H
#define TRINITY_PETAI_H

#include "CreatureAI.h"
#include "Timer.h"

class Creature;
class Spell;

class PetAI : public CreatureAI
{
public:

    explicit PetAI(Creature* c);

    void InitializeAI() override;
    void EnterEvadeMode() override;
    void JustDied(Unit* /*who*/) override;

    void UpdateAI(uint32) override;
    static int Permissible(const Creature*);

    void KilledUnit(Unit* /*victim*/) override;
    // only start attacking if not attacking something else already
    void AttackStart(Unit* target) override;
    // always start attacking if possible
    void _AttackStart(Unit* target);
    void MovementInform(uint32 moveType, uint32 data) override;
    void OwnerDamagedBy(Unit* attacker) override;
    void OwnerAttacked(Unit* target) override;
    void ReceiveEmote(Player* player, uint32 textEmote) override;

private:
    bool _needToStop();
    void _stopAttack();

    void UpdateAllies();

    TimeTracker i_tracker;
    GuidSet m_AllySet;
    uint32 m_updateAlliesTimer;
    uint32 m_timeCheckSelf;

    Unit* SelectNextTarget();
    void HandleReturnMovement();
    void DoAttack(Unit* target, bool chase);
    bool CanAttack(Unit* target);
};
#endif


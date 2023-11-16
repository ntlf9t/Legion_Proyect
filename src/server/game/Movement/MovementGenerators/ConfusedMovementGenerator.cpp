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

#include "Unit.h"
#include "Creature.h"
#include "MapManager.h"
#include "ConfusedMovementGenerator.h"
#include "VMapFactory.h"
#include "MoveSplineInit.h"
#include "Player.h"
#include "MoveSpline.h"

#define WALK_SPEED_YARDS_PER_SECOND 2.45f

template<class T>
void ConfusedMovementGenerator<T>::DoInitialize(T &unit)
{
    unit.AddUnitState(UNIT_STATE_CONFUSED);
    unit.SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED);
    unit.GetPosition(i_x, i_y, i_z, unit.GetTransport());
    //i_duration = unit.GetTimeForSpline();
    //unit.SetSpeed(MOVE_WALK, 1.f, true);
    //speed = WALK_SPEED_YARDS_PER_SECOND;
    //moving_to_start = false;

    if (!unit.isAlive() || unit.IsStopped())
        return;

    unit.StopMoving();
    unit.AddUnitState(UNIT_STATE_CONFUSED_MOVE);
}

template<class T>
void ConfusedMovementGenerator<T>::DoReset(T &unit)
{
    i_nextMoveTime.Reset(0);

    if (!unit.isAlive() || unit.IsStopped())
        return;

    unit.StopMoving();
    unit.AddUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_CONFUSED_MOVE);
}

template<class T>
bool ConfusedMovementGenerator<T>::DoUpdate(T &unit, const uint32 &diff)
{
    if (unit.HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED | UNIT_STATE_DISTRACTED))
        return true;

    //i_nextMoveTime.Update(diff);
    //if(i_duration >= diff)
    //   i_duration -= diff;

    if (i_nextMoveTime.Passed())
    {
        // currently moving, update location
        unit.AddUnitState(UNIT_STATE_CONFUSED_MOVE);

        if (unit.movespline->Finalized())
            i_nextMoveTime.Reset(urand(800, 1500));
    }
    else
    {
        // waiting for next move
        i_nextMoveTime.Update(diff);
        if (i_nextMoveTime.Passed())
        {
            // start moving
            unit.AddUnitState(UNIT_STATE_CONFUSED_MOVE);

            float dest = 4.0f * (float)rand_norm() - 2.0f;

            Position pos;
            pos.Relocate(i_x, i_y, i_z);
            unit.MovePositionToFirstCollision(pos, dest, 0.0f);

            PathGenerator path(&unit);
            path.SetPathLengthLimit(30.0f);
            bool result = path.CalculatePath(i_x, i_y, i_z);
            if (!result || (path.GetPathType() & PATHFIND_NOPATH))
            {
                i_nextMoveTime.Reset(100);
                return true;
            }

            Movement::MoveSplineInit init(unit);
            init.MovebyPath(path.GetPath());
            init.SetWalk(true);
            init.Launch();
        }
    }

    return true;
}

template<>
void ConfusedMovementGenerator<Player>::DoFinalize(Player &unit)
{
    unit.RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED);
    unit.ClearUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_CONFUSED_MOVE);
    unit.StopMoving();
}

template<>
void ConfusedMovementGenerator<Creature>::DoFinalize(Creature &unit)
{
    unit.RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED);
    unit.ClearUnitState(UNIT_STATE_CONFUSED | UNIT_STATE_CONFUSED_MOVE);
    if (unit.getVictim())
        unit.SetTarget(unit.getVictim()->GetGUID());
}

template void ConfusedMovementGenerator<Player>::DoInitialize(Player &player);
template void ConfusedMovementGenerator<Creature>::DoInitialize(Creature &creature);
template void ConfusedMovementGenerator<Player>::DoReset(Player &player);
template void ConfusedMovementGenerator<Creature>::DoReset(Creature &creature);
template bool ConfusedMovementGenerator<Player>::DoUpdate(Player &player, const uint32 &diff);
template bool ConfusedMovementGenerator<Creature>::DoUpdate(Creature &creature, const uint32 &diff);


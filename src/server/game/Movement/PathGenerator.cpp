/*
 * Copyright (C) 2008-2017 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "PathGenerator.h"
#include "Map.h"
#include "Creature.h"
#include "MMapFactory.h"
#include "MMapManager.h"
#include "Log.h"
#include "DisableMgr.h"
#include "DetourCommon.h"
#include "DetourNavMeshQuery.h"
#include <G3D/Vector3.h>


////////////////// PathGenerator //////////////////
PathGenerator::PathGenerator(const Unit* owner) : _charges(false), _polyLength(0), _type(PATHFIND_BLANK), _useStraightPath(false), _forceDestination(false), _stopOnlyOnEndPos(false), 
_pointPathLimit(MAX_POINT_PATH_LENGTH), _straightLine(false), _endPosition(G3D::Vector3::zero()), _transport(nullptr), _go(nullptr), _enableShort(true), _sourceUnit(owner), _navMesh(nullptr), _navMeshQuery(nullptr)
{
    memset(_pathPolyRefs, 0, sizeof(_pathPolyRefs));

    if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ PathGenerator::PathGenerator for %s", _sourceUnit->GetGUID().ToString().c_str());

    CreateFilter();
}

PathGenerator::~PathGenerator()
{
    if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ PathGenerator::~PathGenerator() for %s", _sourceUnit->GetGUID().ToString().c_str());

}

void PathGenerator::InitPath()
{
    _navMeshQuery = nullptr;
    _navMesh = nullptr;

    Map* map = _sourceUnit->GetMap();
    if (!map || map->IsMapUnload())
        return;

    uint32 mapId = _sourceUnit->GetMapId();
    if (DisableMgr::IsPathfindingEnabled(mapId))
    {
        auto mmap = MMAP::MMapFactory::createOrGetMMapManager();
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        {
            _transport = _sourceUnit->GetTransport();

            if (_transport)
                _navMeshQuery = mmap->GetModelNavMeshQuery(_transport->GetDisplayId(), GetThreadID());
            else
            {
                DynamicTreeCallback dCallback;
                float heightZ = map->GetHeight(_sourceUnit->GetPhases(), _sourceUnit->GetPositionX(), _sourceUnit->GetPositionY(), _sourceUnit->GetPositionZ(), true, DEFAULT_HEIGHT_SEARCH, &dCallback);
                if (dCallback.go/* && heightZ <= _sourceUnit->GetPositionZ() && (_sourceUnit->GetPositionZ() - heightZ) <= 2.3f*/) // Only if go under unit
                    _go = dCallback.go;

                if (_go)
                    _navMeshQuery = mmap->GetModelNavMeshQuery(_go->GetDisplayId(), GetThreadID());
            }
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "PathGenerator 0 displayId %i _transport %i _navMeshQuery %i _navMesh %i", _go ? _go->GetDisplayId() : 0, _transport ? _transport->GetDisplayId() : 0, bool(_navMeshQuery), bool(_navMesh));
        }


        if (!_navMeshQuery && !_transport) // If transport or go don`t have mesh disable it
        {
            _go = nullptr;
            _navMeshQuery = mmap->GetNavMeshQuery(mapId, GetThreadID());
        }

        if (_navMeshQuery)
            _navMesh = _navMeshQuery->getAttachedNavMesh();

        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "PathGenerator 1 displayId %i _transport %i _navMeshQuery %i _navMesh %i", _go ? _go->GetDisplayId() : 0, _transport ? _transport->GetDisplayId() : 0, bool(_navMeshQuery), bool(_navMesh));
    }
}

bool PathGenerator::CalculatePath(float destX, float destY, float destZ, bool forceDest, bool straightLine, bool stopOnlyOnEndPos)
{
    InitPath();

    // start debug info
    volatile uint32 displayId = _go ? _go->GetDisplayId() : 0;
    volatile uint32 goEntry = _go ? _go->GetEntry() : 0;
    // end debug info

    float x, y, z = 0.0f;
    _sourceUnit->GetPosition(x, y, z, _transport);

    // if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        // TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "CalculatePath ::0 startPos %f %f %f endPos %f %f %f", x, y, z, destX, destY, destZ);

    if (_transport)
        _transport->CalculatePassengerOffset(destX, destY, destZ);
    else if (_go)
    {
        _go->CalculatePassengerOffset(x, y, z);
        _go->CalculatePassengerOffset(destX, destY, destZ);
    }

    // if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        // TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "CalculatePath :: 1 startPos %f %f %f endPos %f %f %f", x, y, z, destX, destY, destZ);

    if (!Trinity::IsValidMapCoord(destX, destY, destZ) || !Trinity::IsValidMapCoord(x, y, z))
        return false;

    G3D::Vector3 dest(destX, destY, destZ);
    SetEndPosition(dest);

    G3D::Vector3 start(x, y, z);
    SetStartPosition(start);

    _forceDestination = forceDest;
    _straightLine = straightLine;
    _stopOnlyOnEndPos = stopOnlyOnEndPos;

    // if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        // TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ PathGenerator::CalculatePath() for %s _navMesh %u _navMeshQuery %u start %u dest %u",
            // _sourceUnit->GetGUID().ToString().c_str(), bool(_navMesh), bool(_navMeshQuery), HaveTile(start), HaveTile(dest));

    // make sure navMesh works - we can run on map w/o mmap
    // check if the start and end point have a .mmtile loaded (can we pass via not loaded tile on the way?)
    if (!_navMesh || !_navMeshQuery || _sourceUnit->HasUnitState(UNIT_STATE_IGNORE_PATHFINDING) || !HaveTile(start) || !HaveTile(dest))
    {
        BuildShortcut();
        _type = PathType(PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
        return true;
    }

    UpdateFilter();

    BuildPolyPath(start, dest);
    return true;
}

bool PathGenerator::CalculateShortcutPath(float destX, float destY, float destZ)
{
    InitPath();
    float x, y, z = 0.0f;
    _sourceUnit->GetPosition(x, y, z, _transport);

    if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "CalculatePath :: startPos %f %f %f endPos %f %f %f", x, y, z, destX, destY, destZ);

    /*if (_transport)
        _transport->CalculatePassengerOffset(destX, destY, destZ);
    else */if (_go)
    {
        _go->CalculatePassengerOffset(x, y, z);
        _go->CalculatePassengerOffset(destX, destY, destZ);
    }

    if (!Trinity::IsValidMapCoord(destX, destY, destZ) || !Trinity::IsValidMapCoord(x, y, z))
        return false;

    G3D::Vector3 dest(destX, destY, destZ);
    SetEndPosition(dest);

    G3D::Vector3 start(x, y, z);
    SetStartPosition(start);

    _forceDestination = true;
    _straightLine = false;

    // if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        // TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ PathGenerator::CalculateShortcutPath() for %s _navMesh %u _navMeshQuery %u start %u dest %u",
            // _sourceUnit->GetGUID().ToString().c_str(), bool(_navMesh), bool(_navMeshQuery), HaveTile(start), HaveTile(dest));

    BuildShortcut();
    _type = PathType(PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
    return true;
}

dtPolyRef PathGenerator::GetPathPolyByPosition(dtPolyRef const* polyPath, uint32 polyPathSize, float const* point, float* distance) const
{
    if (!polyPath || !polyPathSize)
        return INVALID_POLYREF;

    dtPolyRef nearestPoly = INVALID_POLYREF;
    float minDist = FLT_MAX;

    for (uint32 i = 0; i < polyPathSize; ++i)
    {
        float closestPoint[VERTEX_SIZE];
        if (dtStatusFailed(_navMeshQuery->closestPointOnPoly(polyPath[i], point, closestPoint, nullptr)))
            continue;

        float d = dtVdistSqr(point, closestPoint);
        if (d < minDist)
        {
            minDist = d;
            nearestPoly = polyPath[i];
        }

        if (minDist < 1.0f) // shortcut out - close enough for us
            break;
    }

    if (distance)
        *distance = dtMathSqrtf(minDist);

    return (minDist < 3.0f) ? nearestPoly : INVALID_POLYREF;
}

dtPolyRef PathGenerator::FindWalkPoly(dtNavMeshQuery const* query, float const* pointYZX, dtQueryFilter const& filter, float* closestPointYZX, float zSearchDist)
{
    ASSERT(query);

    // WARNING : Nav mesh coords are Y, Z, X (and not X, Y, Z)
    float extents[3] = {5.0f, zSearchDist, 5.0f};
    int polyCount = 0;
    dtPolyRef polyRef;

    // Default recastnavigation method
    if (dtStatusFailed(query->findNearestPoly(pointYZX, extents, &filter, &polyRef, closestPointYZX)))
        return 0;
    // Do not select points over player pos
    if (closestPointYZX[1] > pointYZX[1] + 3.0f)
        return 0;
    return polyRef;
}

dtPolyRef PathGenerator::GetPolyByLocation(float const* point, float* distance) const
{
    // first we check the current path
    // if the current path doesn't contain the current poly,
    // we need to use the expensive navMesh.findNearestPoly
    dtPolyRef polyRef = GetPathPolyByPosition(_pathPolyRefs, _polyLength, point, distance);
    if (polyRef != INVALID_POLYREF)
        return polyRef;

    // we don't have it in our old path
    // try to get it by findNearestPoly()
    // first try with low search box
    float extents[VERTEX_SIZE] = {3.0f, 5.0f, 3.0f};    // bounds of poly search area
    float closestPoint[VERTEX_SIZE] = {0.0f, 0.0f, 0.0f};
    if (dtStatusSucceed(_navMeshQuery->findNearestPoly(point, extents, &_filter, &polyRef, closestPoint)) && polyRef != INVALID_POLYREF)
    {
        *distance = dtVdist(closestPoint, point);
        return polyRef;
    }

    // still nothing ..
    // try with bigger search box
    // Note that the extent should not overlap more than 128 polygons in the navmesh (see dtNavMeshQuery::findNearestPoly)
    extents[1] = 50.0f;

    if (dtStatusSucceed(_navMeshQuery->findNearestPoly(point, extents, &_filter, &polyRef, closestPoint)) && polyRef != INVALID_POLYREF)
    {
        *distance = dtVdist(closestPoint, point);
        return polyRef;
    }

    return INVALID_POLYREF;
}

void PathGenerator::BuildPolyPath(G3D::Vector3 const& startPos, G3D::Vector3 const& endPos)
{
    // *** getting start/end poly logic ***

    float distToStartPoly, distToEndPoly;
    float startPoint[VERTEX_SIZE] = {startPos.y, startPos.z, startPos.x};
    float endPoint[VERTEX_SIZE] = {endPos.y, endPos.z, endPos.x};

    dtPolyRef startPoly = GetPolyByLocation(startPoint, &distToStartPoly);
    dtPolyRef endPoly = GetPolyByLocation(endPoint, &distToEndPoly);

    if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: startPos %f %f %f endPos %f %f %f", startPos.y, startPos.z, startPos.x, endPos.y, endPos.z, endPos.x);

    // we have a hole in our mesh
    // make shortcut path and mark it as NOPATH ( with flying and swimming exception )
    // its up to caller how he will use this info
    if (startPoly == INVALID_POLYREF || endPoly == INVALID_POLYREF)
    {
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: (startPoly == %u || endPoly == %u)", startPoly, endPoly);

        BuildShortcut();
        bool path = _sourceUnit->GetTypeId() == TYPEID_UNIT && _sourceUnit->ToCreature()->CanFly();

        bool waterPath = _sourceUnit->GetTypeId() == TYPEID_UNIT && _sourceUnit->ToCreature()->CanSwim();
        if (waterPath)
        {
            // Check both start and end points, if they're both in water, then we can *safely* let the creature move
            for (uint32 i = 0; i < _pathPoints.size(); ++i)
            {
                ZLiquidStatus status = _sourceUnit->GetMap()->getLiquidStatus(_pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z, MAP_ALL_LIQUIDS, nullptr);
                // One of the points is not in the water, cancel movement.
                if (status == LIQUID_MAP_NO_WATER)
                {
                    waterPath = false;
                    break;
                }
            }
        }

        _type = (path || waterPath) ? PathType(PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH) : PATHFIND_NOPATH;
        return;
    }

    // we may need a better number here
    bool farFromPoly = (distToStartPoly > 7.0f || distToEndPoly > 7.0f);
    if (farFromPoly)
    {
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: farFromPoly distToStartPoly=%.3f distToEndPoly=%.3f\n", distToStartPoly, distToEndPoly);

        bool buildShotrcut = false;
        if (_sourceUnit->GetTypeId() == TYPEID_UNIT)
        {
            Creature* owner = (Creature*)_sourceUnit;

            G3D::Vector3 const& p = (distToStartPoly > 7.0f) ? startPos : endPos;
            if (_sourceUnit->GetMap()->IsUnderWater(p))
            {
                if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
                    TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: underWater case\n");
                if (owner->CanSwim())
                    buildShotrcut = true;
            }
            else
            {
                if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
                    TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: flying case\n");
                if (owner->CanFly())
                    buildShotrcut = true;
            }
        }

        if (buildShotrcut)
        {
            BuildShortcut();
            _type = PathType(PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
            return;
        }
        float closestPoint[VERTEX_SIZE];
        // we may want to use closestPointOnPolyBoundary instead
        if (dtStatusSucceed(_navMeshQuery->closestPointOnPoly(endPoly, endPoint, closestPoint, nullptr)))
        {
            dtVcopy(endPoint, closestPoint);
            SetActualEndPosition(G3D::Vector3(endPoint[2], endPoint[0], endPoint[1]));
        }

        _type = PATHFIND_INCOMPLETE;
    }

    // *** poly path generating logic ***

    // start and end are on same polygon
    // just need to move in straight line
    if (startPoly == endPoly && !_stopOnlyOnEndPos)
    {
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: (startPoly == endPoly)\n");

        _pathPolyRefs[0] = startPoly;
        _polyLength = 1;

        if (_enableShort)
		    _type = farFromPoly ? PathType(PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY) : PATHFIND_NORMAL;
        else
            _type = PATHFIND_NOPATH;

        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: path type %d\n", _type);

		BuildPointPath(startPoint, endPoint);
        return;
    }

    // look for startPoly/endPoly in current path
    /// @todo we can merge it with getPathPolyByPosition() loop
    bool startPolyFound = false;
    bool endPolyFound = false;
    uint32 pathStartIndex = 0;
    uint32 pathEndIndex = 0;

    if (_polyLength)
    {
        for (; pathStartIndex < _polyLength; ++pathStartIndex)
        {
            // here to catch few bugs
            if (_pathPolyRefs[pathStartIndex] == INVALID_POLYREF)
            {
                TC_LOG_ERROR(LOG_FILTER_PATH_GENERATOR, "Invalid poly ref in BuildPolyPath. _polyLength: %u, pathStartIndex: %u," " startPos: %s, endPos: %s, mapid: %u", _polyLength, pathStartIndex, startPos.toString().c_str(), endPos.toString().c_str(), _sourceUnit->GetMapId());
                break;
            }

            if (_pathPolyRefs[pathStartIndex] == startPoly)
            {
                startPolyFound = true;
                break;
            }
        }

        for (pathEndIndex = _polyLength-1; pathEndIndex > pathStartIndex; --pathEndIndex)
            if (_pathPolyRefs[pathEndIndex] == endPoly)
            {
                endPolyFound = true;
                break;
            }
    }

    if (startPolyFound && endPolyFound)
    {
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: (startPolyFound && endPolyFound)\n");

        // we moved along the path and the target did not move out of our old poly-path
        // our path is a simple subpath case, we have all the data we need
        // just "cut" it out

        _polyLength = pathEndIndex - pathStartIndex + 1;
        memmove(_pathPolyRefs, _pathPolyRefs + pathStartIndex, _polyLength * sizeof(dtPolyRef));
    }
    else if (startPolyFound && !endPolyFound)
    {
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: (startPolyFound && !endPolyFound)\n");

        // we are moving on the old path but target moved out
        // so we have atleast part of poly-path ready

        _polyLength -= pathStartIndex;

        // try to adjust the suffix of the path instead of recalculating entire length
        // at given interval the target cannot get too far from its last location
        // thus we have less poly to cover
        // sub-path of optimal path is optimal

        // take ~80% of the original length
        /// @todo play with the values here
        uint32 prefixPolyLength = uint32(_polyLength * 0.8f + 0.5f);
        memmove(_pathPolyRefs, _pathPolyRefs+pathStartIndex, prefixPolyLength * sizeof(dtPolyRef));

        dtPolyRef suffixStartPoly = _pathPolyRefs[prefixPolyLength-1];

        // we need any point on our suffix start poly to generate poly-path, so we need last poly in prefix data
        float suffixEndPoint[VERTEX_SIZE];
        if (dtStatusFailed(_navMeshQuery->closestPointOnPoly(suffixStartPoly, endPoint, suffixEndPoint, nullptr)))
        {
            // we can hit offmesh connection as last poly - closestPointOnPoly() don't like that
            // try to recover by using prev polyref
            --prefixPolyLength;
            suffixStartPoly = _pathPolyRefs[prefixPolyLength-1];
            if (dtStatusFailed(_navMeshQuery->closestPointOnPoly(suffixStartPoly, endPoint, suffixEndPoint, nullptr)))
            {
                // suffixStartPoly is still invalid, error state
                BuildShortcut();
                _type = PATHFIND_NOPATH;
                return;
            }
        }

        // generate suffix
        uint32 suffixPolyLength = 0;

        dtStatus dtResult;
        if (_straightLine)
        {
            float hit = 0;
            float hitNormal[3];
            memset(hitNormal, 0, sizeof(hitNormal));

            dtResult = _navMeshQuery->raycast(suffixStartPoly, suffixEndPoint, endPoint, &_filter, &hit, hitNormal, _pathPolyRefs + prefixPolyLength - 1, reinterpret_cast<int*>(&suffixPolyLength), MAX_PATH_LENGTH - prefixPolyLength);

            // raycast() sets hit to FLT_MAX if there is a ray between start and end
            if (hit != FLT_MAX)
            {
                // the ray hit something, return no path instead of the incomplete one
                _type = PATHFIND_NOPATH;
                return;
            }
        }
        else
            dtResult = _navMeshQuery->findPath( suffixStartPoly, endPoly, suffixEndPoint, endPoint, &_filter, _pathPolyRefs + prefixPolyLength - 1, reinterpret_cast<int*>(&suffixPolyLength), MAX_PATH_LENGTH - prefixPolyLength);

        if (!suffixPolyLength || dtStatusFailed(dtResult))
        {
            // this is probably an error state, but we'll leave it
            // and hopefully recover on the next Update
            // we still need to copy our preffix
            TC_LOG_ERROR(LOG_FILTER_PATH_GENERATOR, "%s's Path Build failed: 0 length path", _sourceUnit->GetGUID().ToString().c_str());
        }

        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++  m_polyLength=%u prefixPolyLength=%u suffixPolyLength=%u \n", _polyLength, prefixPolyLength, suffixPolyLength);

        // new path = prefix + suffix - overlap
        _polyLength = prefixPolyLength + suffixPolyLength - 1;
    }
    else
    {
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildPolyPath :: (!startPolyFound && !endPolyFound)\n");

        // either we have no path at all -> first run
        // or something went really wrong -> we aren't moving along the path to the target
        // just generate new path

        // free and invalidate old path data
        Clear();

        dtStatus dtResult;
        if (_straightLine)
        {
            float hit = 0;
            float hitNormal[3];
            memset(hitNormal, 0, sizeof(hitNormal));

            dtResult = _navMeshQuery->raycast( startPoly, startPoint, endPoint, &_filter, &hit, hitNormal, _pathPolyRefs, reinterpret_cast<int*>(&_polyLength), MAX_PATH_LENGTH);

            // raycast() sets hit to FLT_MAX if there is a ray between start and end
            if (hit != FLT_MAX)
            {
                // the ray hit something, return no path instead of the incomplete one
                Clear();
                _polyLength = 2;
                _pathPoints.resize(2);
                _pathPoints[0] = GetStartPosition();
                float hitPos[3];
                dtVlerp(hitPos, startPoint, endPoint, hit);
                _pathPoints[1] = G3D::Vector3(hitPos[2], hitPos[0], hitPos[1]);

                _type = PATHFIND_NOPATH;
                return;
            }
            else
                _navMeshQuery->getPolyHeight(_pathPolyRefs[_polyLength - 1], endPoint, &endPoint[1]);
        }
        else
		{
            dtResult = _navMeshQuery->findPath(
                            startPoly,          // start polygon
                            endPoly,            // end polygon
                            startPoint,         // start position
                            endPoint,           // end position
                            &_filter,           // polygon search filter
                            _pathPolyRefs,     // [out] path
							reinterpret_cast<int*>(&_polyLength),
                            MAX_PATH_LENGTH);   // max number of polygons in output path
		}

        if (!_polyLength || dtStatusFailed(dtResult))
        {
            // only happens if we passed bad data to findPath(), or navmesh is messed up
            TC_LOG_ERROR(LOG_FILTER_PATH_GENERATOR, "%s's Path Build failed: 0 length path", _sourceUnit->GetGUID().ToString().c_str());
            BuildShortcut();
            _type = PATHFIND_NOPATH;
            return;
        }
    }

    // by now we know what type of path we can get
    if (_pathPolyRefs[_polyLength - 1] == endPoly && !(_type & PATHFIND_INCOMPLETE))
        _type = PATHFIND_NORMAL;
    else
        _type = PATHFIND_INCOMPLETE;

    if (_type & PATHFIND_INCOMPLETE && _sourceUnit->CanFly())
    {
        BuildShortcut();
        _type = PathType(PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH);
        return;
    }

    if (farFromPoly)
        _type = PathType(_type | PATHFIND_FARFROMPOLY);

    // generate the point-path out of our up-to-date poly-path
    BuildPointPath(startPoint, endPoint);
}

void PathGenerator::BuildPointPath(const float *startPoint, const float *endPoint)
{
    float pathPoints[MAX_POINT_PATH_LENGTH*VERTEX_SIZE];
    uint32 pointCount = 0;
    dtStatus dtResult = DT_FAILURE;
    if (!_navMeshQuery)
    {
        BuildShortcut();
        _type = PATHFIND_NOPATH;
        return;
    }

    if (_straightLine)
    {
        dtResult = DT_SUCCESS;
        pointCount = 1;
        memcpy(&pathPoints[VERTEX_SIZE * 0], startPoint, sizeof(float)* 3); // first point

        float stepSize = _charges ? CHARGES_PATH_STEP_SIZE : SMOOTH_PATH_STEP_SIZE;
        // path has to be split into polygons with dist SMOOTH_PATH_STEP_SIZE between them
        G3D::Vector3 startVec = G3D::Vector3(startPoint[0], startPoint[1], startPoint[2]);
        G3D::Vector3 endVec = G3D::Vector3(endPoint[0], endPoint[1], endPoint[2]);
        G3D::Vector3 diffVec = (endVec - startVec);
        G3D::Vector3 prevVec = startVec;
        float len = diffVec.length();
        diffVec *= stepSize / len;

        // If the path is short PATHFIND_SHORT will be set as type
        while (len > stepSize && pointCount < MAX_POINT_PATH_LENGTH)
        {
            len -= stepSize;
            prevVec += diffVec;
            pathPoints[VERTEX_SIZE * pointCount + 0] = prevVec.x;
            pathPoints[VERTEX_SIZE * pointCount + 1] = prevVec.y;
            pathPoints[VERTEX_SIZE * pointCount + 2] = prevVec.z;
            ++pointCount;
        }

        // If the path is short PATHFIND_SHORT will be set as type
        if (pointCount < MAX_POINT_PATH_LENGTH)
        {
            memcpy(&pathPoints[VERTEX_SIZE * pointCount], endPoint, sizeof(float)* 3); // last point
            ++pointCount;
		}
    }
    else if (_useStraightPath)
        dtResult = _navMeshQuery->findStraightPath(startPoint, endPoint, _pathPolyRefs, _polyLength, pathPoints, nullptr, nullptr, reinterpret_cast<int*>(&pointCount), _pointPathLimit);
    else
        dtResult = FindSmoothPath( startPoint, endPoint, _pathPolyRefs, _polyLength, pathPoints, reinterpret_cast<int*>(&pointCount), _pointPathLimit);

    // Special case with start and end positions very close to each other
    if (_polyLength == 1 && pointCount == 1)
    {
        // First point is start position, append end position
        dtVcopy(&pathPoints[1 * VERTEX_SIZE], endPoint);
        pointCount++;
    }
    else if ( pointCount < 2 || dtStatusFailed(dtResult))
    {
        // only happens if pass bad data to findStraightPath or navmesh is broken
        // single point paths can be generated here
        /// @todo check the exact cases
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ PathGenerator::BuildPointPath FAILED! path sized %d returned\n", pointCount);
        BuildShortcut();
        _type = PATHFIND_NOPATH;
        return;
    }
    else if (pointCount >= _pointPathLimit)
    {
        if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ PathGenerator::BuildPointPath FAILED! path sized %d returned, lower than limit set to %d\n", pointCount, _pointPathLimit);
        BuildShortcut();
        _type = PATHFIND_SHORT;
        return;
    }

    _pathPoints.resize(pointCount);
    for (uint32 i = 0; i < pointCount; ++i)
        _pathPoints[i] = G3D::Vector3(pathPoints[i*VERTEX_SIZE+2], pathPoints[i*VERTEX_SIZE], pathPoints[i*VERTEX_SIZE+1]);

    NormalizePath();
    pointCount = _pathPoints.size(); // Update after recalculate

    // first point is always our current location - we need the next one
    SetActualEndPosition(_pathPoints[pointCount-1]);

    if (_stopOnlyOnEndPos && !InRange(GetEndPosition(), GetActualEndPosition(), SMOOTH_PATH_SLOP, 1.f))
    {
        _type = PathType(PATHFIND_INCOMPLETE);
    }

    // force the given destination, if needed
    if (_forceDestination &&
        (!(_type & PATHFIND_NORMAL) || !InRange(GetEndPosition(), GetActualEndPosition(), 1.0f, 1.0f)))
    {
        // we may want to keep partial subpath
        if (Dist3DSqr(GetActualEndPosition(), GetEndPosition()) < 0.3f * Dist3DSqr(GetStartPosition(), GetEndPosition()))
        {
            SetActualEndPosition(GetEndPosition());
            _pathPoints[_pathPoints.size()-1] = GetEndPosition();
        }
        else
        {
            SetActualEndPosition(GetEndPosition());
            BuildShortcut();
        }

        _type = PathType(PATHFIND_NORMAL | PATHFIND_NOT_USING_PATH | PATHFIND_FARFROMPOLY);
    }

    if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ PathGenerator::BuildPointPath path type %d size %d poly-size %d _forceDestination %u", _type, pointCount, _polyLength, _forceDestination);
}

void PathGenerator::SetStartPosition(G3D::Vector3 const& point)
{
    _startPosition = point;
}

void PathGenerator::SetEndPosition(G3D::Vector3 const& point)
{
    _actualEndPosition = point;
    _endPosition = point;
}

void PathGenerator::SetActualEndPosition(G3D::Vector3 const& point)
{
    _actualEndPosition = point;
}

void PathGenerator::NormalizePath()
{
    Map* map = _sourceUnit->GetMap();
    if (!map || map->IsMapUnload())
        return;

    Movement::PointsArray _newPathPoints;
    float unused = 0.0f;
    if (_go)
        for (uint32 i = 0; i < _pathPoints.size(); ++i)
            _go->CalculatePassengerPosition(_pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z);

    for (uint32 i = 0; i < _pathPoints.size(); ++i)
        _sourceUnit->UpdateAllowedPositionZ(_pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z);

    if (!_sourceUnit->IsInWorld())
        return;

    if (!_sourceUnit->IsPlayer() && !_sourceUnit->isSummon())
        return;

    for (uint32 i = 0, next = 1; next < _pathPoints.size(); ++i, ++next)
    {
        float nextPoint[VERTEX_SIZE], lastPoint[VERTEX_SIZE];
        nextPoint[0] = _pathPoints[next].x;
        nextPoint[1] = _pathPoints[next].y;
        nextPoint[2] = _pathPoints[next].z;
        lastPoint[0] = _pathPoints[i].x;
        lastPoint[1] = _pathPoints[i].y;
        lastPoint[2] = _pathPoints[i].z;
        // ((WorldObject*)_sourceUnit)->SummonCreature(44548, lastPoint[0], lastPoint[1], lastPoint[2], _sourceUnit->GetOrientation(), TEMPSUMMON_TIMED_DESPAWN, 20000); // For visual point test
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "PathGenerator::NormalizePath next %u _pathPoints %u vector %f %f %f dist %f 2ddist %f", next, i, _pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z, dtVdist(lastPoint, nextPoint), dtVdist2D(lastPoint, nextPoint));
    }

    TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "PathGenerator::NormalizePath _pathPoints %u", _pathPoints.size());

    return; // Now disable, need fix calculation

    uint32 counter = 0;
    uint32 next = 1;
    for (uint32 i = 0; next < _pathPoints.size(); ++i, ++next)
    {
        _sourceUnit->UpdateAllowedPositionZ(_pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z);
        _newPathPoints.push_back(_pathPoints[i]);
        bool result = map->isInLineOfSight(_pathPoints[next].x, _pathPoints[next].y, _pathPoints[next].z + 0.5f, _pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z + 0.5f, _sourceUnit->GetPhases());

        float delta[VERTEX_SIZE], moveTgt[VERTEX_SIZE], moveCur[VERTEX_SIZE], nextPoint[VERTEX_SIZE], lastPoint[VERTEX_SIZE];
        nextPoint[0] = _pathPoints[next].x;
        nextPoint[1] = _pathPoints[next].y;
        nextPoint[2] = _pathPoints[next].z;
        lastPoint[0] = _pathPoints[i].x;
        lastPoint[1] = _pathPoints[i].y;
        lastPoint[2] = _pathPoints[i].z;
        dtVcopy(moveCur, nextPoint);

        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "PathGenerator::NormalizePath next %u _pathPoints %u vector %f %f %f dist %f 2ddist %f", next, i, _pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z, dtVdist(lastPoint, nextPoint), dtVdist2D(lastPoint, nextPoint));

        while (!result)
        {
            counter++;
            if (dtVdist(lastPoint, nextPoint) <= 2.0f)
                break;

            dtVsub(delta, lastPoint, moveCur);
            delta[0] /= 2;
            delta[1] /= 2;
            delta[2] /= 2;

            dtVadd(moveTgt, lastPoint, delta);

            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "NormalizePath 0 %f %f %f moveCur %f %f %f", lastPoint[0], lastPoint[1], lastPoint[2], moveCur[0], moveCur[1], moveCur[2]);

            if (dtVdist(lastPoint, moveTgt) <= 2.0f) // min distance 2m for divide patch
            {
                _sourceUnit->UpdateAllowedPositionZ(moveTgt[0], moveTgt[1], moveTgt[2]);
                _newPathPoints.push_back(G3D::Vector3{moveTgt[0], moveTgt[1], moveTgt[2]});
                dtVcopy(lastPoint, moveTgt);
                dtVcopy(moveCur, nextPoint);
                TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "NormalizePath 0 push_back %u vector %f %f %f dist %f 2ddist %f", _newPathPoints.size(), moveTgt[0], moveTgt[1], moveTgt[2], dtVdist(lastPoint, moveCur), dtVdist2D(lastPoint, moveCur));
                break;
            }
            result = map->isInLineOfSight(lastPoint[0], lastPoint[1], lastPoint[2] + 0.5f, moveTgt[0], moveTgt[1], moveTgt[2] + 0.5f, _sourceUnit->GetPhases());
            if (result)
            {
                if (dtVdist(moveCur, nextPoint) > 2.0f)
                    result = false;

                dtVcopy(lastPoint, moveTgt);
                dtVcopy(moveCur, nextPoint);
                _sourceUnit->UpdateAllowedPositionZ(moveTgt[0], moveTgt[1], moveTgt[2]);
                _newPathPoints.push_back(G3D::Vector3{moveTgt[0], moveTgt[1], moveTgt[2]});

                TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "NormalizePath 1 push_back %u vector %f %f %f dist %f 2ddist %f", _newPathPoints.size(), moveTgt[0], moveTgt[1], moveTgt[2], dtVdist(lastPoint, moveCur), dtVdist2D(lastPoint, moveCur));

                if (!result)
                    result = map->isInLineOfSight(lastPoint[0], lastPoint[1], lastPoint[2] + 0.5f, moveCur[0], moveCur[1], moveCur[2] + 0.5f, _sourceUnit->GetPhases());
            }
            else
                dtVcopy(moveCur, moveTgt);

            TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "NormalizePath 1 %f %f %f moveCur %f %f %f", lastPoint[0], lastPoint[1], lastPoint[2], moveCur[0], moveCur[1], moveCur[2]);
        }
    }
    _newPathPoints.push_back(_pathPoints[_pathPoints.size()-1]);

    Clear();
    _pathPoints = _newPathPoints;

    for (uint32 i = 0; i < _pathPoints.size(); ++i)
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "NormalizePath _pathPoints %u vector %f %f %f", i, _pathPoints[i].x, _pathPoints[i].y, _pathPoints[i].z);

    TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "PathGenerator::NormalizePath end _pathPoints %u counter %u", _pathPoints.size(), counter);
}

void PathGenerator::Clear()
{
    _polyLength = 0;
    _pathPoints.clear();
}

void PathGenerator::BuildShortcut()
{
    if (!_enableShort)
        return;

    if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        TC_LOG_DEBUG(LOG_FILTER_PATH_GENERATOR, "++ BuildShortcut :: making shortcut\n");

    Clear();

    // make two point path, our curr pos is the start, and dest is the end
    _pathPoints.resize(2);

    // set start and a default next position
    _pathPoints[0] = GetStartPosition();
    _pathPoints[1] = GetActualEndPosition();

    NormalizePath();

    _type = PATHFIND_SHORTCUT;
}

void PathGenerator::CreateFilter()
{
    uint16 includeFlags = 0;
    uint16 excludeFlags = 0;

    if (_sourceUnit->GetTypeId() == TYPEID_UNIT)
    {
        Creature* creature = (Creature*)_sourceUnit;
        if (creature->CanWalk())
            includeFlags |= NAV_GROUND;          // walk

        // creatures don't take environmental damage
        if (creature->CanSwim())
            includeFlags |= (NAV_WATER | NAV_MAGMA_SLIME);           // swim
    }
    else // assume Player
    {
        // perfect support not possible, just stay 'safe'
        includeFlags |= (NAV_GROUND | NAV_WATER | NAV_MAGMA_SLIME);
    }

    _filter.setIncludeFlags(includeFlags);
    _filter.setExcludeFlags(excludeFlags);

    UpdateFilter();
}

void PathGenerator::UpdateFilter()
{
    // allow creatures to cheat and use different movement types if they are moved
    // forcefully into terrain they can't normally move in
    if (_sourceUnit->IsInWater() || _sourceUnit->IsUnderWater())
    {
        uint16 includedFlags = _filter.getIncludeFlags();
        includedFlags |= GetNavTerrain(_sourceUnit->GetPositionX(), _sourceUnit->GetPositionY(),_sourceUnit->GetPositionZ());

        _filter.setIncludeFlags(includedFlags);
    }
}

NavTerrainFlag PathGenerator::GetNavTerrain(float x, float y, float z)
{
    LiquidData data;
    ZLiquidStatus liquidStatus = _sourceUnit->GetMap()->getLiquidStatus(x, y, z, MAP_ALL_LIQUIDS, &data);
    if (liquidStatus == LIQUID_MAP_NO_WATER)
        return NAV_GROUND;

    data.type_flags &= ~MAP_LIQUID_TYPE_DARK_WATER;
    switch (data.type_flags)
    {
        case MAP_LIQUID_TYPE_WATER:
        case MAP_LIQUID_TYPE_OCEAN:
            return NAV_WATER;
        case MAP_LIQUID_TYPE_MAGMA:
        case MAP_LIQUID_TYPE_SLIME:
            return NAV_MAGMA_SLIME;
        default:
            return NAV_GROUND;
    }
}

bool PathGenerator::HaveTile(const G3D::Vector3& p) const
{
    if (_go || _transport)
        return true;

    int tx = -1, ty = -1;
    float point[VERTEX_SIZE] = {p.y, p.z, p.x};

    _navMesh->calcTileLoc(point, &tx, &ty);

    if (_sourceUnit->IsPlayer() || _sourceUnit->isSummon())
        TC_LOG_ERROR(LOG_FILTER_PATH_GENERATOR, "++ HaveTile tx %i ty %i p %f %f %f",tx, ty, p.y, p.z, p.x);

    /// Workaround
    /// For some reason, often the tx and ty variables wont get a valid value
    /// Use this check to prevent getting negative tile coords and crashing on getTileAt
    if (tx < 0 || ty < 0)
        return false;

    return (_navMesh->getTileAt(tx, ty, 0) != nullptr);
}

uint32 PathGenerator::FixupCorridor(dtPolyRef* path, uint32 npath, uint32 maxPath, dtPolyRef const* visited, uint32 nvisited)
{
    int32 furthestPath = -1;
    int32 furthestVisited = -1;

    // Find furthest common polygon.
    for (int32 i = npath-1; i >= 0; --i)
    {
        bool found = false;
        for (int32 j = nvisited-1; j >= 0; --j)
        {
            if (path[i] == visited[j])
            {
                furthestPath = i;
                furthestVisited = j;
                found = true;
            }
        }
        if (found)
            break;
    }

    // If no intersection found just return current path.
    if (furthestPath == -1 || furthestVisited == -1)
        return npath;

    // Concatenate paths.

    // Adjust beginning of the buffer to include the visited.
    uint32 req = nvisited - furthestVisited;
    uint32 orig = uint32(furthestPath + 1) < npath ? furthestPath + 1 : npath;
    uint32 size = npath > orig ? npath - orig : 0;
    if (req + size > maxPath)
        size = maxPath-req;

    if (size)
        memmove(path + req, path + orig, size * sizeof(dtPolyRef));

    // Store visited
    for (uint32 i = 0; i < req; ++i)
        path[i] = visited[(nvisited - 1) - i];

    return req+size;
}

bool PathGenerator::GetSteerTarget(float const* startPos, float const* endPos, float minTargetDist, dtPolyRef const* path, uint32 pathSize, float* steerPos, unsigned char& steerPosFlag, dtPolyRef& steerPosRef)
{
    // Find steer target.
    static const uint32 MAX_STEER_POINTS = 3;
    float steerPath[MAX_STEER_POINTS*VERTEX_SIZE];
    unsigned char steerPathFlags[MAX_STEER_POINTS];
    dtPolyRef steerPathPolys[MAX_STEER_POINTS];
    uint32 nsteerPath = 0;
    dtStatus dtResult = _navMeshQuery->findStraightPath(startPos, endPos, path, pathSize, steerPath, steerPathFlags, steerPathPolys, reinterpret_cast<int*>(&nsteerPath), MAX_STEER_POINTS);
    if (!nsteerPath || dtStatusFailed(dtResult))
        return false;

    // Find vertex far enough to steer to.
    uint32 ns = 0;
    while (ns < nsteerPath)
    {
        // Stop at Off-Mesh link or when point is further than slop away.
        if ((steerPathFlags[ns] & DT_STRAIGHTPATH_OFFMESH_CONNECTION) || !InRangeYZX(&steerPath[ns*VERTEX_SIZE], startPos, minTargetDist, 1000.0f))
            break;
        ns++;
    }
    // Failed to find good point to steer to.
    if (ns >= nsteerPath)
        return false;

    dtVcopy(steerPos, &steerPath[ns*VERTEX_SIZE]);
    steerPos[1] = startPos[1];  // keep Z value
    steerPosFlag = steerPathFlags[ns];
    steerPosRef = steerPathPolys[ns];

    return true;
}

dtStatus PathGenerator::FindSmoothPath(float const* startPos, float const* endPos, dtPolyRef const* polyPath, uint32 polyPathSize, float* smoothPath, int* smoothPathSize, uint32 maxSmoothPathSize)
{
    if (!_navMeshQuery)
        return DT_FAILURE;

    *smoothPathSize = 0;
    uint32 nsmoothPath = 0;

    dtPolyRef polys[MAX_PATH_LENGTH];
    memcpy(polys, polyPath, sizeof(dtPolyRef)*polyPathSize);
    uint32 npolys = polyPathSize;

    float iterPos[VERTEX_SIZE], targetPos[VERTEX_SIZE];

    if (polyPathSize > 1)
    {
        // Pick the closest poitns on poly border
        if (dtStatusFailed(_navMeshQuery->closestPointOnPolyBoundary(polys[0], startPos, iterPos)))
            return DT_FAILURE;

        if (dtStatusFailed(_navMeshQuery->closestPointOnPolyBoundary(polys[npolys - 1], endPos, targetPos)))
            return DT_FAILURE;
    }
    else
    {
        // Case where the path is on the same poly
        dtVcopy(iterPos, startPos);
        dtVcopy(targetPos, endPos);
    }

    dtVcopy(&smoothPath[nsmoothPath*VERTEX_SIZE], iterPos);
    nsmoothPath++;

    float stepSize = _charges ? CHARGES_PATH_STEP_SIZE : SMOOTH_PATH_STEP_SIZE;
    // Move towards target a small advancement at a time until target reached or
    // when ran out of memory to store the path.
    while (npolys && nsmoothPath < maxSmoothPathSize)
    {
        // Find location to steer towards.
        float steerPos[VERTEX_SIZE];
        unsigned char steerPosFlag;
        dtPolyRef steerPosRef = INVALID_POLYREF;

        if (!GetSteerTarget(iterPos, targetPos, SMOOTH_PATH_SLOP, polys, npolys, steerPos, steerPosFlag, steerPosRef))
            break;

        bool endOfPath = (steerPosFlag & DT_STRAIGHTPATH_END) != 0;
        bool offMeshConnection = (steerPosFlag & DT_STRAIGHTPATH_OFFMESH_CONNECTION) != 0;

        // Find movement delta.
        float delta[VERTEX_SIZE];
        dtVsub(delta, steerPos, iterPos);
        float len = dtMathSqrtf(dtVdot(delta, delta));
        // If the steer target is end of path or off-mesh link, do not move past the location.
        if ((endOfPath || offMeshConnection) && len < stepSize)
            len = 1.0f;
        else
            len = stepSize / len;

        float moveTgt[VERTEX_SIZE];
        dtVmad(moveTgt, iterPos, delta, len);

        // Move
        float result[VERTEX_SIZE];
        const static uint32 MAX_VISIT_POLY = 16;
        dtPolyRef visited[MAX_VISIT_POLY];

        uint32 nvisited = 0;
        _navMeshQuery->moveAlongSurface(polys[0], iterPos, moveTgt, &_filter, result, visited, reinterpret_cast<int*>(&nvisited), MAX_VISIT_POLY);
        npolys = FixupCorridor(polys, npolys, MAX_PATH_LENGTH, visited, nvisited);

        _navMeshQuery->getPolyHeight(polys[0], result, &result[1]);
        result[1] += 0.5f;
        dtVcopy(iterPos, result);

        // Handle end of path and off-mesh links when close enough.
        if (endOfPath && InRangeYZX(iterPos, steerPos, SMOOTH_PATH_SLOP, 1.0f))
        {
            // Reached end of path.
            if (_stopOnlyOnEndPos)
            {
                if (InRangeYZX(targetPos, steerPos, SMOOTH_PATH_SLOP, 0.1f))
                {
                    dtVcopy(iterPos, targetPos);
                }
            }
            else
            {
                dtVcopy(iterPos, targetPos);
            }

            if (nsmoothPath < maxSmoothPathSize)
            {
                dtVcopy(&smoothPath[nsmoothPath*VERTEX_SIZE], iterPos);
                nsmoothPath++;
            }
            break;
        }
        if (offMeshConnection && InRangeYZX(iterPos, steerPos, SMOOTH_PATH_SLOP, 1.0f))
        {
            // Advance the path up to and over the off-mesh connection.
            dtPolyRef prevRef = INVALID_POLYREF;
            dtPolyRef polyRef = polys[0];
            uint32 npos = 0;
            while (npos < npolys && polyRef != steerPosRef)
            {
                prevRef = polyRef;
                polyRef = polys[npos];
                npos++;
            }

            for (uint32 i = npos; i < npolys; ++i)
                polys[i-npos] = polys[i];

            npolys -= npos;

            // Handle the connection.
            float connectionStartPos[VERTEX_SIZE], connectionEndPos[VERTEX_SIZE];
            if (dtStatusSucceed(_navMesh->getOffMeshConnectionPolyEndPoints(prevRef, polyRef, connectionStartPos, connectionEndPos)))
            {
                if (nsmoothPath < maxSmoothPathSize)
                {
                    dtVcopy(&smoothPath[nsmoothPath*VERTEX_SIZE], connectionStartPos);
                    nsmoothPath++;
                }
                // Move position at the other side of the off-mesh link.
                dtVcopy(iterPos, connectionEndPos);
                _navMeshQuery->getPolyHeight(polys[0], iterPos, &iterPos[1]);
                iterPos[1] += 0.5f;
            }
        }

        // Store results.
        if (nsmoothPath < maxSmoothPathSize)
        {
            dtVcopy(&smoothPath[nsmoothPath*VERTEX_SIZE], iterPos);
            nsmoothPath++;
        }
    }

    *smoothPathSize = nsmoothPath;

    // this is most likely a loop
    return nsmoothPath < MAX_POINT_PATH_LENGTH ? DT_SUCCESS : DT_FAILURE;
}

bool PathGenerator::InRangeYZX(const float* v1, const float* v2, float r, float h) const
{
    const float dx = v2[0] - v1[0];
    const float dy = v2[1] - v1[1]; // elevation
    const float dz = v2[2] - v1[2];
    return (dx * dx + dz * dz) < r * r && fabsf(dy) < h;
}

bool PathGenerator::InRange(G3D::Vector3 const& p1, G3D::Vector3 const& p2, float r, float h) const
{
    G3D::Vector3 d = p1 - p2;
    return (d.x * d.x + d.y * d.y) < r * r && fabsf(d.z) < h;
}

float PathGenerator::Dist3DSqr(G3D::Vector3 const& p1, G3D::Vector3 const& p2) const
{
    return (p1 - p2).squaredLength();
}

void PathGenerator::ReducePathLenghtByDist(float dist)
{
    if (GetPathType() == PATHFIND_BLANK)
    {
        TC_LOG_ERROR(LOG_FILTER_PATH_GENERATOR, "PathGenerator::ReducePathLenghtByDist called before path was built");
        return;
    }

    if (_pathPoints.size() < 2) // path building failure
        return;

    uint32 i = _pathPoints.size();
    G3D::Vector3 nextVec = _pathPoints[--i];
    while (i > 0)
    {
        G3D::Vector3 currVec = _pathPoints[--i];
        G3D::Vector3 diffVec = (nextVec - currVec);
        float len = diffVec.length();
        if (len > dist)
        {
            float step = dist / len;
            // same as nextVec
            _pathPoints[i + 1] -= diffVec * step;
            _sourceUnit->UpdateAllowedPositionZ(_pathPoints[i + 1].x, _pathPoints[i + 1].y, _pathPoints[i + 1].z);
            _pathPoints.resize(i + 2);
            break;
        }
        if (i == 0) // at second point
        {
            _pathPoints[1] = _pathPoints[0];
            _pathPoints.resize(2);
            break;
        }

        dist -= len;
        nextVec = currVec; // we're going backwards
    }
}

float PathGenerator::GetTotalLength() const
{
    float len = 0.0f;
    if (_pathPoints.size() < 2)
        return len;

    for (uint32 i = 1; i < _pathPoints.size() - 1; ++i)
    {
        G3D::Vector3 node = _pathPoints[i];
        G3D::Vector3 prev = _pathPoints[i - 1];
        float xd = node.x - prev.x;
        float yd = node.y - prev.y;
        float zd = node.z - prev.z;
        len += sqrtf(xd * xd + yd * yd + zd * zd);
    }

    return len;
}

bool PathGenerator::IsInvalidDestinationZ(Unit const* target) const
{
    return (target->GetPositionZ() - GetActualEndPosition().z) > 5.0f;
}

void PathGenerator::SetUseStraightPath(bool useStraightPath)
{
    _useStraightPath = useStraightPath;
}

void PathGenerator::SetPathLengthLimit(float distance)
{
    _pointPathLimit = std::min<uint32>(uint32(distance / SMOOTH_PATH_STEP_SIZE), MAX_POINT_PATH_LENGTH);
}

G3D::Vector3 const& PathGenerator::GetStartPosition() const
{
    return _startPosition;
}

G3D::Vector3 const& PathGenerator::GetEndPosition() const
{
    return _endPosition;
}

G3D::Vector3 const& PathGenerator::GetActualEndPosition() const
{
    return _actualEndPosition;
}

Movement::PointsArray const& PathGenerator::GetPath() const
{
    return _pathPoints;
}

PathType PathGenerator::GetPathType() const
{
    return _type;
}

void PathGenerator::SetShortPatch(bool enable)
{
    _enableShort = enable;
}

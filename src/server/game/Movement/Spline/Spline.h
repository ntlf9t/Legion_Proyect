/*
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

#ifndef TRINITYSERVER_SPLINE_H
#define TRINITYSERVER_SPLINE_H

#include "MovementTypedefs.h"
#include <limits>

namespace Movement {

class SplineBase
{
public:
    typedef int index_type;
    typedef std::vector<G3D::Vector3> ControlArray;

    enum EvaluationMode
    {
        ModeLinear,
        ModeCatmullrom,
        ModeBezier3_Unused,
        UninitializedMode,
        ModesEnd
    };

protected:
    ControlArray points;

    index_type index_lo;
    index_type index_hi;

    uint8 m_mode;
    bool cyclic;

    enum{
        // could be modified, affects segment length evaluation precision
        // lesser value saves more performance in cost of lover precision
        // minimal value is 1
        // client's value is 20, blizzs use 2-3 steps to compute length
        STEPS_PER_SEGMENT = 3,
    };
    static_assert(STEPS_PER_SEGMENT > 0, "shouldn't be lesser than 1");

    void EvaluateLinear(index_type, float, G3D::Vector3&) const;
    void EvaluateCatmullRom(index_type, float, G3D::Vector3&) const;
    void EvaluateBezier3(index_type, float, G3D::Vector3&) const;
    typedef void (SplineBase::*EvaluationMethtod)(index_type,float, G3D::Vector3&) const;
    static EvaluationMethtod evaluators[ModesEnd];

    void EvaluateDerivativeLinear(index_type, float, G3D::Vector3&) const;
    void EvaluateDerivativeCatmullRom(index_type, float, G3D::Vector3&) const;
    void EvaluateDerivativeBezier3(index_type, float, G3D::Vector3&) const;
    static EvaluationMethtod derivative_evaluators[ModesEnd];

    float SegLengthLinear(index_type) const;
    float SegLengthCatmullRom(index_type) const;
    float SegLengthBezier3(index_type) const;
    typedef float (SplineBase::*SegLenghtMethtod)(index_type) const;
    static SegLenghtMethtod seglengths[ModesEnd];

    void InitLinear(const G3D::Vector3*, index_type, bool, index_type);
    void InitCatmullRom(const G3D::Vector3*, index_type, bool, index_type);
    void InitBezier3(const G3D::Vector3*, index_type, bool, index_type);
    typedef void (SplineBase::*InitMethtod)(const G3D::Vector3*, index_type, bool, index_type);
    static InitMethtod initializers[ModesEnd];

    void UninitializedSpline() const { ASSERT(false);}

public:

    explicit SplineBase();

    /** Caclulates the position for given segment Idx, and percent of segment length t
        @param t - percent of segment length, assumes that t in range [0, 1]
        @param Idx - spline segment index, should be in range [first, last)
     */
    void evaluate_percent(index_type Idx, float u, G3D::Vector3& c) const;

    /** Caclulates derivation in index Idx, and percent of segment length t
        @param Idx - spline segment index, should be in range [first, last)
        @param t  - percent of spline segment length, assumes that t in range [0, 1]
     */
    void evaluate_derivative(index_type Idx, float u, G3D::Vector3& hermite) const;

    /**  Bounds for spline indexes. All indexes should be in range [first, last). */
    index_type first() const { return index_lo;}
    index_type last()  const { return index_hi;}

    bool empty() const { return index_lo == index_hi;}
    EvaluationMode mode() const { return static_cast<EvaluationMode>(m_mode);}
    bool isCyclic() const { return cyclic;}

    const ControlArray& getPoints() const { return points;}
    index_type getPointCount() const { return points.size();}
    const G3D::Vector3& getPoint(index_type i) const { return points[i];}

    /** Initializes spline. Don't call other methods while spline not initialized. */
    void init_spline(const G3D::Vector3 * controls, index_type count, EvaluationMode m);
    void init_cyclic_spline(const G3D::Vector3 * controls, index_type count, EvaluationMode m, index_type cyclic_point);

    /** As i can see there are a lot of ways how spline can be initialized
        would be no harm to have some custom initializers. */
    template<class Init>
    void init_spline(Init& initializer)
    {
        initializer(m_mode,cyclic,points,index_lo,index_hi);
    }

    template<class Init> inline void init_spline_custom(Init& initializer)
    {
        initializer(m_mode, cyclic, points, index_lo, index_hi);
    }

    void clear();

    /** Calculates distance between [i; i+1] points, assumes that index i is in bounds. */
    float SegLength(index_type i) const { return (this->*seglengths[m_mode])(i);}

    std::string ToString() const;
};

template<typename length_type>
class Spline : public SplineBase
{
public:
    typedef length_type LengthType;
    typedef std::vector<length_type> LengthArray;
protected:

    LengthArray lengths;

    index_type computeIndexInBounds(length_type length) const;
public:

    explicit Spline(){}

    /** Calculates the position for given t
        @param t - percent of spline's length, assumes that t in range [0, 1]. */
    void evaluate_percent(float t, G3D::Vector3 & c) const;

    /** Calculates derivation for given t
        @param t - percent of spline's length, assumes that t in range [0, 1]. */
    void evaluate_derivative(float t, G3D::Vector3& hermite) const;

    /** Calculates the position for given segment Idx, and percent of segment length t
        @param t = partial_segment_length / whole_segment_length
        @param Idx - spline segment index, should be in range [first, last). */
    void evaluate_percent(index_type Idx, float u, G3D::Vector3& c) const { SplineBase::evaluate_percent(Idx,u,c);}

    /** Caclulates derivation for index Idx, and percent of segment length t
        @param Idx - spline segment index, should be in range [first, last)
        @param t  - percent of spline segment length, assumes that t in range [0, 1]. */
    void evaluate_derivative(index_type Idx, float u, G3D::Vector3& c) const { SplineBase::evaluate_derivative(Idx,u,c);}

    // Assumes that t in range [0, 1]
    index_type computeIndexInBounds(float t) const;
    void computeIndex(float t, index_type& out_idx, float& out_u) const;

    /** Initializes spline. Don't call other methods while spline not initialized. */
    void init_spline(const G3D::Vector3 * controls, index_type count, EvaluationMode m) { SplineBase::init_spline(controls,count,m);}
    void init_cyclic_spline(const G3D::Vector3 * controls, index_type count, EvaluationMode m, index_type cyclic_point) { SplineBase::init_cyclic_spline(controls,count,m,cyclic_point);}

    /**  Initializes lengths with SplineBase::SegLength method. */
    void initLengths();

    /** Initializes lengths in some custom way
        Note that value returned by cacher must be greater or equal to previous value. */
    template<class T>
    void initLengths(T& cacher)
    {
        index_type i = index_lo;
        lengths.resize(index_hi+1);
        length_type prev_length = 0, new_length = 0;
        while(i < index_hi)
        {
            new_length = cacher(*this, i);

            //length overflowed, assign to max positive value
            if( new_length < 0)
                new_length = std::numeric_limits<length_type>::max();

            lengths[++i] = new_length;

            //ASSERT(prev_length <= new_length);
            if(prev_length > new_length)
                break;
            prev_length = new_length;
        }
    }

    void updateLengths(float velocity, int32 pointId, int32 timePassed)
    {
        float velocityInv = 1000.f/velocity;
        index_type i = index_lo;
        lengths.resize(index_hi+1);
        length_type prev_length = 0, new_length = 0;

        // Before current node not change length
        while(i < pointId - 1)
            new_length += lengths[++i];

        // Current node recalc length
        ++i;
        length_type seg_length = length(pointId, pointId+1);
        length_type cur_seg = lengths[pointId] - timePassed;

        float percSeg = (cur_seg * 100.0f) / seg_length / 100.0f;

        length_type new_seg_length = SegLength(pointId) * velocityInv;
        new_length += seg_length * percSeg;
        new_length += new_seg_length * (1.0f - percSeg);

        // After all node recalc
        while(i < index_hi)
        {
            new_length += SegLength(i) * velocityInv;

            //length overflowed, assign to max positive value
            if( new_length < 0)
                new_length = std::numeric_limits<length_type>::max();

            lengths[++i] = new_length;

            if(prev_length > new_length)
                break;
            prev_length = new_length;
        }
    }

    /** Returns length of the whole spline. */
    length_type length() const
    {
        if (lengths.empty())
            return 0;
        return lengths[index_hi];
    }
    /** Returns length between given nodes. */
    length_type length(index_type first, index_type last) const { return lengths[last]-lengths[first];}
    length_type length(index_type Idx) const { return lengths[Idx];}

    void set_length(index_type i, length_type length) { lengths[i] = length;}
    void clear();
};

}

#include "SplineImpl.h"

#endif // TRINITYSERVER_SPLINE_H

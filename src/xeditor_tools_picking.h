#ifndef XEDITOR_TOOLS_PICKING_H
#define XEDITOR_TOOLS_PICKING_H
#pragma once

// Shared CPU ray-picking primitives - factored out of xskeleton.plugin's own PickWedge (which
// duplicated RayTriangleIntersect/RaySphereIntersect locally) so every editor's viewport can pick
// against its own geometry with a ray from xgpu::tools::view::RayFromScreen without re-deriving the
// math. Solid/generous test volumes (triangles+spheres for bones, AABBs for boxes) over exact
// per-pixel GPU ID-picking - see xskeleton.plugin's own E23 heritage comment for why: this codebase's
// pickable shapes are chunky enough that a synchronous CPU test is simpler and precise enough, with
// none of GPU ID-picking's extra pipeline/SSBO/multi-frame-readback machinery.
#include "dependencies/xmath/source/xmath.h"
#include <cmath>
#include <limits>

namespace xeditor_tools::picking
{
    // Moller-Trumbore. Dir need not be unit length - OutT is then in "Dir units", only meaningful
    // compared against another OutT for the same ray.
    inline bool RayTriangleIntersect(const xmath::fvec3& Origin, const xmath::fvec3& Dir, const xmath::fvec3& V0, const xmath::fvec3& V1, const xmath::fvec3& V2, float& OutT) noexcept
    {
        constexpr float Epsilon = 1.0e-6f;

        const xmath::fvec3 Edge1 = V1 - V0;
        const xmath::fvec3 Edge2 = V2 - V0;
        const xmath::fvec3 H     = Dir.Cross(Edge2);
        const float        A     = Edge1.Dot(H);
        if (std::fabs(A) < Epsilon) return false;

        const float        F = 1.0f / A;
        const xmath::fvec3 S = Origin - V0;
        const float        U = F * S.Dot(H);
        if (U < 0.0f || U > 1.0f) return false;

        const xmath::fvec3 Q = S.Cross(Edge1);
        const float        V = F * Dir.Dot(Q);
        if (V < 0.0f || U + V > 1.0f) return false;

        const float T = F * Edge2.Dot(Q);
        if (T <= Epsilon) return false;

        OutT = T;
        return true;
    }

    inline bool RaySphereIntersect(const xmath::fvec3& Origin, const xmath::fvec3& Dir, const xmath::fvec3& Center, float Radius, float& OutT) noexcept
    {
        const xmath::fvec3 OC = Origin - Center;
        const float A = Dir.Dot(Dir);
        if (A < 1.0e-12f) return false;
        const float B = 2.0f * Dir.Dot(OC);
        const float C = OC.Dot(OC) - Radius * Radius;
        const float Disc = B * B - 4.0f * A * C;
        if (Disc < 0.0f) return false;

        const float SqrtDisc = std::sqrt(Disc);
        float T = (-B - SqrtDisc) / (2.0f * A);
        if (T <= 1.0e-6f) T = (-B + SqrtDisc) / (2.0f * A);
        if (T <= 1.0e-6f) return false;

        OutT = T;
        return true;
    }

    // Axis-aligned box, Center +/- HalfExtents (slab method).
    inline bool RayAABBIntersect(const xmath::fvec3& Origin, const xmath::fvec3& Dir, const xmath::fvec3& Center, const xmath::fvec3& HalfExtents, float& OutT) noexcept
    {
        float TMin = 0.0f, TMax = std::numeric_limits<float>::max();
        const float OriginArr[3]{ Origin.m_X, Origin.m_Y, Origin.m_Z };
        const float DirArr[3]{ Dir.m_X, Dir.m_Y, Dir.m_Z };
        const float MinArr[3]{ Center.m_X - HalfExtents.m_X, Center.m_Y - HalfExtents.m_Y, Center.m_Z - HalfExtents.m_Z };
        const float MaxArr[3]{ Center.m_X + HalfExtents.m_X, Center.m_Y + HalfExtents.m_Y, Center.m_Z + HalfExtents.m_Z };

        for (int i = 0; i < 3; ++i)
        {
            if (std::fabs(DirArr[i]) < 1.0e-12f)
            {
                if (OriginArr[i] < MinArr[i] || OriginArr[i] > MaxArr[i]) return false;
                continue;
            }
            const float InvD = 1.0f / DirArr[i];
            float T0 = (MinArr[i] - OriginArr[i]) * InvD;
            float T1 = (MaxArr[i] - OriginArr[i]) * InvD;
            if (T0 > T1) std::swap(T0, T1);
            TMin = std::max(TMin, T0);
            TMax = std::min(TMax, T1);
            if (TMin > TMax) return false;
        }

        if (TMin <= 1.0e-6f) return false;
        OutT = TMin;
        return true;
    }


    // Oriented box: Transform ray into local space (inverse Rotation about Center), then AABB slab
    // with local HalfExtents. Matches a mesh drawn with setupSRT(Scale, Rotation, Position) where
    // unit-mesh half-extents are 0.5 (pass HalfExtents = 0.5 * Scale).
    inline bool RayOBBIntersect( const xmath::fvec3& Origin, const xmath::fvec3& Dir
                               , const xmath::fvec3& Center, const xmath::fquat& Rotation
                               , const xmath::fvec3& HalfExtents, float& OutT ) noexcept
    {
        const xmath::fquat InvR = Rotation.InverseCopy();
        const xmath::fvec3 LocalOrigin = InvR.RotateVector(Origin - Center);
        const xmath::fvec3 LocalDir    = InvR.RotateVector(Dir);
        return RayAABBIntersect(LocalOrigin, LocalDir, xmath::fvec3::fromZero(), HalfExtents, OutT);
    }
    // Accumulates the closest hit across a loop of candidates - the "T < OutT" bookkeeping every
    // PickXxx function above already repeats per-candidate, pulled out once.
    template< typename T_ID >
    struct closest_hit
    {
        T_ID  m_Id{};
        float m_T = std::numeric_limits<float>::max();

        void Consider(T_ID Id, float T) noexcept { if (T < m_T) { m_T = T; m_Id = Id; } }
        bool isValid(void) const noexcept { return m_T != std::numeric_limits<float>::max(); }
    };
}

#endif // XEDITOR_TOOLS_PICKING_H

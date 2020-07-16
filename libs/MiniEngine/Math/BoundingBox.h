#pragma once

#include "Frustum.h"

namespace Math
{
    class BoundingBox : public Frustum
    {
    public:
        BoundingBox() = default;

        BoundingBox(Vector3 boxMin, Vector3 boxMax)
        {
            float Left   = boxMin.GetX();
            float Right  = boxMax.GetX();
            float Top    = boxMax.GetY();
            float Bottom = boxMin.GetY();
            float Front  = -boxMin.GetZ();
            float Back   = -boxMax.GetZ();
            ConstructOrthographicFrustum(Left, Right, Top, Bottom, Front, Back);
        }

        BoundingSphere toBoundingSphere() const;

        friend BoundingBox  operator* (const OrthogonalTransform& xform, const BoundingBox& frustum);	// Fast
        friend BoundingBox  operator* (const AffineTransform& xform, const BoundingBox& frustum);		// Slow
        friend BoundingBox  operator* (const Matrix4& xform, const BoundingBox& frustum);				// Slowest (and most general)
    };

    inline BoundingSphere BoundingBox::toBoundingSphere() const
    {
        auto boxMin = GetFrustumCorner(Math::Frustum::kNearLowerLeft);
        auto boxMax = GetFrustumCorner(Math::Frustum::kFarUpperRight);
        auto anchor = boxMin;
        auto size = boxMax - boxMin;

        auto center = anchor + 0.5 * size;
        auto radius = Math::Length(size) * 0.5f;
        return Math::BoundingSphere((center), Math::Scalar(radius));
    }

    inline BoundingBox operator* (const OrthogonalTransform& xform, const BoundingBox& frustum)
    {
        BoundingBox result;

        for (int i = 0; i < 8; ++i)
            result.m_FrustumCorners[i] = xform * frustum.m_FrustumCorners[i];

        for (int i = 0; i < 6; ++i)
            result.m_FrustumPlanes[i] = xform * frustum.m_FrustumPlanes[i];

        return result;
    }

    inline BoundingBox operator* (const AffineTransform& xform, const BoundingBox& frustum)
    {
        BoundingBox result;

        for (int i = 0; i < 8; ++i)
            result.m_FrustumCorners[i] = xform * frustum.m_FrustumCorners[i];

        Matrix4 XForm = Transpose(Invert(Matrix4(xform)));

        for (int i = 0; i < 6; ++i)
            result.m_FrustumPlanes[i] = BoundingPlane(XForm * Vector4(frustum.m_FrustumPlanes[i]));

        return result;
    }

    inline BoundingBox operator* (const Matrix4& mtx, const BoundingBox& frustum)
    {
        BoundingBox result;

        for (int i = 0; i < 8; ++i)
            result.m_FrustumCorners[i] = Vector3(mtx * frustum.m_FrustumCorners[i]);

        Matrix4 XForm = Transpose(Invert(mtx));

        for (int i = 0; i < 6; ++i)
            result.m_FrustumPlanes[i] = BoundingPlane(XForm * Vector4(frustum.m_FrustumPlanes[i]));

        return result;
    }
}

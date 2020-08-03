#pragma once

#include <numeric>
#include <algorithm>
#include "Frustum.h"

namespace Math
{
    class BoundingBox : public Frustum
    {
    public:
        BoundingBox() = default;

        BoundingBox(Vector3 boxMin, Vector3 boxMax, Vector3 primaryVec)
            : m_PrimaryVector(primaryVec)
        {
            float Left   = boxMin.GetX();
            float Right  = boxMax.GetX();
            float Top    = boxMax.GetY();
            float Bottom = boxMin.GetY();
            float Front  = -boxMin.GetZ();
            float Back   = -boxMax.GetZ();
            ConstructOrthographicFrustum(Left, Right, Top, Bottom, Front, Back);
        }

        Vector3 GetPrimaryVector() const { return m_PrimaryVector; }

        BoundingSphere toBoundingSphere() const;

        friend BoundingBox  operator* (const OrthogonalTransform& xform, const BoundingBox& frustum);	// Fast
        friend BoundingBox  operator* (const AffineTransform& xform, const BoundingBox& frustum);		// Slow
        friend BoundingBox  operator* (const Matrix4& xform, const BoundingBox& frustum);				// Slowest (and most general)

        friend BoundingBox  operator+ (const BoundingBox& lhs, const BoundingBox& rhs);

    private:
        Vector3 m_PrimaryVector;
    };

    inline BoundingSphere BoundingBox::toBoundingSphere() const
    {
        Vector3 centroid = std::accumulate(std::begin(m_FrustumCorners), std::end(m_FrustumCorners), Vector3(0, 0, 0)) / 8;
        float radius = std::accumulate(std::begin(m_FrustumCorners), std::end(m_FrustumCorners), 0.0f, [&](float maxRadius, Vector3 corner) {
            float length = XMVectorGetX(XMVector3Length(corner - centroid));
            return max(maxRadius, length);
        });

        return Math::BoundingSphere(centroid, radius);
    }

    inline BoundingBox operator* (const OrthogonalTransform& xform, const BoundingBox& frustum)
    {
        BoundingBox result;

        for (int i = 0; i < 8; ++i)
            result.m_FrustumCorners[i] = xform * frustum.m_FrustumCorners[i];

        result.m_PrimaryVector = xform * frustum.m_PrimaryVector;

        for (int i = 0; i < 6; ++i)
            result.m_FrustumPlanes[i] = xform * frustum.m_FrustumPlanes[i];

        return result;
    }

    inline BoundingBox operator* (const AffineTransform& xform, const BoundingBox& frustum)
    {
        BoundingBox result;

        for (int i = 0; i < 8; ++i)
            result.m_FrustumCorners[i] = xform * frustum.m_FrustumCorners[i];

        result.m_PrimaryVector = xform * frustum.m_PrimaryVector;

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

        result.m_PrimaryVector = Vector3(mtx * frustum.m_PrimaryVector);

        Matrix4 XForm = Transpose(Invert(mtx));

        for (int i = 0; i < 6; ++i)
            result.m_FrustumPlanes[i] = BoundingPlane(XForm * Vector4(frustum.m_FrustumPlanes[i]));

        return result;
    }

    inline BoundingBox operator+ (const BoundingBox& lhs, const BoundingBox& rhs)
    {
        Vector3 lMin = lhs.GetFrustumCorner(Frustum::CornerID::kNearLowerLeft);
        Vector3 rMin = rhs.GetFrustumCorner(Frustum::CornerID::kNearLowerLeft);
        Vector3 lMax = lhs.GetFrustumCorner(Frustum::CornerID::kFarUpperRight);
        Vector3 rMax = rhs.GetFrustumCorner(Frustum::CornerID::kFarUpperRight);

        float minX = std::min<float>(lMin.GetX(), rMin.GetX());
        float minY = std::min<float>(lMin.GetY(), rMin.GetY());
        float minZ = std::min<float>(lMin.GetZ(), rMin.GetZ());
        float maxX = std::max<float>(lMax.GetX(), rMax.GetX());
        float maxY = std::max<float>(lMax.GetY(), rMax.GetY());
        float maxZ = std::max<float>(lMax.GetZ(), rMax.GetZ());

        Vector3 pvec = 0.5 * (lhs.m_PrimaryVector + rhs.m_PrimaryVector);

        return BoundingBox(Vector3(minX, minY, minZ), Vector3(maxX, maxY, maxZ), pvec);
    }
}

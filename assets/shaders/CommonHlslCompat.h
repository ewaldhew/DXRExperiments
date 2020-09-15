#ifndef COMMONHLSLCOMPAT_H
#define COMMONHLSLCOMPAT_H

struct DirectionalLightParams
{
    XMFLOAT4 forwardDir;
    XMFLOAT4 color; // radiant intensity at unit distance
};

struct PointLightParams
{
    XMFLOAT4 worldPos;
    XMFLOAT4 color;
};

struct CameraParams
{
    XMFLOAT4 worldEyePos;
    XMFLOAT4 U;
    XMFLOAT4 V;
    XMFLOAT4 W;
    XMFLOAT2 frustumNH;
    XMFLOAT2 frustumNV;
    XMFLOAT2 frustumNearFar;
    XMFLOAT2 jitters;
    UINT frameCount;
    UINT accumCount;
    XMFLOAT2 padding;
};

struct ProceduralPrimitiveAttributes
{
    XMFLOAT3 normal;
};

// Attributes per primitive instance.
struct PrimitiveInstanceConstants
{
    //UINT instanceIndex;  // Used to index into per frame attributes
    UINT primitiveType; // Procedural primitive type
    UINT pad1;
    UINT pad2;
    UINT pad3;
    XMMATRIX localSpaceToBottomLevelAS;   // Matrix from local primitive space to bottom-level object space.
    XMMATRIX bottomLevelASToLocalSpace;   // Matrix from bottom-level object space to local primitive space.
};

#ifndef RASTER_PIPELINE
namespace MaterialType {
    enum Enum
    {
        Diffuse = 0,
        Glossy,
        Glass,
        __UniformMaterials,

        DiffuseTexture,
        ParticipatingMedia,

        Count,
    };
}
#else
namespace MaterialType {
    typedef UINT Enum;
    static const UINT
        Diffuse = 0,
        Glossy = Diffuse + 1,
        Glass = Glossy + 1,
        __UniformMaterials = Glass + 1,

        DiffuseTexture = __UniformMaterials + 1,
        ParticipatingMedia = DiffuseTexture + 1,
        __END;
}
#endif

struct MaterialParams
{
    XMFLOAT4 albedo;
    XMFLOAT4 specular;
    XMFLOAT4 emissive;
    float reflectivity;
    float roughness;
    float IoR;
    UINT type; // 0: diffuse, 1: glossy, 2: specular (glass)
};

struct MaterialTextureParams
{
    XMMATRIX objectSpaceToTex;
};

// Number of metaballs to use within an AABB.
#define N_METABALLS 3    // = {3, 5}

// Limitting calculations only to metaballs a ray intersects can speed up raytracing
// dramatically particularly when there is a higher number of metaballs used.
// Use of dynamic loops can have detrimental effects to performance for low iteration counts
// and outweighing any potential gains from avoiding redundant calculations.
// Requires: USE_DYNAMIC_LOOPS set to 1 to take effect.
#if N_METABALLS >= 5
#define USE_DYNAMIC_LOOPS 1
#define LIMIT_TO_ACTIVE_METABALLS 1
#else
#define USE_DYNAMIC_LOOPS 0
#define LIMIT_TO_ACTIVE_METABALLS 0
#endif

#define N_FRACTAL_ITERATIONS 4      // = <1,...>

#ifndef RASTER_PIPELINE
namespace PrimitiveType {
    enum Enum
    {
        AnalyticPrimitive_AABB = 0,
        AnalyticPrimitive_Spheres,
        VolumetricPrimitive_Metaballs,
        SignedDistancePrimitive_MiniSpheres,
        SignedDistancePrimitive_IntersectedRoundCube,
        SignedDistancePrimitive_SquareTorus,
        SignedDistancePrimitive_TwistedTorus,
        SignedDistancePrimitive_Cog,
        SignedDistancePrimitive_Cylinder,
        SignedDistancePrimitive_FractalPyramid,
    };
}
#else
namespace PrimitiveType {
    typedef UINT Enum;
    static const UINT
        AnalyticPrimitive_AABB = 0,
        AnalyticPrimitive_Spheres = AnalyticPrimitive_AABB + 1,
        VolumetricPrimitive_Metaballs = AnalyticPrimitive_Spheres + 1,
        SignedDistancePrimitive_MiniSpheres = VolumetricPrimitive_Metaballs + 1,
        SignedDistancePrimitive_IntersectedRoundCube = SignedDistancePrimitive_MiniSpheres + 1,
        SignedDistancePrimitive_SquareTorus = SignedDistancePrimitive_IntersectedRoundCube + 1,
        SignedDistancePrimitive_TwistedTorus = SignedDistancePrimitive_SquareTorus + 1,
        SignedDistancePrimitive_Cog = SignedDistancePrimitive_TwistedTorus + 1,
        SignedDistancePrimitive_Cylinder = SignedDistancePrimitive_Cog + 1,
        SignedDistancePrimitive_FractalPyramid = SignedDistancePrimitive_Cylinder + 1,
        __END;
}
#endif

namespace PhotonMapID {
#ifndef RASTER_PIPELINE
    enum Value
    {
        Surface = 0,
        Volume, /* must be last */
        Count
    };
#else
    typedef UINT Value;
    static const Value
        Surface = 0,
        Volume = Surface + 1,
        Count = Volume + 1;
#endif
}

struct PhotonMappingConstants
{
    XMUINT2 numTiles;
    float tileAreaConstant;
    float maxRayLength;
    XMUINT4 counts[PhotonMapID::Count];

    // Splatting options
    XMFLOAT3 volumeBboxMin;
    float kernelScaleMin;
    XMFLOAT3 volumeBboxMax;
    float kernelScaleMax;
    float uniformScaleStrength;
    float maxLightShapingScale;
    float kernelCompressFactor;
    float volumeSplatPhotonSize;
    UINT particlesPerSlab;
    UINT photonGeometryBuildStrategy;
};

struct Photon
{
    XMFLOAT3 position;
    XMFLOAT3 power;
    XMFLOAT3 direction;
    XMFLOAT3 normal;
    float distTravelled;
    UINT randSeed;
    UINT materialIndex;
    UINT padding;
};

struct PhotonAABB
{
    float MinX;
    float MinY;
    float MinZ;
    float MaxX;
    float MaxY;
    float MaxZ;
    float pad0;
    float pad1;
};

#endif // !COMMONHLSLCOMPAT_H

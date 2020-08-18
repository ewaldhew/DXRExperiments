#define HLSL
#include "RasterCommon.hlsli"
#include "ProceduralPrimitivesLibrary.hlsli"

ConstantBuffer<PrimitiveInstanceConstants> aabbCB : register(b1, space1);
ConstantBuffer<PerObjectConstants> obj : register(b1);

struct PixelShaderInput
{
    float4 position : SV_POSITION;
    float3 positionObjSpace : POSITION_OBJ;
    float3 normal : NORMAL;
};

struct PixelShaderOutput
{
    float4 gbNormal : SV_TARGET0;
    //float4 gbAlbedo : SV_TARGET1;
    float depth : SV_Depth;
};

bool Intersection(in Ray ray, out float thit, out ProceduralPrimitiveAttributes attr)
{
    bool didHit;

    switch (aabbCB.primitiveType)
    {
    case PrimitiveType::AnalyticPrimitive_AABB:
    case PrimitiveType::AnalyticPrimitive_Spheres:
        didHit = RayAnalyticGeometryIntersectionTest(ray, aabbCB.primitiveType, thit, attr);
        break;
    case PrimitiveType::VolumetricPrimitive_Metaballs:
        didHit = RayVolumetricGeometryIntersectionTest(ray, aabbCB.primitiveType, thit, attr, 0);
        break;
    case PrimitiveType::SignedDistancePrimitive_MiniSpheres:
    case PrimitiveType::SignedDistancePrimitive_IntersectedRoundCube:
    case PrimitiveType::SignedDistancePrimitive_SquareTorus:
    case PrimitiveType::SignedDistancePrimitive_TwistedTorus:
    case PrimitiveType::SignedDistancePrimitive_Cog:
    case PrimitiveType::SignedDistancePrimitive_Cylinder:
    case PrimitiveType::SignedDistancePrimitive_FractalPyramid:
        didHit = RaySignedDistancePrimitiveTest(ray, aabbCB.primitiveType, thit, attr);
        break;
    default:
        didHit = false;
        break;
    }

    if (!didHit)
    {
        return false;
    }

    attr.normal = mul(attr.normal, (float3x3) aabbCB.localSpaceToBottomLevelAS);
    attr.normal = normalize(mul((float3x3) obj.worldMatrix, attr.normal));

    return true;
}

// Test if a position is in the volume
bool shootVolumeRay(inout float3 pos, float3 dir)
{
    Ray ray; // input: obj space
    ray.origin = mul(float4(pos, 1), (float4x4) aabbCB.bottomLevelASToLocalSpace).xyz;
    ray.direction = mul(dir, (float3x3) aabbCB.bottomLevelASToLocalSpace);

    float thit;
    ProceduralPrimitiveAttributes attr;
    bool hit = Intersection(ray, thit, attr);
    if (hit) {
       return thit > 0.0f;
    }

    return hit;
}

float getExtinction(float3 position)
{
    return sampleMaterial(materialParams.albedo, position).x;
}

bool evaluateVolumeInteraction(inout uint randSeed, inout float3 position, float3 direction, out float t, inout float3 params)
{
    float extinctionMax = materialParams.reflectivity;
    t = 0.0f;
    float3 pos;

    do { // woodcock tracking
        t -= log(nextRand(randSeed)) / extinctionMax;
        pos = position + direction * t;
    } while (shootVolumeRay(pos, direction) && getExtinction(pos) < nextRand(randSeed) * extinctionMax);

    position = pos;
    // x - extinction (ka), y - scattering (ks)
    params = sampleMaterial(materialParams.albedo, pos).xyz;

    return shootVolumeRay(position, direction);
}

PixelShaderOutput main(PixelShaderInput IN)
{
    PixelShaderOutput OUT;

    MaterialParams mat = materialParams;
    collectMaterialParams(mat, IN.positionObjSpace);

    float4 albedo = mat.albedo;
    if (albedo.a == 0.0f)
    {
        discard;//discard this pixel
    }

    if (obj.isProcedural)
    {
        // build the local space ray
        float3 camPosObj = mul(obj.invWorldMatrix, float4(perFrameConstants.cameraParams.worldEyePos.xyz, 1.0f)).xyz;
        float3 rayDir = normalize(IN.positionObjSpace - camPosObj);

        Ray ray;
        ray.origin = mul(float4(camPosObj, 1), (float4x4) aabbCB.bottomLevelASToLocalSpace).xyz;
        ray.direction = mul(rayDir, (float3x3) aabbCB.bottomLevelASToLocalSpace);

        float thit;
        ProceduralPrimitiveAttributes attr;
        if (!Intersection(ray, thit, attr)) { discard; }

        float3 hitPositionObj = camPosObj + thit * rayDir;

        if (mat.type == MaterialType::ParticipatingMedia)
        {
            // single scattering
            uint randSeed = initRand(IN.position.x, IN.position.y);
            float throughput = 1.0;
            float3 params; // x - extinction, y - scattering
            float t;
            if(!evaluateVolumeInteraction(randSeed, hitPositionObj, rayDir, t, params))
            {
                discard;
            }
/*
            if (evaluateVolumeInteraction(randSeed, hitPositionObj, rayDir, t, params))
            {
                // attenuate by albedo = scattering / extinction
                throughput *= params.y / params.x;

                // Russian roulette absorption
                if (throughput < 0.2) {
                    if (nextRand(randSeed) > throughput * 5.0) {
                        throughput = 0.0;
                    } else {
                        throughput = 0.2;
                    }
                }
            }
            else { discard; }
*/
        }

        float4x4 mvp = mul(perFrameConstants.WorldToViewClipMatrix, (float4x4) obj.worldMatrix);
        float4 position = mul(mvp, float4(hitPositionObj, 1.0f));

        OUT.depth = position.z / position.w;
        OUT.gbNormal = float4(attr.normal.xyz, 0.0f);
    }
    else
    {
        OUT.depth = IN.position.z;
        OUT.gbNormal = float4(normalize(IN.normal), 0.0f);
    }

    //OUT.gbAlbedo = float4(albedo.xyz * albedo.a, 1.0f);

    return OUT;
}

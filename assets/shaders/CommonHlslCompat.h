#ifndef COMMONHLSLCOMPAT_H
#define COMMONHLSLCOMPAT_H

struct PhotonMappingConstants
{
    XMUINT2 numTiles;
    float tileAreaConstant;
    float maxRayLength;
};

struct Photon
{
    XMFLOAT3 position;
    XMFLOAT3 power;
    XMFLOAT3 direction;
    XMFLOAT3 normal;
    float distTravelled;
    UINT randSeed;
    XMUINT2 padding;
};

#endif // !COMMONHLSLCOMPAT_H

#pragma once
#include <vtkSmartPointer.h>
#include <cstdint>
#include <QFile>

#include <algorithm>
#include <cstring>
#include "PatientInfo.h"

class vtkImageData;

#pragma pack(push, 1)
struct _3Dinfo {
    uint32_t UIheader[4];     // nx, ny, nz, t
    double   Doubheader[3];   // sizeX, sizeY, sizeZ (в тех же единицах)

    uint16_t IsNew3Dinfo;     // 0 old, 1 new
    uint32_t IsYZSwap;        // if 0 - need to swap when loaded if 1 - no need to swap when loaded  
    uint32_t IsMRI;           // if 0 - CT if 1 - MRI
    uint16_t Sex;             // 0 unknown, 1 male, 2 female

    char PatientName[64];     // UTF-8, '\0' terminated
    char PatientId[32];
    char Description[64];
    char Sequence[64];
    char SeriesNumber[16];

    unsigned char unused[512 
        - 4 * 4 - 3 * 8 
        -2 - 4 - 4 - 2
        - 64 - 32 - 64 - 64 - 16
    ];
};

struct _mini3Dinfo 
{
    uint32_t UIheader[3];     // nx, ny, nz
    double   Doubheader[3];   // sizeX, sizeY, sizeZ

    unsigned char unused[64
        - 3 * 4 - 3 * 8
    ];
};
#pragma pack(pop)

static_assert(sizeof(_3Dinfo) == 512, "_3Dinfo must be exactly 512 bytes");

template<int N>
static void writeUtf8(char(&dst)[N], const QString& s)
{
    const QByteArray a = s.toUtf8();
    const int n = std::min<int>(N - 1, int(a.size()));

    std::memset(dst, 0, N);
    if (n > 0)
        std::memcpy(dst, a.constData(), size_t(n));
}

template<int N>
static QString readUtf8(const char(&src)[N])
{
    int n = 0;
    while (n < N && src[n] != '\0') ++n;
    return QString::fromUtf8(src, n);
}

inline uint16_t bswap16(uint16_t v);

vtkSmartPointer<vtkImageData> FixAxesFor3DR(vtkImageData* src, bool flipX, bool flipY, bool flipZ);

vtkSmartPointer<vtkImageData> Load3DR_Normalized(const QString& path, bool& IsMRI, bool flipX = false, bool flipY = true, bool flipZ = true);

vtkSmartPointer<vtkImageData> fixAxesIfNeeded(vtkImageData* src, bool swapYZ, bool flipX = false, bool flipY = true, bool flipZ = true);

vtkSmartPointer<vtkImageData> read3dr_asVtk(const QString& path, _3Dinfo* outHdr = nullptr);

vtkSmartPointer<vtkImageData> read3dr_asVtk_noflip(const QString& path, _3Dinfo* outHdr = nullptr);

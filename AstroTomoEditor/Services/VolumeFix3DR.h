#pragma once
#include <vtkSmartPointer.h>
#include <cstdint>
#include <QFile>

class vtkImageData;

#pragma pack(push, 1)
struct _3Dinfo {
    uint32_t UIheader[4];     // nx, ny, nz, t
    double   Doubheader[3];   // sizeX, sizeY, sizeZ (в тех же единицах)
    uint32_t IsYZSwap;        // if 0 - need to swap when loaded if 1 - no need to swap when loaded  
    uint32_t IsMRI;           // if 0 - CT if 1 - MRI
    unsigned char unused[512 - 4 * 4 - 3 * 8 - 4 - 4];
};
#pragma pack(pop)

inline uint16_t bswap16(uint16_t v);

vtkSmartPointer<vtkImageData> FixAxesFor3DR(vtkImageData* src, bool flipX, bool flipY, bool flipZ);

vtkSmartPointer<vtkImageData> Load3DR_Normalized(const QString& path, bool& IsMRI, bool flipX = false, bool flipY = true, bool flipZ = true);

vtkSmartPointer<vtkImageData> fixAxesIfNeeded(vtkImageData* src, bool swapYZ, bool flipX = false, bool flipY = true, bool flipZ = true);

vtkSmartPointer<vtkImageData> read3dr_asVtk(const QString& path, _3Dinfo* outHdr = nullptr);

vtkSmartPointer<vtkImageData> read3dr_asVtk_noflip(const QString& path, _3Dinfo* outHdr = nullptr);

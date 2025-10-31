#include "DicomRange.h"
#include <vtkDICOMReader.h>
#include <vtkDICOMMetaData.h>
#include <vtkDICOMTag.h>
#include <vtkImageData.h>

DicomInfo GetDicomRangesVTK(vtkSmartPointer<vtkDICOMReader> r)
{
    DicomInfo out{};
    auto* md = r->GetMetaData();

    auto getInt = [&](unsigned short g, unsigned short e, int def = 0) {
        vtkDICOMTag tag(g, e);
        return md->HasAttribute(tag) ? md->GetAttributeValue(tag).AsInt() : def;
        };
    auto getDbl = [&](unsigned short g, unsigned short e, double def = 0.0) {
        vtkDICOMTag tag(g, e);
        return md->HasAttribute(tag) ? md->GetAttributeValue(tag).AsDouble() : def;
        };

    out.bitsAllocated = getInt(0x0028, 0x0100, 16);
    out.bitsStored = getInt(0x0028, 0x0101, 12);
    out.highBit = getInt(0x0028, 0x0102, out.bitsStored - 1);
    out.pixelRep = getInt(0x0028, 0x0103, 0);
    out.slope = getDbl(0x0028, 0x1053, 1.0);
    out.intercept = getDbl(0x0028, 0x1052, 0.0);

    // физический диапазон
    double rminmax[2]{};
    r->GetOutput()->GetScalarRange(rminmax);
    out.physicalMin = rminmax[0];
    out.physicalMax = rminmax[1];
    return out;
}


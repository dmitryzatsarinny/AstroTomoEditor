#pragma once
#include <vtkDICOMImageReader.h>
#include <vtkDICOMReader.h>

enum Mode { CT, MRI, CT3DR, MRI3DR};

struct DicomInfo 
{
    Mode TypeOfRecord = CT;
    double physicalMin, physicalMax;
    int RealMin, RealMax;
    int bitsAllocated, bitsStored, highBit, pixelRep;
    double slope, intercept;
    bool rawActualFromTags;
    double mSpX{ 1.0 }, mSpY{ 1.0 }, mSpZ{ 1.0 };
    bool SpCreated{ false };
};


DicomInfo GetDicomRangesVTK(vtkSmartPointer<vtkDICOMReader> R);
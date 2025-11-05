#pragma once
#include <vtkDICOMImageReader.h>
#include <vtkDICOMReader.h>
#include <QString>

enum Mode { CT, MRI, CT3DR, MRI3DR};

constexpr unsigned int HistScale = 256u;
constexpr unsigned int HistMin = 0u;
constexpr unsigned int HistMax = 255u;

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
    QString XTitle = "Hounsfield Units";
    QString YTitle = "Voxel count";
    QString XLable = "HU";
    QString YLable = "N";
};


DicomInfo GetDicomRangesVTK(vtkSmartPointer<vtkDICOMReader> R);
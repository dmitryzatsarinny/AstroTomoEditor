#pragma once
#include <vtkDICOMImageReader.h>
#include <vtkDICOMReader.h>
#include <QString>

enum Mode { CT, MRI, CT3DR, MRI3DR};

constexpr unsigned int HistScale = 256u;
constexpr unsigned int HistMin = 0u;
constexpr unsigned int HistMax = 255u;
constexpr double scale = static_cast<double>(HistMax - HistMin); 

struct DicomInfo 
{
    Mode TypeOfRecord = CT;
    double physicalMin, physicalMax;
    int RealMin, RealMax;
    int bitsAllocated, bitsStored, highBit, pixelRep;
    double slope, intercept;
    bool rawActualFromTags;
    double mSpX{ 1.0 }, mSpY{ 1.0 }, mSpZ{ 1.0 };
    double OriginSpZ{ 1.0 };
    bool SpCreated{ false };
    QString XTitle = "Hounsfield Units";
    QString YTitle = "Voxel count";
    QString XLable = "HU";
    QString YLable = "N";

    quint16 Sex = 0;
    QString patientName = "Null";
    QString patientId = "0";
    QString Description = "Null";
    QString Sequence = "Null";
    QString SeriesNumber = "0";
    QString DicomPath = "C:\\";

    double VolumeOriginX{ 0.0 };
    double VolumeOriginY{ 0.0 };
    double VolumeOriginZ{ 0.0 };

    double VolumeFirstZ{ 0.0 };
    double VolumeLastZ{ 0.0 };

    double VolumeCenterX{ 0.0 };
    double VolumeCenterY{ 0.0 };
    double VolumeCenterZ{ 0.0 };
};


DicomInfo GetDicomRangesVTK(vtkSmartPointer<vtkDICOMReader> R);
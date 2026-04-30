#pragma once

#ifndef STORED_CONTOUR_INFO_H
#define STORED_CONTOUR_INFO_H

#include <QtGlobal>

constexpr int MaxStoredPointPerContour = 256;

// Старый WCHAR в Windows занимает 2 байта.
// char16_t тоже занимает 2 байта и хорошо подходит для бинарной совместимости.
static_assert(sizeof(char16_t) == 2, "char16_t must be 2 bytes");

// Важно: структура хранится/читается как бинарные данные.
// Отключаем выравнивание, чтобы компилятор не добавил неожиданные padding-байты.
#pragma pack(push, 1)

struct ContourType
{
    quint32 ContourTypeOnly : 16;
    quint32 ContourUnusedFlags : 15;
    quint32 ContourCT : 1;
};

struct StoredContourHeader
{
    // 0x00
    char ContourSign[16];              // фактически читается как "CONT0UR", далее нули

    // 0x10
    quint32 HeaderSize;
    quint32 Unused;                    // раньше PointInfoSize
    quint32 TotalDataSize;             // размер данных вместе с заголовком
    quint32 StoredContourPointSize;    // размер структуры StoredContourPoint

    // 0x20
    char16_t ContourName[32];

    // 0x60
    quint8 ContourReserved[144];

    quint8 ContourReserved2[1];

    // 0 - вырез
    // 1 - линия
    // 2 - закрашенная область
    quint8 ContourPurpose;

    quint8 UnVisible;

    // используется для ContourPurpose == 1
    quint8 Width;

    // 0xF4
    quint32 nContourPoint;             // для ContourByPPlaneType должно быть 1

    union
    {
        quint32 ContourTypeALL;
        ContourType ct;
    };

    quint32 ContourColor;
};

struct StoredContourCoord
{
    double x;
    double y;
    double z;
    double unused;
};

struct StoredContourPoint
{
    StoredContourCoord InitialContourPoint;
    StoredContourCoord UserViewPoint1;
    StoredContourCoord UserViewPoint2;

    quint8 unused[16];
};

struct StoredContourPlane
{
    StoredContourCoord PlaneCenterPoint;
    StoredContourCoord PlaneABCD;

    double radius;
    double reserved[3];

    quint8 unused[16];
};

struct StoredContourInfo
{
    StoredContourHeader header;

    union
    {
        StoredContourPoint Points[MaxStoredPointPerContour];
        StoredContourPlane plane;
    };
};

#pragma pack(pop)

// Проверки бинарной совместимости со старым форматом.
static_assert(sizeof(ContourType) == 4, "ContourType size mismatch");
static_assert(sizeof(StoredContourHeader) == 256, "StoredContourHeader size mismatch");
static_assert(sizeof(StoredContourCoord) == 32, "StoredContourCoord size mismatch");
static_assert(sizeof(StoredContourPoint) == 112, "StoredContourPoint size mismatch");
static_assert(sizeof(StoredContourPlane) == 112, "StoredContourPlane size mismatch");
static_assert(sizeof(StoredContourInfo) == 256 + 112 * MaxStoredPointPerContour,
    "StoredContourInfo size mismatch");

#endif // STORED_CONTOUR_INFO_H

#include "Save3DR.h"

#include <QFileDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QMessageBox>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>
#include <vtkImageCast.h>
#include <algorithm>
#include <cstring>

#include <Services/VolumeFix3DR.h> // для _3Dinfo
#include <Services/DicomRange.h>   // для DicomInfo/Mode (CT/MRI)

bool Save3DR::saveWithDialog(QWidget* parent, vtkImageData* img, const DicomInfo* dicom)
{
    if (!img) 
    {
        QMessageBox::warning(parent, QObject::tr("Сохранение 3DR"),
            QObject::tr("Нет загруженного объёма."));
        return false;
    }

    const QString defDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        parent,
        QObject::tr("Сохранить как 3DR"),
        defDir + "/volume.3dr",
        QObject::tr("Astro 3DR (*.3dr)")
    );
    if (path.isEmpty()) return false;

    QString err;
    if (!Save3DR::write(path, img, dicom, &err)) {
        QMessageBox::critical(parent, QObject::tr("Сохранение 3DR"),
            QObject::tr("Не удалось сохранить файл:\n%1").arg(err));
        return false;
    }
    return true;
}

bool Save3DR::write(const QString& path, vtkImageData* img, const DicomInfo* dicom, QString* error)
{
    if (!img) { if (error) *error = "Image is null"; return false; }

    bool swapYZ = true;

    vtkSmartPointer<vtkImageData> imgnew = fixAxesIfNeeded(img, swapYZ, false, false, false);

    // проверим тип
    if (imgnew->GetScalarType() != VTK_UNSIGNED_CHAR ||
        imgnew->GetNumberOfScalarComponents() != 1)
        return false;

    int ext[6];
    imgnew->GetExtent(ext);

    int nx = ext[1] - ext[0] + 1;
    int ny = ext[3] - ext[2] + 1;
    int nz = ext[5] - ext[4] + 1;

    if (nx <= 0 || ny <= 0 || nz <= 0) { if (error) *error = "Invalid image dimensions"; return false; }

    


    const qint64 voxels = qint64(nx) * ny * nz;
    if (voxels <= 0) { if (error) *error = "Empty volume"; return false; }

    std::vector<int16_t> out;
    out.resize(size_t(voxels));

    vtkIdType incX, incY, incZ;  // ВНИМАНИЕ: ИНКРЕМЕНТЫ В БАЙТАХ
    imgnew->GetIncrements(incX, incY, incZ);

    // стартовый адрес (xmin, ymin, zmin)
    auto* p0 = static_cast<const uint8_t*>(
        imgnew->GetScalarPointer(ext[0], ext[2], ext[4]));

    const double k = (dicom->physicalMax - dicom->physicalMin) / HistScale;

    for (int i = 0; i < nx * ny * nz; i++)
    {
        out[size_t(i)] = int16_t(*(p0 + i) * k);
    }

    // --- Заголовок .3dr (512 байт) ---
    _3Dinfo H{};
    H.UIheader[0] = uint32_t(nx);
    H.UIheader[1] = uint32_t(ny);
    H.UIheader[2] = uint32_t(nz);
    H.UIheader[3] = 0; // t

    if (swapYZ)
    {
        H.Doubheader[0] = dicom->mSpX;
        H.Doubheader[1] = dicom->mSpZ;
        H.Doubheader[2] = dicom->mSpY;
    }
    else
    {
        H.Doubheader[0] = dicom->mSpX;
        H.Doubheader[1] = dicom->mSpY;
        H.Doubheader[2] = dicom->mSpZ;
    }

    if (swapYZ)
        H.IsYZSwap = 0;
    else
        H.IsYZSwap = 1;

    H.IsMRI = (dicom && (dicom->TypeOfRecord == MRI || dicom->TypeOfRecord == MRI3DR)) ? 1u : 0u;

    // --- Запись на диск (LE) ---
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) 
    {
        if (error) *error = QString("Cannot open for write: %1").arg(path);
        return false;
    }

    if (f.write(reinterpret_cast<const char*>(&H), sizeof(H)) != qint64(sizeof(H))) {
        if (error) *error = "Header write failed";
        return false;
    }

    const qint64 bytes = qint64(out.size() * sizeof(int16_t));
    if (f.write(reinterpret_cast<const char*>(out.data()), bytes) != bytes) {
        if (error) *error = "Data write failed";
        return false;
    }

    if (!f.commit()) {
        if (error) *error = "Commit failed";
        return false;
    }
    return true;
}
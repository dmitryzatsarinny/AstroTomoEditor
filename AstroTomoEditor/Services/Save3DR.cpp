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

bool Save3DR::saveWithDialog(QWidget* parent, vtkImageData* img, const DicomInfo* dicom, QString& savepath)
{
    if (!img) 
    {
        QMessageBox::warning(parent, QObject::tr("Save 3DR"),
            QObject::tr("No loaded volume"));
        return false;
    }

    const QString defDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        parent,
        QObject::tr("Save as 3DR"),
        defDir + "/volume.3dr",
        QObject::tr("Astro 3DR (*.3dr)")
    );
    if (path.isEmpty()) 
        return false;

    QString err;
    if (!Save3DR::write(path, img, dicom, &err)) {
        QMessageBox::critical(parent, QObject::tr("Save 3DR"),
            QObject::tr("Failed to save file:\n%1").arg(err));
        return false;
    }
    savepath = path;
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

    const double k = (dicom ? (dicom->physicalMax - dicom->physicalMin) / HistScale : 1.0);

    for (int i = 0; i < nx * ny * nz; i++)
    {
        out[size_t(i)] = int16_t(*(p0 + i) * k);
    }

    // --- Заголовок .3dr (512 байт) ---
    _3Dinfo H{};
    H.UIheader[0] = uint32_t(nx);
    H.UIheader[1] = uint32_t(ny);
    H.UIheader[2] = uint32_t(nz);
    H.UIheader[3] = 0;

    if (dicom) {

        H.IsNew3Dinfo = 1;

        if (swapYZ) {
            H.Doubheader[0] = dicom->mSpX;
            H.Doubheader[1] = dicom->mSpZ;
            H.Doubheader[2] = dicom->mSpY;
        }
        else {
            H.Doubheader[0] = dicom->mSpX;
            H.Doubheader[1] = dicom->mSpY;
            H.Doubheader[2] = dicom->mSpZ;
        }

        H.IsMRI = (dicom->TypeOfRecord == MRI || dicom->TypeOfRecord == MRI3DR) ? 1u : 0u;

        H.Sex = uint16_t(dicom->Sex); // 0/1/2 как ты уже сделал в RenderView

        writeUtf8(H.PatientName, dicom->patientName);
        writeUtf8(H.PatientId, dicom->patientId);
        writeUtf8(H.Description, dicom->Description);
        writeUtf8(H.Sequence, dicom->Sequence);
        writeUtf8(H.SeriesNumber, dicom->SeriesNumber);
    }
    else 
    {

        H.IsNew3Dinfo = 0;

        // если dicom == nullptr, хотя бы оставим адекватные дефолты
        H.Doubheader[0] = H.Doubheader[1] = H.Doubheader[2] = 1.0;
        H.IsMRI = 0;
        H.Sex = 0;
    }

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

bool Save3DR::writemini3dr(const QString& path, vtkImageData* img, const DicomInfo* dicom, QString* error)
{
    if (!img) { if (error) *error = "Image is null"; return false; }

    // проверим тип
    if (img->GetScalarType() != VTK_UNSIGNED_CHAR ||
        img->GetNumberOfScalarComponents() != 1)
        return false;

    int ext[6];
    img->GetExtent(ext);

    int nx = ext[1] - ext[0] + 1;
    int ny = ext[3] - ext[2] + 1;
    int nz = ext[5] - ext[4] + 1;

    if (nx <= 0 || ny <= 0 || nz <= 0) { if (error) *error = "Invalid image dimensions"; return false; }

    const qint64 voxels = qint64(nx) * ny * nz;
    if (voxels <= 0) { if (error) *error = "Empty volume"; return false; }


    vtkIdType incX, incY, incZ;  // ВНИМАНИЕ: ИНКРЕМЕНТЫ В БАЙТАХ
    img->GetIncrements(incX, incY, incZ);

    // стартовый адрес (xmin, ymin, zmin)
    auto* p0 = static_cast<const uint8_t*>(
        img->GetScalarPointer(ext[0], ext[2], ext[4]));

    std::vector<uint8_t> out; 
    out.resize(size_t(voxels));
    memcpy(out.data(), p0, size_t(voxels));

    // --- Заголовок .3dr (64 байт) ---
    _mini3Dinfo H{};
    H.UIheader[0] = uint32_t(nx);
    H.UIheader[1] = uint32_t(ny);
    H.UIheader[2] = uint32_t(nz);

    H.Doubheader[0] = dicom->mSpX;
    H.Doubheader[1] = dicom->mSpZ;
    H.Doubheader[2] = dicom->mSpY;
        
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

    const qint64 bytes = voxels;
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

bool Save3DR::readmini3dr_into(const QString& path, vtkImageData* img, QString* error)
{
    if (!img) { if (error) *error = "Image is null"; return false; }

    // img должен быть уже создан и нужного типа
    if (img->GetScalarType() != VTK_UNSIGNED_CHAR || img->GetNumberOfScalarComponents() != 1) {
        if (error) *error = "VTK image type must be uint8 (VTK_UNSIGNED_CHAR) with 1 component";
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QString("Cannot open for read: %1").arg(path);
        return false;
    }

    if (f.size() < qint64(sizeof(_mini3Dinfo))) {
        if (error) *error = "File too small (no header)";
        return false;
    }

    // читаем заголовок
    _mini3Dinfo H{};
    if (f.read(reinterpret_cast<char*>(&H), qint64(sizeof(H))) != qint64(sizeof(H))) {
        if (error) *error = "Header read failed";
        return false;
    }

    const int fileNx = int(H.UIheader[0]);
    const int fileNy = int(H.UIheader[1]);
    const int fileNz = int(H.UIheader[2]);

    if (fileNx <= 0 || fileNy <= 0 || fileNz <= 0) {
        if (error) *error = "Invalid dimensions in header";
        return false;
    }

    // размеры img
    int ext[6];
    img->GetExtent(ext);
    const int nx = ext[1] - ext[0] + 1;
    const int ny = ext[3] - ext[2] + 1;
    const int nz = ext[5] - ext[4] + 1;

    if (nx != fileNx || ny != fileNy || nz != fileNz) {
        if (error) *error = QString("Size mismatch: img=%1x%2x%3, file=%4x%5x%6")
            .arg(nx).arg(ny).arg(nz).arg(fileNx).arg(fileNy).arg(fileNz);
        return false;
    }

    const qint64 voxels = qint64(nx) * ny * nz;
    const qint64 needSize = qint64(sizeof(_mini3Dinfo)) + voxels;
    if (f.size() < needSize) {
        if (error) *error = "File too small (not enough voxel data)";
        return false;
    }

    // проверяем, что можно делать один memcpy
    vtkIdType incX = 0, incY = 0, incZ = 0; // в байтах
    img->GetIncrements(incX, incY, incZ);

    // стартовый адрес (xmin, ymin, zmin)
    auto* p0 = static_cast<uint8_t*>(img->GetScalarPointer(ext[0], ext[2], ext[4]));
    if (!p0) {
        if (error) *error = "VTK scalar pointer is null";
        return false;
    }

    if (incX != 1 || incY != nx || incZ != vtkIdType(nx) * ny) {
        if (error) *error = "VTK image is not tightly packed; cannot memcpy as flat block";
        return false;
    }

    // читаем данные и кладём прямо в VTK-память
    const QByteArray data = f.read(voxels);
    if (data.size() != voxels) {
        if (error) *error = "Data read failed";
        return false;
    }

    memcpy(p0, data.constData(), size_t(voxels));
    return true;
}

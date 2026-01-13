#pragma once
#include <QString>

class QWidget;
class vtkImageData;
struct DicomInfo; // ваш существующий тип с .slope, .intercept (может быть nullptr)

namespace Save3DR {

	// Откроет диалог "Сохранить как", вернёт true при успехе.
	// dicom можно передать nullptr, тогда slope/intercept не пишутся.
	bool saveWithDialog(QWidget* parent, vtkImageData* img, const DicomInfo* dicom, QString& savepath);

	// Непосредственно пишет файл .3dr по пути path, без диалогов.
	// В случае ошибки вернёт false и, если error != nullptr, заполнит её текстом.
	bool write(const QString& path, vtkImageData* img, const DicomInfo* dicom, QString* error = nullptr);

	bool writemini3dr(const QString& path, vtkImageData* img, const DicomInfo* dicom, QString* error = nullptr);
	
	bool readmini3dr_into(const QString& path, vtkImageData* img, QString* error = nullptr);

} // namespace Save3DR

#include "ContentFilterProxy.h"
#include "DicomSniffer.h"

ContentFilterProxy::ContentFilterProxy(QObject* parent)
	: QSortFilterProxyModel(parent)
{
	setDynamicSortFilter(true);
	setFilterCaseSensitivity(Qt::CaseInsensitive);
}


bool ContentFilterProxy::filterAcceptsRow(int row, const QModelIndex& parent) const
{
	const auto* fsm = qobject_cast<const QFileSystemModel*>(sourceModel());
	if (!fsm) return true;


	const QModelIndex idx = fsm->index(row, 0, parent);
	const QFileInfo fi = fsm->fileInfo(idx);


	if (fi.isDir()) return true; // Папки оставляем для навигации


	const QString name = fi.fileName();


	switch (mode_) {
	case Volume3D:
		return name.endsWith(".3dr", Qt::CaseInsensitive);
	case DicomFiles:
		if (name.endsWith(".dcm", Qt::CaseInsensitive)) return true;
		if (name.endsWith(".dicom", Qt::CaseInsensitive)) return true;
		if (name.endsWith(".ima", Qt::CaseInsensitive)) return true;
		if (DicomSniffer::isDicomdirName(name)) return true;
	default:

		if (!checkMagic_) return false;


		const QString path = fi.absoluteFilePath();
		const QDateTime lm = fi.lastModified();
		auto it = cache_.find(path);
		if (it != cache_.end() && it->first == lm) return it->second;


		const bool ok = DicomSniffer::looksLikeDicomFile(path);
		cache_[path] = qMakePair(lm, ok);
		return ok;
	}
}
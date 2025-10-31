#pragma once
#ifndef CONTENTFILTERPROXY_h
#define CONTENTFILTERPROXY_h

#include "Pool.h"

class QFileSystemModel;


class ContentFilterProxy : public QSortFilterProxyModel {
	Q_OBJECT
public:
	enum Mode { DicomFiles, Volume3D };


	explicit ContentFilterProxy(QObject* parent = nullptr);


	void setMode(Mode m) { mode_ = m; invalidateFilter(); }
	void setCheckDicomMagic(bool on) { checkMagic_ = on; invalidateFilter(); }


protected:
	bool filterAcceptsRow(int row, const QModelIndex& parent) const override;


private:
	Mode mode_ = DicomFiles;
	bool checkMagic_ = true;
	mutable QHash<QString, QPair<QDateTime, bool>> cache_;
};

#endif
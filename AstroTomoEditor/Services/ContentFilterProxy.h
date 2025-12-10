#pragma once
#ifndef CONTENTFILTERPROXY_h
#define CONTENTFILTERPROXY_h

#include "Pool.h"

#include <QSortFilterProxyModel>
#include <QHash>
#include <QPair>
#include <QDateTime>
#include <QSet>

class QFileSystemModel;

class ContentFilterProxy : public QSortFilterProxyModel {
	Q_OBJECT
public:
	enum Mode { DicomFiles, Volume3D };


	explicit ContentFilterProxy(QObject* parent = nullptr);


	void setMode(Mode m) {
		mode_ = m;
		invalidateFilter();
	}

	void setCheckDicomMagic(bool on) {
		if (checkMagic_ == on)
			return;

		checkMagic_ = on;
		cache_.clear();
		resetMagicBudget();
		invalidateFilter();
	}

	void resetMagicBudget()
	{
		magicBudgetRemaining_ = maxMagicSyncPerDir_;
		pendingAsync_.clear();
	}


protected:
	bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private slots:
	void enqueueMagicCheck(const QString& path, const QDateTime& lm);

private:
	void resetMagicBudget() const {
		magicBudgetRemaining_ = maxMagicSyncPerDir_;
	}

	Mode mode_ = DicomFiles;
	bool checkMagic_ = false;
	mutable QHash<QString, QPair<QDateTime, bool>> cache_;

	mutable int magicBudgetRemaining_ = 0;
	static constexpr int maxMagicSyncPerDir_ = 32;

	mutable QSet<QString> pendingAsync_;
};

#endif
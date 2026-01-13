#pragma once
#ifndef CONTENTFILTERPROXY_h
#define CONTENTFILTERPROXY_h

#include <QSortFilterProxyModel>
#include <QHash>
#include <QPair>
#include <QDateTime>
#include <QSet>
#include <QTimer>

class QFileSystemModel;

class ContentFilterProxy : public QSortFilterProxyModel
{
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
        pendingAsync_.clear();
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex& parent) const override;

private slots:
    void enqueueMagicCheck(const QString& path, const QDateTime& lm);

private:
    void scheduleInvalidate();
    QVariant data(const QModelIndex& index, int role) const;

    Mode mode_ = DicomFiles;
    bool checkMagic_ = false;

    // path -> (lastModified, isDicom)
    mutable QHash<QString, QPair<QDateTime, bool>> cache_;

    // какие файлы уже поставлены в очередь
    mutable QSet<QString> pendingAsync_;

    // чтобы не спамить invalidateFilter каждый раз
    QTimer* refilterTimer_ = nullptr;

    // ограничим количество фоновых задач на каталог
    static constexpr int maxAsyncPerDir_ = 32;
};

#endif

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
        if (name.endsWith(".dcm", Qt::CaseInsensitive))  return true;
        if (name.endsWith(".dicom", Qt::CaseInsensitive)) return true;
        if (name.endsWith(".ima", Qt::CaseInsensitive))   return true;
        if (DicomSniffer::isDicomdirName(name))           return true;
        // дальше — только «магия»
        break;

    default:
        break;
    }

    // ---- сюда попали только если в режиме DicomFiles и расширение «левое» ----

    if (!checkMagic_)
        return false;

    const QString path = fi.absoluteFilePath();
    const QDateTime lm = fi.lastModified();

    // 1) смотрим кэш
    auto it = cache_.find(path);
    if (it != cache_.end() && it->first == lm)
        return it->second;

    // 2) если ещё есть бюджет — проверяем синхронно
    if (magicBudgetRemaining_ > 0) {
        --magicBudgetRemaining_;
        const bool ok = DicomSniffer::looksLikeDicomFile(path);
        cache_[path] = qMakePair(lm, ok);
        return ok;
    }

    // 3) бюджета нет — ставим в очередь фоновую проверку и пока скрываем
    if (!pendingAsync_.contains(path)) {
        pendingAsync_.insert(path);
        QMetaObject::invokeMethod(
            const_cast<ContentFilterProxy*>(this),
            "enqueueMagicCheck",
            Qt::QueuedConnection,
            Q_ARG(QString, path),
            Q_ARG(QDateTime, lm)
        );
    }

    return false;   // до прихода результата не показываем
}

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

void ContentFilterProxy::enqueueMagicCheck(const QString& path,
    const QDateTime& lm)
{
    auto* watcher = new QFutureWatcher<bool>(const_cast<ContentFilterProxy*>(this));

    connect(watcher, &QFutureWatcher<bool>::finished,
        this, [this, watcher, path, lm]() {
            const bool ok = watcher->result();
            watcher->deleteLater();

            pendingAsync_.remove(path);
            cache_[path] = qMakePair(lm, ok);
            invalidateFilter();   // пересчитать фильтрацию, файл появится/пропадёт
        });

    watcher->setFuture(QtConcurrent::run([path]() {
        return DicomSniffer::looksLikeDicomFile(path);
        }));
}

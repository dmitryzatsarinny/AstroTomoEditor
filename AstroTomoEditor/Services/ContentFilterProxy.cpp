#include "ContentFilterProxy.h"
#include "DicomSniffer.h"

#include <QFileSystemModel>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

ContentFilterProxy::ContentFilterProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    setFilterCaseSensitivity(Qt::CaseInsensitive);

    refilterTimer_ = new QTimer(this);
    refilterTimer_->setSingleShot(true);
    refilterTimer_->setInterval(256);
    connect(refilterTimer_, &QTimer::timeout, this, [this]() {
        invalidateFilter();
        });
}

QVariant ContentFilterProxy::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};

    // подмена отображаемого типа
    if (role == Qt::DisplayRole && index.column() == 2)
    {
        const QModelIndex src = mapToSource(index);

        auto* fsm = qobject_cast<QFileSystemModel*>(sourceModel());
        if (fsm)
        {
            const QString path = fsm->filePath(src);
            const QFileInfo fi(path);

#ifdef Q_OS_WIN
            // корень диска
            if (fi.isDir() && fi.isRoot())
                return tr("Drive");   // переведёшь как "Диск"
#endif
            if (fi.isDir())
                return tr("Folder");  // "Папка"

            // для файлов можно:
            // 1) пусто (как проводник иногда)
            // 2) расширение
            // 3) просто "File"
            if (fi.isFile())
                return tr("File");
        }
    }

    return QSortFilterProxyModel::data(index, role);
}


void ContentFilterProxy::scheduleInvalidate()
{
    if (!refilterTimer_)
        return;
    if (!refilterTimer_->isActive())
        refilterTimer_->start();
}

bool ContentFilterProxy::filterAcceptsRow(int row, const QModelIndex& parent) const
{
    const auto* fsm = qobject_cast<const QFileSystemModel*>(sourceModel());
    if (!fsm) return true;

    const QModelIndex idx = fsm->index(row, 0, parent);
    const QFileInfo fi = fsm->fileInfo(idx);

    if (fi.isDir())
        return true; // папки не фильтруем

    const QString name = fi.fileName();

    switch (mode_) {
    case Volume3D:
        return name.endsWith(".3dr", Qt::CaseInsensitive);

    case DicomFiles:
        if (name.endsWith(".dcm", Qt::CaseInsensitive))   return true;
        if (name.endsWith(".dicom", Qt::CaseInsensitive)) return true;
        if (name.endsWith(".ima", Qt::CaseInsensitive))   return true;
        if (DicomSniffer::isDicomdirName(name))           return true;
        // дальше только "магия"
        break;

    default:
        break;
    }

    // сюда попали, если: режим DicomFiles, расширение "левое"
    if (!checkMagic_)
        return false;

    const QString path = fi.absoluteFilePath();
    const QDateTime lm = fi.lastModified();

    // 1) смотрим кэш
    auto it = cache_.find(path);
    if (it != cache_.end() && it->first == lm)
        return it->second;

    // 2) ещё не проверяли – ставим в фон, если есть место в очереди
    if (pendingAsync_.size() < maxAsyncPerDir_ && !pendingAsync_.contains(path)) {
        pendingAsync_.insert(path);
        QMetaObject::invokeMethod(
            const_cast<ContentFilterProxy*>(this),
            "enqueueMagicCheck",
            Qt::QueuedConnection,
            Q_ARG(QString, path),
            Q_ARG(QDateTime, lm)
        );
    }

    // Пока результата нет – считаем не DICOM и не показываем
    return false;
}

void ContentFilterProxy::enqueueMagicCheck(const QString& path,
    const QDateTime& lm)
{
    auto* watcher = new QFutureWatcher<bool>(this);

    connect(watcher, &QFutureWatcher<bool>::finished,
        this, [this, watcher, path, lm]() {
            const bool ok = watcher->result();
            watcher->deleteLater();

            pendingAsync_.remove(path);
            cache_[path] = qMakePair(lm, ok);

            // Не перефильтровывать мгновенно каждый файл,
            // а чуть-чуть подождать и сделать один проход.
            scheduleInvalidate();
        });

    watcher->setFuture(QtConcurrent::run([path]() {
        return DicomSniffer::looksLikeDicomFile(path);
        }));
}

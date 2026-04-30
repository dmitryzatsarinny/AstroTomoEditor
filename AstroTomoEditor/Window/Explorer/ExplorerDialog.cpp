#include "ExplorerDialog.h"
#include "..\..\Services\ContentFilterProxy.h"
#include "..\..\Services\DicomSniffer.h"
#include <QCheckBox>
#include <QScrollBar>
#include "..\MainWindow\TitleBar.h"
#include <QStyledItemDelegate>
#include <Services/LanguageManager.h>
#include <QFutureWatcher>
#include <QStandardItemModel>
#include <QtConcurrent/QtConcurrentRun>

namespace
{
    constexpr int kManualPathRole = Qt::UserRole + 100;
    constexpr int kManualIsDirRole = Qt::UserRole + 101;
    constexpr int kManualIsParentRole = Qt::UserRole + 102;

    struct ManualEntry
    {
        QString name;
        QString path;
        QString sizeText;
        QString dateText;
        bool isDir = false;
        bool isParent = false;
    };

    QStringList manualHeaderLabels()
    {
        if (LanguageManager::instance().language() == "ru")
            return { QStringLiteral("Имя"), QStringLiteral("Размер"), QStringLiteral("Тип"), QStringLiteral("Дата изменения") };

        return { QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Type"), QStringLiteral("Date Modified") };
    }

    QString manualFileTypeText(bool isDir)
    {
        if (LanguageManager::instance().language() == "ru")
            return isDir ? QStringLiteral("Папка") : QStringLiteral("Файл");

        return isDir ? QStringLiteral("Folder") : QStringLiteral("File");
    }
}

static inline QString normPath(const QString& p)
{
    return QDir::cleanPath(QDir::toNativeSeparators(p));
}

ExplorerDialog::ExplorerDialog(QWidget* parent)
    : DialogShell(parent, tr("Astrocard DICOM Explorer"), WindowType::Explorer)   // ← вот так
{
    resize(900, 600);

    // ====== КОНТЕЙНЕР КОНТЕНТА ======
    QWidget* content = contentWidget();     // ← Берем пустой контейнер из DialogShell
    content->setObjectName("ExplorerContent");

    auto* mainLay = new QVBoxLayout(content);
    mainLay->setContentsMargins(12, 10, 12, 10);
    mainLay->setSpacing(8);

    // верхняя панель
    auto* top = new QHBoxLayout();
    m_driveCombo = new FixedDownComboBox(content);
    m_pathCombo = new QComboBox(content);
    m_pathCombo->setEditable(true);
    m_typeCombo = new FixedDownComboBox(content);
    m_magicCheck = new QCheckBox(tr("Deep сhecking"), content);
    m_magicCheck->setToolTip(tr("Check files without DICOM extension by signature"));
    m_magicCheck->setChecked(false);

    m_typeCombo->addItem(tr("DICOM"), int(ContentFilterProxy::DicomFiles));
    m_typeCombo->addItem(tr("3D Volume (*.3dr)"), int(ContentFilterProxy::Volume3D));

    top->addWidget(m_driveCombo, 0);
    top->addWidget(m_pathCombo, 1);
    top->addWidget(m_typeCombo, 0);
    top->addWidget(m_magicCheck, 0);
    mainLay->addLayout(top);

    // модель / прокси
    m_model = new QFileSystemModel(this);
    m_model->setFilter(QDir::AllDirs | QDir::NoDot | QDir::Files);
    m_model->setNameFilterDisables(false);
    m_model->setRootPath(QDir::rootPath());

    m_proxy = new ContentFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setDynamicSortFilter(true);
    m_proxy->setMode(ContentFilterProxy::DicomFiles);
    m_proxy->setCheckDicomMagic(m_magicCheck->isChecked());
    updateModelNameFilters();

    mManualModel = new QStandardItemModel(this);
    mManualModel->setColumnCount(4);
    updateManualHeaders();

    connect(m_magicCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_proxy)
            m_proxy->setCheckDicomMagic(on);
        updateModelNameFilters();

        const QString currentPath = mManualMode ? mManualRootPath : mCurrentRootPath;
        if (!currentPath.isEmpty())
            navigateTo(currentPath);
        });

    // дерево
    m_view = new FlatTreeView(content);
    m_view->setModel(m_proxy);
    m_view->setRootIndex(m_proxy->mapFromSource(m_model->index(QDir::rootPath())));
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(0, Qt::AscendingOrder);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setAlternatingRowColors(true);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_view->setItemDelegate(new NoFocusDelegate(m_view));
    m_view->setAllColumnsShowFocus(false);

    applyViewHeaderLayout();
    wireCurrentSelectionModel();

    mainLay->addWidget(m_view, 1);

    // ===== НИЖНЯЯ ЛИНИЯ: статус слева, кнопки справа =====
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, content);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));

    mStatusBar = new QWidget(content);
    mStatusBar->setObjectName("ExplorerStatusBar");
    auto* barLy = new QHBoxLayout(mStatusBar);
    barLy->setContentsMargins(0, 2, 0, 0);
    barLy->setSpacing(8);

    mBusy = new AsyncProgressBar(mStatusBar);
    mBusy->setVisible(false);      // по умолчанию спрячем
    mBusy->hideBar();              // внутренний режим Hidden
    mBusy->setFixedHeight(4);
    //mBusy->setObjectName("StatusProgress");

    QPalette pal = mBusy->palette();
    pal.setColor(QPalette::Base, QColor(0, 0, 0, 0));              // фон
    pal.setColor(QPalette::Window, QColor(0, 0, 0, 0));            // на всякий
    pal.setColor(QPalette::Highlight, QColor(230, 230, 230, 180)); // цвет "бегущей" полоски
    pal.setColor(QPalette::HighlightedText, Qt::white);
    mBusy->setPalette(pal);

    barLy->addWidget(mBusy, 0);
    barLy->addStretch();
    barLy->addWidget(m_buttons, 0);

    mainLay->addWidget(mStatusBar, 0);


    mOpenTimeout = new QTimer(this);
    mOpenTimeout->setSingleShot(true);
    connect(mOpenTimeout, &QTimer::timeout, this, [this] {
        setStatus(LoadState::Ready, tr("Ready"));
        });

    populateDrives();
#ifdef Q_OS_WIN
    navigateTo("C:/");
#else
    navigateTo(QDir::homePath());
#endif

    connect(m_model, &QFileSystemModel::directoryLoaded,
        this, &ExplorerDialog::onDirectoryLoaded);

    connect(m_model, &QFileSystemModel::rowsInserted, this,
        [this](const QModelIndex& parent, int, int) {
            const QString p = normPath(m_model->filePath(parent));
            Q_UNUSED(p);
        });

    connect(m_driveCombo, &QComboBox::currentIndexChanged,
        this, &ExplorerDialog::onDriveChanged);
    connect(m_pathCombo, &QComboBox::activated,
        this, &ExplorerDialog::onPathActivated);
    connect(m_pathCombo->lineEdit(), &QLineEdit::editingFinished,
        this, &ExplorerDialog::onPathEdited);
    connect(m_view, &QTreeView::doubleClicked,
        this, &ExplorerDialog::onDoubleClicked);
    connect(m_buttons, &QDialogButtonBox::accepted,
        this, &ExplorerDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected,
        this, &ExplorerDialog::reject);
    connect(m_typeCombo, &QComboBox::currentIndexChanged,
        this, &ExplorerDialog::onTypeChanged);
    connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
        this, [this] { retranslateUi(); });

    connect(this->titleBar(), &TitleBar::settingsClicked,
        this, &ExplorerDialog::showSettings);

    if (!mSettingsDlg)
        mSettingsDlg = new SettingsDialog(this, false);
    mSettingsDlg->hide();

    connect(mSettingsDlg, &SettingsDialog::languageChanged, this, [](const QString& code)
        {
            LanguageManager::instance().setLanguage(code);
        });


    updateOkState(false);

    // ===== СТИЛИ: список, скролл, комбобоксы, чекбокс, кнопки =====
    content->setStyleSheet(
        // фон зоны
        "#ExplorerContent {"
        "  background:transparent;"
        "}"

        // комбобоксы
        "QComboBox {"
        "  background:#26282c;"
        "  color:#f0f0f0;"
        "  border:1px solid rgba(255,255,255,0.25);"
        "  border-radius:4px;"
        "  padding:2px 22px 2px 6px;"
        "}"
        "QComboBox:hover {"
        "  background:#2c2f34;"
        "}"
        "QComboBox::drop-down {"
        "  border:none;"
        "  width:18px;"
        "}"
        "QComboBox::down-arrow {"
        "  image:none;"
        "}"

        // список
        "QTreeView {"
        "  background:transparent;"
        "  border:none;"
        "  color:#e6e6e6;"
        "  alternate-background-color:rgba(255,255,255,0.05);"
        "  selection-background-color:rgba(255,255,255,0.20);"
        "  selection-color:#ffffff;"
        "  show-decoration-selected:1;"
        "}"
        "QTreeView::item {"
        "  height:22px;"
        "  padding:1px 4px;"
        "}"
        "QTreeView::item:hover {"
        "  background:rgba(255,255,255,0.10);"
        "}"
        "QTreeView::item:selected:active {"
        "  background:rgba(255,255,255,0.20);"
        "}"
        "QTreeView::item:selected:!active {"
        "  background:rgba(255,255,255,0.20);"
        "}"
        "QTreeView::item:focus {"
        "  outline:none;"
        "}"

        // заголовок
        "QHeaderView::section {"
        "  background:#25262a;"
        "  color:#f0f0f0;"
        "  padding:4px 6px;"
        "  border:none;"
        "  border-bottom:1px solid rgba(255,255,255,0.12);"
        "}"

        // скроллбар
        "QScrollBar:vertical {"
        "  background:transparent;"
        "  width:10px;"
        "  margin:2px 0 2px 0;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background:rgba(255,255,255,0.30);"
        "  min-height:30px;"
        "  border-radius:4px;"
        "}"
        "QScrollBar::add-line:vertical,"
        "QScrollBar::sub-line:vertical {"
        "  height:0;"
        "  border:none;"
        "  background:transparent;"
        "}"
        "QScrollBar::add-page:vertical,"
        "QScrollBar::sub-page:vertical {"
        "  background:transparent;"
        "}"


        // чекбокс
        "QCheckBox {"
        "  color:#e6e6e6;"
        "}"
        "QCheckBox::indicator {"
        "  width:14px;"
        "  height:14px;"
        "}"
        "QCheckBox::indicator:unchecked {"
        "  border:1px solid rgba(255,255,255,0.5);"
        "  background:transparent;"
        "  border-radius:5px;"
        "}"
        "QCheckBox::indicator:checked {"
        "  background:rgba(255,255,255,0.6);"
        "  border:1px solid rgba(255,255,255,0.8);"
        "  border-radius:5px;"
        "}"

        // кнопки OK/Cancel
        "QDialogButtonBox QPushButton {"
        "  min-width:80px;"
        "  padding:4px 14px;"
        "  background:#2a2c30;"
        "  color:#f0f0f0;"
        "  border-radius:6px;"
        "  border:1px solid rgba(255,255,255,0.22);"
        "}"
        "QDialogButtonBox QPushButton:hover {"
        "  background:#32353a;"
        "}"
        "QDialogButtonBox QPushButton:pressed {"
        "  background:#3a3d44;"
        "}"
        "QDialogButtonBox QPushButton:disabled {"
        "  background:#25272b;"
        "  color:#777777;"
        "  border-color:rgba(255,255,255,0.10);"
        "}"
    );

    retranslateUi();
}

ExplorerDialog::~ExplorerDialog() = default;

void ExplorerDialog::updateManualHeaders()
{
    if (!mManualModel)
        return;

    mManualModel->setColumnCount(4);
    mManualModel->setHorizontalHeaderLabels(manualHeaderLabels());

    for (int row = 0; row < mManualModel->rowCount(); ++row)
    {
        auto* typeItem = mManualModel->item(row, 2);
        auto* nameItem = mManualModel->item(row, 0);
        if (!typeItem || !nameItem)
            continue;

        const bool isDir = nameItem->data(kManualIsDirRole).toBool();
        typeItem->setText(manualFileTypeText(isDir));
    }
}

void ExplorerDialog::applyViewHeaderLayout()
{
    if (!m_view || !m_view->header())
        return;

    auto* header = m_view->header();
    header->setStretchLastSection(false);

    if (mManualMode)
    {
        header->setSectionResizeMode(0, QHeaderView::Fixed);
        header->setSectionResizeMode(1, QHeaderView::Fixed);
        header->setSectionResizeMode(2, QHeaderView::Fixed);
        header->setSectionResizeMode(3, QHeaderView::Fixed);

        const QStringList labels = manualHeaderLabels();
        const QFontMetrics fm = m_view->fontMetrics();
        const int sizeWidth = std::max(90, fm.horizontalAdvance(labels.value(1)) + 28);
        const int typeWidth = std::max(76, fm.horizontalAdvance(labels.value(2)) + 28);
        const int dateWidth = std::max(136, fm.horizontalAdvance(labels.value(3)) + 28);
        const int viewportWidth = m_view->viewport() ? m_view->viewport()->width() : m_view->width();
        const int frameReserve = m_view->verticalScrollBar() && m_view->verticalScrollBar()->isVisible()
            ? m_view->verticalScrollBar()->width()
            : 0;
        const int nameWidth = std::max(180, viewportWidth - frameReserve - sizeWidth - typeWidth - dateWidth);

        m_view->setColumnWidth(0, nameWidth);
        m_view->setColumnWidth(1, sizeWidth);
        m_view->setColumnWidth(2, typeWidth);
        m_view->setColumnWidth(3, dateWidth);
        return;
    }

    header->setSectionResizeMode(0, QHeaderView::Stretch);
    header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
}

void ExplorerDialog::retranslateUi()
{
    const QString title = tr("Astrocard DICOM Explorer");
    setWindowTitle(title);
    if (titleBar())
        titleBar()->setTitle(title);


    if (m_buttons) 
    {
        m_buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));
        m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    }

    if (m_magicCheck) 
    {
        m_magicCheck->setText(tr("Deep checking"));
        m_magicCheck->setToolTip(tr("Check files without DICOM extension by signature"));
    }

    if (mSettingsDlg)
        mSettingsDlg->retranslateUi();

    updateManualHeaders();
    applyViewHeaderLayout();

    if (m_typeCombo)
    {
        const int prevMode = m_typeCombo->currentData().toInt();

        // обновляем тексты существующих пунктов, не трогая userData
        for (int i = 0; i < m_typeCombo->count(); ++i)
        {
            const auto mode = static_cast<ContentFilterProxy::Mode>(m_typeCombo->itemData(i).toInt());
            switch (mode)
            {
            case ContentFilterProxy::DicomFiles:
                m_typeCombo->setItemText(i, tr("DICOM"));
                break;
            case ContentFilterProxy::Volume3D:
                m_typeCombo->setItemText(i, tr("3D Volume (*.3dr)"));
                break;
            default:
                break;
            }
        }

        // восстановим выбранный пункт по mode (на всякий)
        const int idx = m_typeCombo->findData(prevMode);
        if (idx >= 0) m_typeCombo->setCurrentIndex(idx);
    }

    // диски (их проще пересоздать, потому что label = tr("Disk %1"))
    if (m_driveCombo)
    {
        const QString prevRoot = m_driveCombo->currentData().toString();

        populateDrives();

        const int idx = m_driveCombo->findData(prevRoot);
        if (idx >= 0) m_driveCombo->setCurrentIndex(idx);
    }
}


void ExplorerDialog::setRootPath(const QString& path)
{
    navigateTo(path);                        // Публичный метод-обёртка: перейти к указанному пути.
}

void ExplorerDialog::setNameFilters(const QStringList& filters) {
    Q_UNUSED(filters);
}

QString ExplorerDialog::filePathFromViewIndex(const QModelIndex& viewIdx) const {
    if (!viewIdx.isValid()) return {};
    if (mManualMode)
        return viewIdx.sibling(viewIdx.row(), 0).data(kManualPathRole).toString();

    const QModelIndex src = m_proxy->mapToSource(viewIdx);
    return m_model->filePath(src);
}

QString ExplorerDialog::selectedFile() const {
    if (selectedKind() == SelectionKind::DicomFile ||
        selectedKind() == SelectionKind::DicomDir ||
        selectedKind() == SelectionKind::File3DR) {
        return selectedPath();            // это файл
    }
    return {};                            // для папки — пусто
}


QStringList ExplorerDialog::selectedFiles() const {
    // остаётся как было; для папок возвращаем пусто.
    QStringList out;
    if (!selectedFile().isEmpty()) out << selectedFile();
    return out;
}

void ExplorerDialog::onTypeChanged(int index)
{
    const auto mode = static_cast<ContentFilterProxy::Mode>(m_typeCombo->itemData(index).toInt());
    m_proxy->setMode(mode);
    if (m_magicCheck) {
        const bool enableMagic = (mode == ContentFilterProxy::DicomFiles);
        m_magicCheck->setEnabled(enableMagic);
        if (!enableMagic && m_magicCheck->isChecked())
            m_magicCheck->setChecked(false);
        if (enableMagic)
            m_proxy->setCheckDicomMagic(m_magicCheck->isChecked());
    }
    updateModelNameFilters();
}

void ExplorerDialog::applyFiltersForType()
{
    // Достаём из userData набор масок (QStringList), который
    // мы положили при добавлении пунктов в комбобокс.
    const QStringList filters = m_typeCombo->currentData().toStringList();

    if (filters.isEmpty()) {
        // показать всё (если когда-нибудь сделаешь пункт "Все файлы")
        m_model->setNameFilters(QStringList());
    }
    else {
        m_model->setNameFilters(filters);
    }
    m_model->setNameFilterDisables(false); // скрывать неподходящее
}

void ExplorerDialog::updateModelNameFilters()
{
    if (!m_model || !m_typeCombo)
        return;

    const auto mode = static_cast<ContentFilterProxy::Mode>(
        m_typeCombo->currentData().toInt());

    QStringList filters;
    if (mode == ContentFilterProxy::Volume3D)
    {
        filters << QStringLiteral("*.3dr");
    }
    else if (!m_magicCheck || !m_magicCheck->isChecked())
    {
        filters << QStringLiteral("*.dcm")
            << QStringLiteral("*.dicom")
            << QStringLiteral("*.ima")
            << QStringLiteral("DICOMDIR")
            << QStringLiteral("DIRFILE");
    }

    m_model->setNameFilters(filters);
    m_model->setNameFilterDisables(false);
}

void ExplorerDialog::wireCurrentSelectionModel()
{
    if (!m_view || !m_view->selectionModel())
        return;

    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, &ExplorerDialog::onSelectionChanged,
        Qt::UniqueConnection);
}

void ExplorerDialog::populateDrives()
{
    m_driveCombo->clear();                 // Чистим список дисков.
    for (const QFileInfo& drv : QDir::drives()) {       // QDir::drives() — список корневых томов.
        const QString root = drv.absoluteFilePath();    // Например, "C:/".
        QString label = root;                           // Метка по умолчанию — сам путь.
#ifdef Q_OS_WIN
        if (root.size() >= 2 && root[1] == ':')
            label = tr("Disk %1").arg(root.left(2));    // Красивее: "Диск C:".
#endif
        m_driveCombo->addItem(label, root);             // Текст в UI и userData = сам путь.
    }
}

bool ExplorerDialog::isDicomFile(const QString& filePath) const {
    const QFileInfo fi(filePath);
    if (!fi.isFile())
        return false;
    const QString name = fi.fileName();
    if (name.endsWith(".dcm", Qt::CaseInsensitive))
        return true;
    return DicomSniffer::looksLikeDicomFile(filePath);
}

bool ExplorerDialog::is3drFile(const QString& filePath) const {
    return QFileInfo(filePath).suffix().compare("3dr", Qt::CaseInsensitive) == 0;
}

bool ExplorerDialog::dirHasSdir(const QString& dirPath, int maxProbe) const {
    QDir d(dirPath);
    if (!d.exists()) return false;

    // Быстрый проход: смотрим не более maxProbe файлов
    QDirIterator it(dirPath, QDir::Dirs | QDir::NoDotAndDotDot);
    int checked = 0;

    while (it.hasNext() && checked < std::max(1, maxProbe)) {
        const QString p = it.next();
        const QString name = QFileInfo(p).fileName();

        // Дешёвая эвристика по расширению
        if (!name.startsWith("S", Qt::CaseInsensitive))
            return false;

        ++checked;
    }
    return true;
}

bool ExplorerDialog::dirHasDicom(const QString& dirPath, int maxProbe) const {
    QDir d(dirPath);
    if (!d.exists()) return false;

    // Быстрый проход: смотрим не более maxProbe файлов
    QDirIterator it(dirPath, QDir::Files | QDir::NoDotAndDotDot);
    int checked = 0;

    while (it.hasNext() && checked < std::max(1, maxProbe)) {
        const QString p = it.next();
        const QString name = QFileInfo(p).fileName();

        // Дешёвая эвристика по расширению
        if (name.endsWith(".dcm", Qt::CaseInsensitive) ||
            name.endsWith(".dicom", Qt::CaseInsensitive) ||
            name.endsWith(".ima", Qt::CaseInsensitive))
            return true;


        // Точная проверка по сигнатуре (дороже, но мы её ограничили maxProbe)
        if (DicomSniffer::looksLikeDicomFile(p))
            return true;

        ++checked;
    }
    return false;
}

bool ExplorerDialog::dirHasDicomdir(const QString& dirPath) const {
    QDir d(dirPath);
    return d.exists("DICOMDIR") || d.exists("dicomdir") || d.exists("dirfile") || d.exists("DIRFILE");
}

bool ExplorerDialog::shouldUseManualListing(const QString& dirPath) const
{
    if (!m_typeCombo || !m_magicCheck)
        return false;

    const auto mode = static_cast<ContentFilterProxy::Mode>(
        m_typeCombo->currentData().toInt());
    if (mode != ContentFilterProxy::DicomFiles)
        return false;

    QDir d(dirPath);
    if (!d.exists())
        return false;

    int extensionlessFiles = 0;
    QDirIterator it(dirPath, QDir::Files | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        it.next();
        const QString suffix = it.fileInfo().suffix();
        if (suffix.isEmpty() && ++extensionlessFiles >= 128)
            return true;
    }

    return false;
}

void ExplorerDialog::navigateToManual(const QString& path)
{
    const QString want = normPath(path);
    QFileInfo fi(want);
    if (!fi.isDir())
        return;

    ++mDirProbeGeneration;
    ++mManualGeneration;
    mPendingDirProbes.clear();
    mManualMode = true;
    mManualRootPath = normPath(fi.absoluteFilePath());
    mCurrentRootPath = mManualRootPath;

    onDirectoryAboutToChange(mManualRootPath);

    if (m_view->model() != mManualModel) {
        m_view->setModel(mManualModel);
        wireCurrentSelectionModel();
    }
    m_view->setSortingEnabled(false);
    m_view->setRootIndex(QModelIndex());
    applyViewHeaderLayout();
    QTimer::singleShot(0, this, [this] { applyViewHeaderLayout(); });

    rebuildManualModel(mManualRootPath, mManualGeneration);

    int existing = m_pathCombo->findText(mManualRootPath, Qt::MatchFixedString);
    if (existing < 0) m_pathCombo->insertItem(0, mManualRootPath);
    m_pathCombo->setCurrentText(mManualRootPath);

    for (int i = 0; i < m_driveCombo->count(); ++i) {
        const QString root = m_driveCombo->itemData(i).toString();
        if (mManualRootPath.startsWith(root, Qt::CaseInsensitive)) {
            m_driveCombo->setCurrentIndex(i);
            break;
        }
    }

    updateOkState(false);
}

void ExplorerDialog::rebuildManualModel(const QString& path, int generation)
{
    if (!mManualModel)
        return;

    mManualModel->clear();
    mManualModel->setColumnCount(4);
    updateManualHeaders();

    auto addRow = [this](const ManualEntry& e)
    {
        QList<QStandardItem*> row;
        row << new QStandardItem(e.name)
            << new QStandardItem(e.sizeText)
            << new QStandardItem(manualFileTypeText(e.isDir))
            << new QStandardItem(e.dateText);

        for (auto* item : row) {
            item->setEditable(false);
            item->setData(e.path, kManualPathRole);
            item->setData(e.isDir, kManualIsDirRole);
            item->setData(e.isParent, kManualIsParentRole);
        }

        mManualModel->appendRow(row);
    };

    const QFileInfo rootInfo(path);
    const QString parentPath = rootInfo.dir().absolutePath();
    if (!parentPath.isEmpty() && parentPath != path) {
        ManualEntry parent;
        parent.name = QStringLiteral("..");
        parent.path = parentPath;
        parent.isDir = true;
        parent.isParent = true;
        addRow(parent);
    }

    auto* watcher = new QFutureWatcher<QVector<ManualEntry>>(this);
    connect(watcher, &QFutureWatcher<QVector<ManualEntry>>::finished, this,
        [this, watcher, generation, addRow]()
        {
            const QVector<ManualEntry> entries = watcher->result();
            watcher->deleteLater();

            if (!mManualMode || generation != mManualGeneration)
                return;

            for (const auto& e : entries)
                addRow(e);

            applyViewHeaderLayout();
            QTimer::singleShot(0, this, [this] { applyViewHeaderLayout(); });
            setStatus(LoadState::Ready, tr("Ready"));
        });

    const bool deepCheck = m_magicCheck && m_magicCheck->isChecked();
    watcher->setFuture(QtConcurrent::run([path, deepCheck]() {
        QVector<ManualEntry> entries;
        QDir dir(path);
        if (!dir.exists())
            return entries;

        const QFileInfoList infos = dir.entryInfoList(
            QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

        entries.reserve(infos.size());
        for (const QFileInfo& fi : infos) {
            if (fi.isFile()) {
                const QString name = fi.fileName();
                const bool hasDicomName =
                    name.endsWith(".dcm", Qt::CaseInsensitive) ||
                    name.endsWith(".dicom", Qt::CaseInsensitive) ||
                    name.endsWith(".ima", Qt::CaseInsensitive) ||
                    DicomSniffer::isDicomdirName(name);

                if (!hasDicomName && (!deepCheck || !DicomSniffer::looksLikeDicomFile(fi.absoluteFilePath())))
                    continue;
            }

            ManualEntry e;
            e.name = fi.fileName();
            e.path = fi.absoluteFilePath();
            e.isDir = fi.isDir();
            e.sizeText = fi.isDir() ? QString() : QString::number(fi.size());
            e.dateText = fi.lastModified().toString(QStringLiteral("dd.MM.yyyy hh:mm"));
            entries.push_back(std::move(e));
        }

        return entries;
        }));
}

void ExplorerDialog::scheduleDirProbe(const QString& dirPath) const
{
    const QString path = normPath(dirPath);
    if (path.isEmpty() ||
        mDirKindCache.contains(path) ||
        mPendingDirProbes.contains(path))
        return;

    mPendingDirProbes.insert(path);
    const int generation = mDirProbeGeneration;
    auto* self = const_cast<ExplorerDialog*>(this);
    auto* watcher = new QFutureWatcher<SelectionKind>(self);

    connect(watcher, &QFutureWatcher<SelectionKind>::finished, self,
        [this, self, watcher, path, generation]()
        {
            const SelectionKind kind = watcher->result();
            watcher->deleteLater();

            if (generation != mDirProbeGeneration)
                return;

            mPendingDirProbes.remove(path);
            mDirKindCache.insert(path, kind);

            const auto rows = m_view && m_view->selectionModel()
                ? m_view->selectionModel()->selectedRows(0)
                : QModelIndexList{};
            if (!rows.isEmpty() && normPath(filePathFromViewIndex(rows.first())).compare(path, Qt::CaseInsensitive) == 0)
                self->updateOkState(true);
        });

    watcher->setFuture(QtConcurrent::run([path]() {
        QDir d(path);
        if (!d.exists())
            return ExplorerDialog::SelectionKind::None;

        if (d.exists("DICOMDIR") || d.exists("dicomdir") ||
            d.exists("dirfile") || d.exists("DIRFILE"))
            return ExplorerDialog::SelectionKind::DicomFolder;

        QDirIterator files(path, QDir::Files | QDir::NoDotAndDotDot);
        int checkedFiles = 0;
        while (files.hasNext() && checkedFiles < 10) {
            const QString p = files.next();
            const QString name = QFileInfo(p).fileName();
            if (name.endsWith(".dcm", Qt::CaseInsensitive) ||
                name.endsWith(".dicom", Qt::CaseInsensitive) ||
                name.endsWith(".ima", Qt::CaseInsensitive) ||
                DicomSniffer::looksLikeDicomFile(p))
                return ExplorerDialog::SelectionKind::DicomFolder;
            ++checkedFiles;
        }

        QDirIterator dirs(path, QDir::Dirs | QDir::NoDotAndDotDot);
        int checkedDirs = 0;
        bool sawDir = false;
        while (dirs.hasNext() && checkedDirs < 10) {
            sawDir = true;
            const QString name = QFileInfo(dirs.next()).fileName();
            if (!name.startsWith("S", Qt::CaseInsensitive))
                return ExplorerDialog::SelectionKind::None;
            ++checkedDirs;
        }

        return sawDir ? ExplorerDialog::SelectionKind::DicomFolder : ExplorerDialog::SelectionKind::None;
        }));
}

void ExplorerDialog::navigateTo(const QString& path)
{
    qDebug() << "Go to dir: " << path;

    const QString want = normPath(path);
    if (shouldUseManualListing(want)) {
        navigateToManual(want);
        return;
    }

    ++mDirProbeGeneration;
    mPendingDirProbes.clear();
    mManualMode = false;
    mManualRootPath.clear();

    onDirectoryAboutToChange(path); // теперь реально включает "Opening" прямо сейчас

    if (m_view->model() != m_proxy) {
        m_view->setModel(m_proxy);
        wireCurrentSelectionModel();
        applyViewHeaderLayout();
        m_view->setSortingEnabled(true);
        m_view->sortByColumn(0, Qt::AscendingOrder);
    }

    QModelIndex src = m_model->setRootPath(want);
    
    if (!src.isValid()) { setStatus(LoadState::Ready, tr("Ready")); return; }

    m_view->setRootIndex(m_proxy->mapFromSource(src));
    mCurrentRootPath = normPath(m_model->filePath(src));

    // обновляем путь в комбобоксе (история)
    const QString abspath = mCurrentRootPath;
    int existing = m_pathCombo->findText(abspath, Qt::MatchFixedString);
    if (existing < 0) m_pathCombo->insertItem(0, abspath);
    m_pathCombo->setCurrentText(abspath);

    // выбрать диск
    for (int i = 0; i < m_driveCombo->count(); ++i) {
        const QString root = m_driveCombo->itemData(i).toString();
        if (abspath.startsWith(root, Qt::CaseInsensitive)) {
            m_driveCombo->setCurrentIndex(i);
            break;
        }
    }

    updateOkState(false);
}

void ExplorerDialog::showBusy(const QString& text)
{
    Q_UNUSED(text);
    if (!mBusy)
        return;

    mBusy->setVisible(true);
    mBusy->startLoading();
}

void ExplorerDialog::hideBusy()
{
    if (!mBusy)
        return;

    mBusy->hideBar();
    mBusy->setVisible(false);
}

void ExplorerDialog::onDirectoryAboutToChange(const QString& path)
{
    mPendingPath = normPath(path);
    setStatus(LoadState::Opening, tr("Opening: %1").arg(mPendingPath));
}

void ExplorerDialog::onDirectoryLoaded(const QString& path)
{
    qDebug() << "Dir Loaded: " << path;
    setStatus(LoadState::Ready, tr("Ready"));
    const QString loaded = normPath(path);

    if (!mCurrentRootPath.isEmpty() && loaded.compare(mCurrentRootPath, Qt::CaseInsensitive) == 0) 
    {
        mPendingPath.clear();
        hideBusy();
    }
}

void ExplorerDialog::onDriveChanged(int index)
{
    const QString root = m_driveCombo->itemData(index).toString(); // Берём путь из userData.
    if (!root.isEmpty())
        navigateTo(root);                  // Переходим в корень выбранного диска.
}

void ExplorerDialog::onPathActivated(int index)
{
    Q_UNUSED(index);                       // Мы берём текст напрямую ниже; индекс не нужен.
    navigateTo(m_pathCombo->currentText()); // Переход к выбранному в истории/введённому пути.
}

void ExplorerDialog::onPathEdited()
{
    navigateTo(m_pathCombo->currentText()); // Enter в поле пути -> перейти.
}

void ExplorerDialog::onDoubleClicked(const QModelIndex& vIdx)
{
    if (!vIdx.isValid()) return;
    hideBusy();

    const QString path = filePathFromViewIndex(vIdx);
    const QFileInfo fi(path);

    if (fi.isDir()) {
        navigateTo(path);
        return;
    }

    // файл — если OK сейчас разрешён, закрываем диалог
    auto* okBtn = m_buttons->button(QDialogButtonBox::Ok);
    if (fi.isFile() && okBtn && okBtn->isEnabled()) {
        accept();
    }
}


void ExplorerDialog::onSelectionChanged(const QItemSelection&, const QItemSelection&)
{
    auto sm = m_view->selectionModel();
    if (!sm || !sm->hasSelection()) {
        updateOkState(false);
        return;
    }
    updateOkState(true);
}

void ExplorerDialog::updateOkState(const bool state) 
{
    bool enable = state;
    if (enable)
    switch (selectedKind()) 
    {
    case ExplorerDialog::SelectionKind::DicomFile:
        break;
    case ExplorerDialog::SelectionKind::DicomDir:
        break;
    case ExplorerDialog::SelectionKind::File3DR:
        break;
    case ExplorerDialog::SelectionKind::DicomFolder:
        break;
    default:
        enable = false;
        break;
    }

    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(enable);
}

ExplorerDialog::SelectionKind ExplorerDialog::selectedKind() const {
    const auto rows = m_view->selectionModel()->selectedRows(0);
    if (rows.isEmpty()) return SelectionKind::None;

    const QString path = filePathFromViewIndex(rows.first());
    const QFileInfo fi(path);

    // Текущий режим из комбобокса
    const auto mode = static_cast<ContentFilterProxy::Mode>(
        m_typeCombo->currentData().toInt());

    if (fi.isFile())
    {
        if (mode == ContentFilterProxy::DicomFiles &&
            DicomSniffer::isDicomdirName(fi.fileName()))
            return SelectionKind::DicomDir;
        if (mode == ContentFilterProxy::Volume3D && is3drFile(path))
            return SelectionKind::File3DR;
        if (mode == ContentFilterProxy::DicomFiles && isDicomFile(path))
            return SelectionKind::DicomFile;
        return SelectionKind::None;
    }

    if (fi.isDir()) 
    {
        if (mode == ContentFilterProxy::DicomFiles)
        {
            const QString key = normPath(path);
            const auto it = mDirKindCache.constFind(key);
            if (it != mDirKindCache.constEnd())
                return it.value();

            scheduleDirProbe(path);
        }
    }

    return SelectionKind::None;
}

QString ExplorerDialog::selectedPath() const {
    const auto rows = m_view->selectionModel()->selectedRows(0);
    if (rows.isEmpty()) return {};
    return filePathFromViewIndex(rows.first());
}

void ExplorerDialog::setStatus(LoadState st, const QString& text)
{
    Q_UNUSED(text);

    mState = st;
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);

    if (!mStatusBar)
        return;

    if (st == LoadState::Opening) {
        if (mBusy) {
            mBusy->setVisible(true);
            mBusy->startLoading();
        }
        mStatusBar->show();

        if (mOpenTimeout)
            mOpenTimeout->start(5000);
    }
    else 
    {
        if (mOpenTimeout)
            mOpenTimeout->stop();

        mPendingPath.clear();

        if (mBusy) {
            mBusy->hideBar();
            mBusy->setVisible(false);
        }

        mStatusBar->show();
    }
}

void ExplorerDialog::showSettings()
{
    if (!mSettingsDlg)
        mSettingsDlg = new SettingsDialog(this, false);


    mSettingsDlg->show();
    mSettingsDlg->raise();
    mSettingsDlg->activateWindow();

    // Центрируем относительно главного окна
    const QRect r = geometry();
    const QSize s = mSettingsDlg->size();
    mSettingsDlg->move(r.center() - QPoint(s.width() / 2, s.height() / 2 + 40));
}

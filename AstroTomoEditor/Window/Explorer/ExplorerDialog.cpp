#include "ExplorerDialog.h"
#include "..\..\Services\ContentFilterProxy.h"
#include "..\..\Services\DicomSniffer.h"
#include <QCheckBox>

static inline QString normPath(const QString& p)
{
    return QDir::cleanPath(QDir::toNativeSeparators(p));
}

ExplorerDialog::ExplorerDialog(QWidget* parent)
    : QDialog(parent)                                       // Базовый конструктор QDialog; parent задаёт владение.
{
    setWindowTitle(tr("Astrocard DICOM Explorer"));         // Заголовок окна (через tr() — пригодно для перевода).
    resize(900, 600);                                       // Стартовый размер диалога.

    QIcon appIcon(":/icons/Resources/dicom_heart.ico");
    setWindowIcon(appIcon);
    
    auto* mainLay = new QVBoxLayout(this);                  // Главный вертикальный лэйаут; parent = this.

    // Верхняя панель: список дисков + текущий путь
    auto* top = new QHBoxLayout();                          // Горизонтальный лэйаут для комбобоксов.
    m_driveCombo = new QComboBox(this);                     // Комбо со списком дисков.
    m_pathCombo = new QComboBox(this);                      // Комбо с историей путей/вводом пути.
    m_pathCombo->setEditable(true);                         // Разрешаем ручной ввод пути (lineEdit внутри).
    m_typeCombo = new QComboBox(this);                      // Комбо со списком типов.
    m_magicCheck = new QCheckBox(tr("Deep Checking"), this); // Опция глубокой проверки.
    m_magicCheck->setToolTip(tr("Check files without DICOM extension by signature."));
    m_magicCheck->setChecked(false);

    m_typeCombo->addItem(tr("DICOM"), int(ContentFilterProxy::DicomFiles));
    m_typeCombo->addItem(tr("3D Volume (*.3dr)"), int(ContentFilterProxy::Volume3D));

    top->addWidget(m_driveCombo, 0);                        // Добавить комбо дисков (столбец ширины по контенту).
    top->addWidget(m_pathCombo, 1);                         // Путь занимает оставшееся место (stretch = 1).
    top->addWidget(m_typeCombo, 0);                         // Добавить комбо типов (столбец ширины по контенту).
    top->addWidget(m_magicCheck, 0);                        // Чекбокс глубокой проверки
    mainLay->addLayout(top);                                // Вставить верхнюю панель в главный лэйаут.

    // Модель файловой системы
    m_model = new QFileSystemModel(this);                   // Создаём модель; parent = this для автоудаления.
    m_model->setFilter(                                     // Какие элементы показывать:
        QDir::AllDirs                                       //  - папки,
        | QDir::NoDot                                       //  - без "." и "..",
        | QDir::Files);                                     //  - и файлы.
    m_model->setRootPath(QDir::rootPath());                 // Устанавливаем корневой путь (подгрузка начнётся оттуда).

    // Прокси-фильтр поверх файловой модели
    m_proxy = new ContentFilterProxy(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setDynamicSortFilter(true);
    m_proxy->setMode(ContentFilterProxy::DicomFiles);
    m_proxy->setCheckDicomMagic(m_magicCheck->isChecked());

    connect(m_magicCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_proxy)
            m_proxy->setCheckDicomMagic(on);
        });


    // Представление — как в Проводнике: таблица с колонками
    m_view = new QTreeView(this);
    m_view->setModel(m_proxy);
    m_view->setRootIndex(m_proxy->mapFromSource(m_model->index(QDir::rootPath())));

    m_view->setSortingEnabled(true);          // Включаем сортировку по клику на заголовок.
    m_view->sortByColumn(0, Qt::AscendingOrder); // Первичная сортировка по имени.
    m_view->setSelectionMode(                 // Разрешаем выбирать только один элемент.
        QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(             // Выбор целой строки (а не отдельной ячейки).
        QAbstractItemView::SelectRows);
    m_view->setAlternatingRowColors(true);    // Чередующиеся цвета строк (визуально удобней).
    m_view->setEditTriggers(                  // Запрещаем редактирование (двойной клик не редактирует).
        QAbstractItemView::NoEditTriggers);
    m_view->header()->setStretchLastSection(false); // Последняя колонка не растягивается автоматически.
    m_view->header()->setSectionResizeMode(
        0, QHeaderView::Stretch);             // Колонка 0 ("Имя") тянется, заполняя ширину.
    m_view->header()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);    // "Дата" по содержимому.
    m_view->header()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);    // "Тип" по содержимому.
    m_view->header()->setSectionResizeMode(
        3, QHeaderView::ResizeToContents);    // "Размер" по содержимому.

    mainLay->addWidget(m_view, 1);            // Добавляем вид в основной лэйаут (stretch = 1).

    // Кнопки
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("OK"));     // Локализуем надписи.
    m_buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
    mainLay->addWidget(m_buttons);            // Вставляем блок кнопок в низ диалога.

    // ----- НИЖНИЙ "status bar" -----
    mStatusBar = new QWidget(this);
    mStatusBar->setObjectName("ExplorerStatusBar");
    auto* barLy = new QHBoxLayout(mStatusBar);
    barLy->setContentsMargins(8, 4, 8, 4);
    barLy->setSpacing(8);

    mStatusText = new QLabel(tr("Loading..."), mStatusBar);
    mBusy = new QProgressBar(mStatusBar);
    mBusy->setRange(0, 0);              // «неопределённый» прогресс (анимация)
    mBusy->setTextVisible(false);
    mBusy->setFixedHeight(8);

    barLy->addWidget(mStatusText, /*stretch*/0);
    barLy->addWidget(mBusy,       /*stretch*/1);

    mStatusBar->show();
    mStatusText->clear();

    if (auto* mainLy = qobject_cast<QVBoxLayout*>(layout())) 
    {
        int i = mainLy->indexOf(m_buttons);
        if (i < 0) 
            i = mainLy->count();
        mainLy->insertWidget(i, mStatusBar);
    }
    else 
    {
        auto* v = new QVBoxLayout(this);
        v->addWidget(mStatusBar);
        setLayout(v);
    }

    mOpenTimeout = new QTimer(this);
    mOpenTimeout->setSingleShot(true);
    connect(mOpenTimeout, &QTimer::timeout, this, [this] {
        setStatus(LoadState::Ready, tr("Ready")); // fail-safe
        });

    // Наполняем диски и выставляем стартовый путь
    populateDrives();                         // Собираем список логических дисков и заполняем m_driveCombo.
#ifdef Q_OS_WIN
    navigateTo("C:/");                        // На Windows — переходим в C:/
#else
    navigateTo(QDir::homePath());             // На *nix — в домашнюю директорию пользователя.
#endif

    connect(m_model, &QFileSystemModel::directoryLoaded,
        this, &ExplorerDialog::onDirectoryLoaded);     // Смена диска -> перейти к корню этого диска.

    connect(m_model, &QFileSystemModel::rowsInserted, this,
        [this](const QModelIndex& parent, int, int) {
            const QString p = normPath(m_model->filePath(parent));
        });

    connect(m_driveCombo, &QComboBox::currentIndexChanged,
        this, &ExplorerDialog::onDriveChanged);     // Смена диска -> перейти к корню этого диска.

    connect(m_pathCombo, &QComboBox::activated,
        this, &ExplorerDialog::onPathActivated); // Выбор элемента из истории путей.

    connect(m_pathCombo->lineEdit(), &QLineEdit::editingFinished,
        this, &ExplorerDialog::onPathEdited);    // Нажали Enter в поле пути.

    connect(m_view, &QTreeView::doubleClicked,
        this, &ExplorerDialog::onDoubleClicked); // Двойной клик: войти в папку или принять файл.

    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged,
        this, &ExplorerDialog::onSelectionChanged); // Любое изменение выбора.

    connect(m_buttons, &QDialogButtonBox::accepted,
        this, &ExplorerDialog::accept);        // Ok -> accept() (exec() вернёт Accepted).
    connect(m_buttons, &QDialogButtonBox::rejected,
        this, &ExplorerDialog::reject);        // Cancel -> reject().

    connect(m_typeCombo, &QComboBox::currentIndexChanged,
        this, &ExplorerDialog::onTypeChanged);

    updateOkState();                         // Включить/выключить Ok в соответствии с текущим выбором.
}

ExplorerDialog::~ExplorerDialog() = default;

void ExplorerDialog::setRootPath(const QString& path)
{
    navigateTo(path);                        // Публичный метод-обёртка: перейти к указанному пути.
}

void ExplorerDialog::setNameFilters(const QStringList& filters) {
    Q_UNUSED(filters);
}

QString ExplorerDialog::filePathFromViewIndex(const QModelIndex& viewIdx) const {
    if (!viewIdx.isValid()) return {};
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

void ExplorerDialog::navigateTo(const QString& path)
{
    qDebug() << "Go to dir: " << path;

    onDirectoryAboutToChange(path); // теперь реально включает "Opening" прямо сейчас

    const QString want = normPath(path);
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

    updateOkState();
}

void ExplorerDialog::showBusy(const QString& text)
{
    mStatusText->setText(text);
}

void ExplorerDialog::hideBusy()
{
    mStatusText->clear();
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

    if (!mCurrentRootPath.isEmpty() && loaded == mCurrentRootPath) {
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

void ExplorerDialog::onDoubleClicked(const QModelIndex& vIdx) {
    if (!vIdx.isValid()) return;
    hideBusy();

    const QString path = filePathFromViewIndex(vIdx);
    const QFileInfo fi(path);

    if (fi.isDir())
    {
        navigateTo(path);
        return;
    }

    // файл — если валиден под режим, то OK
    if (fi.isFile() && selectedKind() != SelectionKind::None) {
        accept();
    }
}

void ExplorerDialog::onSelectionChanged()
{
    updateOkState();                        // Любое изменение выбора -> пересчитать доступность Ok.
}

void ExplorerDialog::updateOkState() 
{
    bool enable = true;
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

    if (fi.isDir()) {
        if (mode == ContentFilterProxy::DicomFiles && dirHasDicom(path))
            return SelectionKind::DicomFolder;
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
    mState = st;

    // Кнопка OK неактивна во время "Opening"
    if (m_buttons && m_buttons->button(QDialogButtonBox::Ok))
        m_buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(st != LoadState::Opening && selectedKind() != SelectionKind::None);

    if (!mStatusBar) return; // на всякий случай

    if (st == LoadState::Opening) {
        if (mStatusText) mStatusText->setText(text.isEmpty() ? tr("Opening…") : text);
        if (mBusy)       mBusy->setVisible(true);
        mStatusBar->show();
        if (mOpenTimeout) mOpenTimeout->start(5000); // есть — используем; нет — игнорируем
    }
    else { // Ready
        if (mOpenTimeout) mOpenTimeout->stop();
        mPendingPath.clear();
        if (mStatusText) mStatusText->setText(text.isEmpty() ? tr("Ready") : text);
        if (mBusy)       mBusy->setVisible(false);
        mStatusBar->show();  // пусть остаётся видимым с текстом Ready
    }
}
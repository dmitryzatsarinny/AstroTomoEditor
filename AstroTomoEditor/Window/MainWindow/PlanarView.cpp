﻿#pragma once
#include "PlanarView.h"
#include <Services/VolumeFix3DR.h>
#include <Services/DicomRange.h>
#include <vtkDICOMApplyRescale.h>
#include <vtk-9.5/vtkGDCMImageReader.h>

namespace {
    class VtkErrorCatcher : public vtkCommand {
    public:
        static VtkErrorCatcher* New() { return new VtkErrorCatcher; }
        void Execute(vtkObject*, unsigned long, void* callData) override {
            hasError = true;
            if (callData) message = static_cast<const char*>(callData);
        }
        bool hasError = false;
        std::string message;
    };
}

PlanarView::PlanarView(QWidget* parent) : QWidget(parent)
{
    initUi();
    connect(mScroll, &QSlider::valueChanged, this, &PlanarView::setSlice);
}

void PlanarView::initUi()
{
    auto* lay = new QHBoxLayout(this);               // вид + слайдер справа
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    mScene = new QGraphicsScene(this);
    mView = new QGraphicsView(mScene, this);
    mView->setDragMode(QGraphicsView::ScrollHandDrag);
    mView->setRenderHint(QPainter::Antialiasing, false);
    mView->setRenderHint(QPainter::SmoothPixmapTransform, true);
    mView->setFrameShape(QFrame::NoFrame);
    mView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    mView->setResizeAnchor(QGraphicsView::AnchorViewCenter);
    mView->setAlignment(Qt::AlignCenter);
    mView->setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    mView->setTransformationAnchor(QGraphicsView::AnchorViewCenter);

    mView->setFocusPolicy(Qt::StrongFocus);
    mView->viewport()->setFocusPolicy(Qt::StrongFocus);
    this->setFocusPolicy(Qt::StrongFocus);

    mView->installEventFilter(this);
    mView->viewport()->installEventFilter(this);

    mImageItem = mScene->addPixmap(QPixmap());
    mImageItem->setZValue(0);

    mScroll = new QSlider(Qt::Vertical, this);
    mScroll->setRange(0, 0);
    mScroll->setPageStep(1);
    mScroll->setSingleStep(1);
    mScroll->setFixedWidth(18);
    mScroll->setInvertedAppearance(false);
    mScroll->setInvertedControls(false);

    lay->addWidget(mView, 1);
    lay->addWidget(mScroll, 0, Qt::AlignRight);

    mDir.fill(0.0f);
    mDir(0, 0) = mDir(1, 1) = mDir(2, 2) = 1.0f; // единичная
    mOrg[0] = mOrg[1] = mOrg[2] = 0.0;
}

vtkSmartPointer<vtkImageData> PlanarView::makeVtkVolume() const
{
    if (mSlices.isEmpty()) return nullptr;

    const int w = X;
    const int h = Y;
    const int d = Z;

    for (const QImage& im : mSlices)
        if (im.width() != w || im.height() != h)
            return nullptr;

    QVector<QImage> gray; gray.reserve(d);
    for (const QImage& s : mSlices)
        gray.push_back(s.format() == QImage::Format_Grayscale8
            ? s : s.convertToFormat(QImage::Format_Grayscale8));

    auto vol = vtkSmartPointer<vtkImageData>::New();
    vol->SetDimensions(w, h, d);
    vol->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    vol->SetSpacing(Dicom.mSpX, Dicom.mSpY, Dicom.mSpZ);

    vtkNew<vtkMatrix3x3> dir;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            dir->SetElement(r, c, mDir(r, c));

    vol->SetDirectionMatrix(dir);   // direction
    vol->SetOrigin(mOrg);           // origin (LPS)

    for (int z = 0; z < d; ++z) {
        const uchar* src = gray[z].constBits();
        const int stride = gray[z].bytesPerLine();
        for (int y = 0; y < h; ++y) {
            auto* dst = static_cast<uchar*>(vol->GetScalarPointer(0, y, z));
            memcpy(dst, src + y * stride, w);
        }
    }
    vol->Modified();
    return vol;
}

bool PlanarView::eventFilter(QObject* obj, QEvent* ev)
{
    if (!mSlices.isEmpty()) {
        if (ev->type() == QEvent::Wheel) {
            auto* w = static_cast<QWheelEvent*>(ev);
            const int delta = (w->angleDelta().y() > 0) ? -1 : +1;
            nudgeSlice(delta);
            w->accept();
            return true;
        }
        if (ev->type() == QEvent::KeyPress) {
            auto* k = static_cast<QKeyEvent*>(ev);
            switch (k->key()) {
            case Qt::Key_PageUp:   pageSlice(-10); return true;
            case Qt::Key_PageDown: pageSlice(+10); return true;
            case Qt::Key_Up:       nudgeSlice(-1); return true;
            case Qt::Key_Down:     nudgeSlice(+1); return true;
            default: break;
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void PlanarView::nudgeSlice(int delta)
{
    const int v = std::clamp(mScroll->value() + delta, mScroll->minimum(), mScroll->maximum());
    if (v != mScroll->value()) mScroll->setValue(v);
}

void PlanarView::pageSlice(int delta)
{
    int step = mScroll->pageStep();
    if (step < 1) step = 10; // дефолт, если не задан
    nudgeSlice((delta < 0 ? -step : +step));
}

static inline void autoWindowFromPercentiles3D(
    vtkImageData* imgD,
    int x0, int x1, int y0, int y1, int z0, int z1,
    double slope, double intercept,
    // рекомендуемые перцентили: CT = [1..99] или [2..98] для «мягче»
    double pLow, double pHigh,
    double& outWL, double& outWW)
{
    const int w = x1 - x0 + 1;
    const int h = y1 - y0 + 1;
    const int d = z1 - z0 + 1;

    // выберем 3 среза по глубине: нижний/середина/верхний (если слоёв меньше — подстроимся)
    const int zA = z0;
    const int zB = z0 + (d > 1 ? d / 2 : 0);
    const int zC = z1;

    // подвыборка по XY, чтобы не тормозить (динамический шаг)
    const int sx = std::max(1, w / 512);
    const int sy = std::max(1, h / 512);

    std::vector<double> vals;
    vals.reserve((w / sx + 1) * (h / sy + 1) * 3);

    vtkNew<vtkImageCast> cast;
    cast->SetInputData(imgD);
    cast->SetOutputScalarTypeToDouble();
    cast->Update();
    vtkImageData* dimg = cast->GetOutput();

    // теперь increments в единицах double
    vtkIdType inc[3];
    dimg->GetIncrements(inc);

    auto sampleSlice = [&](int z)
        {
            double* base = static_cast<double*>(dimg->GetScalarPointer(x0, y0, z));
            for (int yy = 0; yy < h; yy += sy) {
                double* row = base + vtkIdType(yy) * inc[1];
                for (int xx = 0; xx < w; xx += sx) {
                    double vin = row[vtkIdType(xx) * inc[0]];
                    vals.push_back(slope * vin + intercept);
                }
            }
        };

    sampleSlice(zA);
    if (zB != zA && zB != zC) sampleSlice(zB);
    if (zC != zA)             sampleSlice(zC);

    if (vals.empty()) { outWL = 40; outWW = 400; return; }

    std::sort(vals.begin(), vals.end());
    auto pct = [&](double p) {
        const double idx = (p / 100.0) * (vals.size() - 1);
        const size_t i0 = size_t(std::floor(idx));
        const size_t i1 = std::min(vals.size() - 1, i0 + 1);
        const double t = idx - i0;
        return vals[i0] * (1.0 - t) + vals[i1] * t;
        };

    double lo = pct(pLow);
    double hi = pct(pHigh);

    // подстраховки от «странных» окон
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) {
        outWL = 40; outWW = 400; return;
    }

    outWL = 0.5 * (lo + hi);
    outWW = std::max(1.0, hi - lo);

    // Не давать безумно огромные окна (пустая, «чёрная» картинка)
    outWW = std::clamp(outWW, 50.0, 6000.0);
}

static bool canReconstructVolume(const QVector<QString>& files) 
{
    if (files.size() < 3) 
        return false;

    // Быстрый проход по метаданным без пикселей
    struct SliceInfo { QVector3D R, C, N; double s = 0; bool ok = false; };
    QVector<SliceInfo> a; a.reserve(files.size());

    QVector3D R0, C0, N0;
    bool haveRef = false;

    for (const QString& f : files) {
        auto p = vtkSmartPointer<vtkDICOMParser>::New();
        auto d = vtkSmartPointer<vtkDICOMMetaData>::New();
        p->SetMetaData(d); p->SetFileName(f.toUtf8().constData()); p->Update();

        // базовая валидность
        const bool hasPix = d->Has(DC::Rows) && d->Has(DC::Columns) &&
            (d->Has(DC::PixelData) || d->Has(DC::FloatPixelData) || d->Has(DC::DoubleFloatPixelData));
        if (!hasPix) continue;

        // монохромность
        const int spp = d->Has(DC::SamplesPerPixel) ? d->Get(DC::SamplesPerPixel).AsInt() : 1;
        const QString phot = QString::fromStdString(d->Get(DC::PhotometricInterpretation).AsString()).toUpper();
        if (!(spp == 1 && (phot.isEmpty() || phot.startsWith("MONOCHROME")))) 
            return false;

        SliceInfo si;
        if (d->Has(DC::ImageOrientationPatient)) {
            const auto v = d->Get(DC::ImageOrientationPatient);
            QVector3D R(v.GetDouble(0), v.GetDouble(1), v.GetDouble(2));
            QVector3D C(v.GetDouble(3), v.GetDouble(4), v.GetDouble(5));
            QVector3D N = QVector3D::crossProduct(R, C).normalized();
            si.R = R.normalized(); si.C = C.normalized(); si.N = N;
        }
        else 
            return false;

        if (d->Has(DC::ImagePositionPatient)) {
            const auto ip = d->Get(DC::ImagePositionPatient);
            QVector3D P(ip.GetDouble(0), ip.GetDouble(1), ip.GetDouble(2));
            si.s = QVector3D::dotProduct(P, si.N); // скалярная координата вдоль нормали
        }
        else
            return false;

        if (!haveRef) { R0 = si.R; C0 = si.C; N0 = si.N; haveRef = true; }
        a.push_back(si);
    }

    if (a.size() < 3) return false;

    // Монотонный и почти равномерный шаг по нормали
    std::sort(a.begin(), a.end(), [](auto& A, auto& B) { return A.s < B.s; });
    QVector<double> steps; steps.reserve(a.size() - 1);
    for (int i = 1; i < a.size(); ++i) steps.push_back(a[i].s - a[i - 1].s);

    // выкинуть «нулевые»/почти нулевые шаги (дубликаты)
    const double eps = 1e-6;
    steps.erase(std::remove_if(steps.begin(), steps.end(), [&](double v) { return std::abs(v) < eps; }), steps.end());
    if (steps.size() < 2) return false;

    // оценим равномерность
    const double med = [&] {
        QVector<double> t = steps; std::sort(t.begin(), t.end());
        int m = t.size() / 2; return (t.size() % 2) ? t[m] : 0.5 * (t[m - 1] + t[m]);
        }();
    if (med <= 0)
        return false;

    return true;
}

void PlanarView::loadSeriesFiles(const QVector<QString>& files)
{
    // --- сброс UI/состояния ---
    mSlices.clear();
    mIndex = 0;
    mScene->clear();
    avalibletoreconstruction = false;
    mImageItem = mScene->addPixmap(QPixmap());

    if (files.isEmpty()) {
        mScroll->setRange(0, 0);
        emit loadStarted(0);
        emit loadFinished(0);
        return;
    }

    vtkSmartPointer<vtkImageData> volume;
    bool isCompressed = false;
    vtkAlgorithmOutput* srcPort = nullptr;

    // --- ветка .3dr ---
    const bool is3dr = (files.size() == 1 && files.front().endsWith(".3dr", Qt::CaseInsensitive));
    if (is3dr) {
        QString err; bool isMRI = false;
        volume = Load3DR_Normalized(files.front(), isMRI);

        if (!volume) {
            emit showWarning(tr("3DR load failed."));
            mScroll->setRange(0, 0);
            emit loadFinished(0);
            return;
        }
        // DICOM-поля для .3dr
        if (isMRI) {
            Dicom.TypeOfRecord = MRI3DR;
            Dicom.physicalMin = 0;    Dicom.physicalMax = 255;
            Dicom.RealMin = 0;    Dicom.RealMax = 255;
        }
        else {
            Dicom.TypeOfRecord = CT3DR;
            Dicom.physicalMin = 0;    Dicom.physicalMax = 2000;
            Dicom.RealMin = -1000; Dicom.RealMax = 1000;
        }
        Dicom.slope = 1.0; Dicom.intercept = 0.0;

        // Ориентация по умолчанию
        mDir = QMatrix3x3(); mDir(0, 0) = mDir(1, 1) = mDir(2, 2) = 1.0f;
        mOrg[0] = mOrg[1] = mOrg[2] = 0.0;
    }
    else
    {
        // --- ветка DICOM ---
        auto report = filterSeriesByConsistency(files);                   // быстрый фильтр геометрии
        if (!report.bad.isEmpty()) {
            emit filesFiltered(report.good.size(), report.bad.size());
            emit showInfo(report.reason);
        }
        const QVector<QString> toRead = report.good.isEmpty() ? files : report.good;

        // Сформируем список имён
        auto names = vtkSmartPointer<vtkStringArray>::New();
        names->SetNumberOfValues(toRead.size());
        for (vtkIdType i = 0; i < static_cast<vtkIdType>(toRead.size()); ++i)
            names->SetValue(i, toRead[int(i)].toUtf8().constData());

        // 1) ЧИТАЕМ МЕТАДАННЫЕ/СОРТИРОВКУ через vtkDICOMReader (без декодирования)
        vtkSmartPointer<vtkDICOMReader> mdReader = vtkSmartPointer<vtkDICOMReader>::New();
        auto errObs1 = vtkSmartPointer<VtkErrorCatcher>::New();
        mdReader->AddObserver(vtkCommand::ErrorEvent, errObs1);
        mdReader->SetFileNames(names);
        mdReader->UpdateInformation();                                     // *** важно: только инфо

        if (errObs1->hasError || !mdReader->GetMetaData()) {
            emit showWarning(tr("DICOM metadata read failed."));
            mScroll->setRange(0, 0);
            emit loadFinished(0);
            return;
        }

        // Определяем сжатый TS по метаданным
        auto md = mdReader->GetMetaData();
        auto isCompressedTS = [&](vtkDICOMMetaData* m)->bool {
            if (!m || !m->Has(DC::TransferSyntaxUID)) return false;
            const std::string ts = m->Get(DC::TransferSyntaxUID).AsString();
            return ts.rfind("1.2.840.10008.1.2.4.", 0) == 0   // JPEG*, JPEG-LS, JPEG2000
                || ts == "1.2.840.10008.1.2.5";              // RLE
            };
        isCompressed = isCompressedTS(md);

        // 2a) Если НЕ сжато — обычный vtkDICOMReader
        if (!isCompressed) {
            auto pixReader = vtkSmartPointer<vtkDICOMReader>::New();
            auto errObs2 = vtkSmartPointer<VtkErrorCatcher>::New();
            pixReader->AddObserver(vtkCommand::ErrorEvent, errObs2);
            pixReader->SetFileNames(names);
            pixReader->UpdateInformation();
            pixReader->Update();
            if (errObs2->hasError || !pixReader->GetOutput()) {
                emit showWarning(tr("DICOM read failed."));
                mScroll->setRange(0, 0);
                emit loadFinished(0);
                return;
            }
            volume = pixReader->GetOutput();
            srcPort = pixReader->GetOutputPort();

            // Геометрия LPS
            double iop[6]{ 1,0,0, 0,1,0 };
            if (md->Has(DC::ImageOrientationPatient)) {
                const auto v = md->Get(DC::ImageOrientationPatient);
                for (int i = 0; i < 6 && i < v.GetNumberOfValues(); ++i) iop[i] = v.GetDouble(i);
            }
            const QVector3D RR(iop[0], iop[1], iop[2]);
            const QVector3D CC(iop[3], iop[4], iop[5]);
            const QVector3D NN = QVector3D::crossProduct(RR, CC);
            mDir = QMatrix3x3();
            mDir(0, 0) = RR.x(); mDir(1, 0) = RR.y(); mDir(2, 0) = RR.z();
            mDir(0, 1) = CC.x(); mDir(1, 1) = CC.y(); mDir(2, 1) = CC.z();
            mDir(0, 2) = NN.x(); mDir(1, 2) = NN.y(); mDir(2, 2) = NN.z();
            if (md->Has(DC::ImagePositionPatient)) {
                const auto ipp = md->Get(DC::ImagePositionPatient);
                mOrg[0] = ipp.GetDouble(0); mOrg[1] = ipp.GetDouble(1); mOrg[2] = ipp.GetDouble(2);
            }

            // DICOM-диапазоны/VOI
            Dicom = GetDicomRangesVTK(pixReader);
        }
        // 2b) Если СЖАТО — декодируем через vtkGDCMImageReader
        else {
            vtkSmartPointer<vtkGDCMImageReader> gdcm = vtkSmartPointer<vtkGDCMImageReader>::New();
            auto errObs2 = vtkSmartPointer<VtkErrorCatcher>::New();
            gdcm->AddObserver(vtkCommand::ErrorEvent, errObs2);
            gdcm->SetFileNames(names);                         // *** ВАЖНО: вся серия, не один файл
            gdcm->UpdateInformation();
            gdcm->Update();
            if (errObs2->hasError || !gdcm->GetOutput()) {
                emit showWarning(tr("GDCM decode failed."));
                mScroll->setRange(0, 0);
                emit loadFinished(0);
                return;
            }
            volume = gdcm->GetOutput();
            srcPort = gdcm->GetOutputPort();

            // Геометрия LPS — по метаданным mdReader (единое место истины)
            double iop[6]{ 1,0,0, 0,1,0 };
            if (md->Has(DC::ImageOrientationPatient)) {
                const auto v = md->Get(DC::ImageOrientationPatient);
                for (int i = 0; i < 6 && i < v.GetNumberOfValues(); ++i) iop[i] = v.GetDouble(i);
            }
            const QVector3D RR(iop[0], iop[1], iop[2]);
            const QVector3D CC(iop[3], iop[4], iop[5]);
            const QVector3D NN = QVector3D::crossProduct(RR, CC);
            mDir = QMatrix3x3();
            mDir(0, 0) = RR.x(); mDir(1, 0) = RR.y(); mDir(2, 0) = RR.z();
            mDir(0, 1) = CC.x(); mDir(1, 1) = CC.y(); mDir(2, 1) = CC.z();
            mDir(0, 2) = NN.x(); mDir(1, 2) = NN.y(); mDir(2, 2) = NN.z();
            if (md->Has(DC::ImagePositionPatient)) {
                const auto ipp = md->Get(DC::ImagePositionPatient);
                mOrg[0] = ipp.GetDouble(0); mOrg[1] = ipp.GetDouble(1); mOrg[2] = ipp.GetDouble(2);
            }

            // ---- Итоговый spacing: X/Y из volume, Z — проверяем и при необходимости считаем по IPP ----
            auto finalizeSpacing = [&](double sp[3]) {
                // стартуем с того, что отдал ридер
                volume->GetSpacing(sp);

                const bool zLooksWrong = !(sp[2] > 0.0) || sp[2] == 1.0; // частая «затычка» по умолчанию
                if (zLooksWrong) {
                    // Собираем скаляр s = dot(IPP, N) по всем файлам и берём медианный шаг
                    QVector<double> s; s.reserve(names->GetNumberOfValues());
                    for (vtkIdType i = 0; i < names->GetNumberOfValues(); ++i) {
                        const std::string fname = names->GetValue(i);
                        vtkNew<vtkDICOMParser>    parser;
                        vtkNew<vtkDICOMMetaData>  meta;
                        parser->SetMetaData(meta);
                        parser->SetFileName(fname.c_str());
                        parser->Update(); // быстро: читает только теги
                        if (meta->Has(DC::ImagePositionPatient)) {
                            const auto ipp = meta->Get(DC::ImagePositionPatient);
                            const QVector3D P(ipp.GetDouble(0), ipp.GetDouble(1), ipp.GetDouble(2));
                            s.push_back(QVector3D::dotProduct(P, NN));
                        }
                    }
                    std::sort(s.begin(), s.end());
                    QVector<double> step; step.reserve(std::max(0, (int)s.size() - 1));
                    for (int i = 1; i < s.size(); ++i) {
                        const double d = std::abs(s[i] - s[i - 1]);
                        if (d > 1e-9) step.push_back(d);
                    }
                    if (!step.isEmpty()) {
                        auto mid = step.begin() + step.size() / 2;
                        std::nth_element(step.begin(), mid, step.end());
                        sp[2] = std::max(1e-6, *mid);
                    }
                }

                // сохраняем в поля класса (volume менять не обязательно)
                mSpX = (sp[0] > 0) ? sp[0] : 1.0;
                mSpY = (sp[1] > 0) ? sp[1] : 1.0;
                mSpZ = (sp[2] > 0) ? sp[2] : 1.0;
            };

            // DICOM-диапазоны/VOI — считаем по mdReader, он одинаков для обоих путей
            Dicom = GetDicomRangesVTK(mdReader);
            if (!Dicom.SpCreated)
            {
                Dicom.SpCreated = true;
                double spTmp[3]{ 1,1,1 };
                finalizeSpacing(spTmp);
                Dicom.mSpX = mSpX;
                Dicom.mSpY = mSpY;
                Dicom.mSpZ = mSpZ;
            }
        }

        // Доп. интерпретация modality / VOI
        vtkDICOMMetaData* md2 = mdReader->GetMetaData();
        bool invertMono1 = false;
        QString modalityStr;
        if (md2) {
            if (md2->Has(DC::PhotometricInterpretation)) {
                const QString phot = QString::fromStdString(md2->Get(DC::PhotometricInterpretation).AsString()).toUpper();
                invertMono1 = (phot == "MONOCHROME1");
            }
            if (md2->Has(DC::Modality))
                modalityStr = QString::fromStdString(md2->Get(DC::Modality).AsString()).toUpper();
        }

        // Заодно сохраним «инверсию» сразу в Dicom.TypeOfRecord и передадим в buildCache (ниже).
        if (modalityStr == "CT") {
            Dicom.TypeOfRecord = CT;
            if (Dicom.intercept < -1000) 
            { 
                Dicom.physicalMin = 0; 
                Dicom.physicalMax = 2048; 
            }
            else 
            { 
                Dicom.physicalMin = -1024; 
                Dicom.physicalMax = 1024; 
            }
            Dicom.RealMin = -1000; 
            Dicom.RealMax = 1000;
        }
        else if (modalityStr == "MR" || modalityStr == "MRI") 
        {
            Dicom.TypeOfRecord = MRI;
            Dicom.physicalMin = 0; Dicom.physicalMax = 255;
            Dicom.RealMin = 0; Dicom.RealMax = 255;
        }

    }

    // --- общие параметры (spacing/dims) ---

    if (!Dicom.SpCreated)
    {
        Dicom.SpCreated = true;
        double sp[3]{ 1,1,1 }; volume->GetSpacing(sp);
        mSpX = (sp[0] > 0) ? sp[0] : 1.0;  mSpY = (sp[1] > 0) ? sp[1] : 1.0;  mSpZ = (sp[2] > 0) ? sp[2] : 1.0;
        Dicom.mSpX = mSpX;
        Dicom.mSpY = mSpY;
        Dicom.mSpZ = mSpZ;
    }

    { 
        int* dims = volume->GetDimensions(); 
        X = dims[0]; Y = dims[1]; Z = dims[2]; 
    }

    emit loadStarted(Z);

    const bool invertMono1 = false;
    buildCache(volume, srcPort, invertMono1, Dicom);

    avalibletoreconstruction = (Z > 1) && !mSlices.isEmpty();

    // --- UI финализация ---
    if (mSlices.isEmpty()) {
        mScroll->setRange(0, 0);
        emit loadFinished(0);
        return;
    }

    emit loadFinished(Z);
    mScroll->setRange(0, std::max(0, int(mSlices.size() - 1)));
    mScroll->setPageStep(std::max(1, int(mSlices.size() / 20)));
    mScroll->setValue(std::max(0, int(mSlices.size() - 1)));
    setSlice(mScroll->value());
}

// ===== cache / display ======================================================

static inline uchar wl8(double v, double min, double max, Mode TypeOfRecord, bool invMono1)
{
    int window = max - min;
    if (TypeOfRecord == CT)
    {
        if (v > max + window)
            v = max;
        else if (v > max - window / 20)
            v = max - window / 20;
    }
    double x = (v - min) * 255.0 / window;
    int y = static_cast<int>(std::floor(x));
    if (y < 1)   
        y = 0;
    if (y > 255)
        y = 255;
    if (invMono1) y = 255 - y;
    return static_cast<uchar>(y);
}

void PlanarView::buildCache(vtkImageData* volume, vtkAlgorithmOutput* srcPort, bool invertMono1, DicomInfo Dicom)
{
    if (!volume) {
        mSlices.clear();
        emit loadProgress(0, 0);
        return;
    }

    int ext[6]; volume->GetExtent(ext);
    const int x0 = ext[0], x1 = ext[1];
    const int y0 = ext[2], y1 = ext[3];
    const int z0 = ext[4], z1 = ext[5];

    const int w = x1 - x0 + 1;
    const int h = y1 - y0 + 1;
    const int total = z1 - z0 + 1;

    if (w <= 0 || h <= 0 || total <= 0) {
        mSlices.clear();
        emit loadProgress(0, 0);
        return;
    }

    // --- Универсальный вход: ВСЕГДА от готового vtkImageData ---
    vtkNew<vtkImageCast> cast;
    cast->SetInputData(volume);             // <-- вместо SetInputConnection(srcPort)
    cast->SetOutputScalarTypeToDouble();
    cast->Update();                         // <-- ОБЯЗАТЕЛЬНО

    vtkImageData* imgD = cast->GetOutput();
    if (!imgD) {
        mSlices.clear();
        emit loadProgress(0, 0);
        return;
    }

    mSlices.clear();
    mSlices.reserve(total);

    // Идём сверху вниз (как у тебя было: z = z1..z0), с периодической прокачкой прогресса
    for (int z = z1, idx = 0; z >= z0; --z, ++idx)
    {
        // Измеряем реальные байтовые шаги по X/Y, чтобы корректно читать даже при инвертированных stride
        auto* p00 = static_cast<char*>(imgD->GetScalarPointer(x0, y0, z));
        auto* p10 = static_cast<char*>(imgD->GetScalarPointer(std::min(x0 + 1, x1), y0, z));
        auto* p01 = static_cast<char*>(imgD->GetScalarPointer(x0, std::min(y0 + 1, y1), z));

        // Если что-то пошло не так — перестраховка
        if (!p00 || !p10 || !p01) {
            // попытаемся мягко пропустить этот срез
            QImage empty(w, h, QImage::Format_Grayscale8);
            empty.fill(0);
            mSlices.push_back(std::move(empty));
            continue;
        }

        const ptrdiff_t incXb_raw = (w > 1) ? (p10 - p00) : ptrdiff_t(sizeof(double));
        const ptrdiff_t incYb_raw = (h > 1) ? (p01 - p00) : ptrdiff_t(sizeof(double));

        const bool xPositive = (incXb_raw >= 0);
        const bool yPositive = (incYb_raw >= 0);

        const int  xStart = xPositive ? x0 : x1;
        const int  yStart = yPositive ? y0 : y1;
        const ptrdiff_t stepXb = (xPositive ? +1 : -1) * std::abs(incXb_raw);

        QImage q(w, h, QImage::Format_Grayscale8);

        for (int yy = 0; yy < h; ++yy)
        {
            const int ySrc = yPositive ? (yStart + yy) : (yStart - yy);
            auto* row0 = static_cast<char*>(imgD->GetScalarPointer(xStart, ySrc, z));
            uchar* dst = q.scanLine(yy);

            // safety
            if (!row0 || !dst) {
                std::memset(dst, 0, size_t(w));
                continue;
            }

            for (int xx = 0; xx < w; ++xx)
            {
                auto* p = reinterpret_cast<const double*>(row0 + stepXb * xx);
                const double vHU = (*p * Dicom.slope) - Dicom.intercept;
                dst[xx] = wl8(vHU, Dicom.physicalMin, Dicom.physicalMax, Dicom.TypeOfRecord, invertMono1);
            }
        }

        q = q.mirrored();
        mSlices.push_back(std::move(q));

        if (((idx + 1) % 8) == 0 || idx + 1 == total) {
            emit loadProgress(idx + 1, total);
            qApp->processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
}


void PlanarView::setSlice(int i)
{
    if (mSlices.isEmpty()) return;
    i = std::clamp(i, 0, int(mSlices.size() - 1));
    if (i == mIndex && mImageItem && !mImageItem->pixmap().isNull()) return;

    mIndex = i;

    // гарантированный валидный формат
    const QImage img = mSlices[i].convertToFormat(QImage::Format_ARGB32);
    mImageItem->setPixmap(QPixmap::fromImage(img));

    // обновить границы сцены и вписать в вид
    mScene->setSceneRect(mImageItem->boundingRect());
    if (mAutoFit) mView->fitInView(mImageItem, Qt::KeepAspectRatio);

    mView->viewport()->update();
}

void PlanarView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (mAutoFit) fitToWindow();
}

void PlanarView::wheelEvent(QWheelEvent* e)
{
    if (!mSlices.isEmpty()) {
        const int delta = (e->angleDelta().y() > 0) ? -1 : +1;
        mScroll->setValue(std::clamp(mScroll->value() + delta, mScroll->minimum(), mScroll->maximum()));
        e->accept();
        return;
    }
    QWidget::wheelEvent(e);
}

void PlanarView::keyPressEvent(QKeyEvent* e)
{
    if (mSlices.isEmpty()) { QWidget::keyPressEvent(e); return; }
    int v = mScroll->value();
    if (e->key() == Qt::Key_PageUp)      v -= 10;
    else if (e->key() == Qt::Key_PageDown) v += 10;
    else if (e->key() == Qt::Key_Up)     v -= 1;
    else if (e->key() == Qt::Key_Down)   v += 1;
    else { QWidget::keyPressEvent(e); return; }
    mScroll->setValue(std::clamp(v, mScroll->minimum(), mScroll->maximum()));
}

void PlanarView::setWindowLevel(double level, double width)
{
    mWL = level; mWW = width;
    if (!mSlices.isEmpty()) {
        // TODO: можно сделать быструю перекраску через LUT без полного пересчёта
    }
    setSlice(mIndex);
}

void PlanarView::fitToWindow()
{
    mView->fitInView(mScene->itemsBoundingRect(), Qt::KeepAspectRatio);
}

void PlanarView::resetZoom()
{
    mView->resetTransform();
}

void PlanarView::rebuildPixmap()
{
    setSlice(mIndex);
}

// ===== validation/filtering ==================================================

bool PlanarView::readPixelKeyQuick(const QString& file, DicomPixelKey& out, QString* errMsg)
{
    vtkNew<vtkDICOMReader> r;
    auto err = vtkSmartPointer<VtkErrorCatcher>::New();
    r->AddObserver(vtkCommand::ErrorEvent, err);
    r->SetFileName(file.toUtf8().constData());

    r->UpdateInformation();

    if (err->hasError) {
        if (errMsg) *errMsg = QString::fromStdString(err->message);
        return false;
    }

    vtkDICOMMetaData* meta = r->GetMetaData();
    if (!meta) {
        if (errMsg) *errMsg = tr("No metadata.");
        return false;
    }

    using Tag = vtkDICOMTag;
    auto val = [&](const Tag& t) { return meta->GetAttributeValue(t); };

    out.rows = val(Tag(0x0028, 0x0010)).AsInt(); // Rows
    out.cols = val(Tag(0x0028, 0x0011)).AsInt(); // Columns
    out.bitsAllocated = val(Tag(0x0028, 0x0100)).AsInt(); // BitsAllocated
    out.samplesPerPixel = val(Tag(0x0028, 0x0002)).AsInt(); // SamplesPerPixel
    out.pixelRepresentation = val(Tag(0x0028, 0x0103)).AsInt(); // 0/1
    out.photometric = QString::fromStdString(val(Tag(0x0028, 0x0004)).AsString());

    if (out.rows <= 0 || out.cols <= 0) {
        if (errMsg) *errMsg = tr("Invalid pixel geometry.");
        return false;
    }
    return true;
}

PlanarView::ValidationReport
PlanarView::filterSeriesByConsistency(const QVector<QString>& files)
{
    // Бакеты по ключу пикселей: берём самый «толстый» как majority.
    QHash<QString, QVector<QString>> buckets;
    QVector<QString> failed;

    auto keyToString = [](const DicomPixelKey& k) {
        return QString("%1x%2|b%3|s%4|pr%5|pi%6")
            .arg(k.rows).arg(k.cols)
            .arg(k.bitsAllocated)
            .arg(k.samplesPerPixel)
            .arg(k.pixelRepresentation)
            .arg(k.photometric);
        };

    for (const auto& f : files) {
        DicomPixelKey k;
        QString err;
        if (!readPixelKeyQuick(f, k, &err)) {
            failed.push_back(f);
            continue;
        }
        buckets[keyToString(k)].push_back(f);
    }

    QString bestKey;
    int bestSize = -1;
    for (auto it = buckets.begin(); it != buckets.end(); ++it) {
        if (it.value().size() > bestSize) {
            bestSize = it.value().size();
            bestKey = it.key();
        }
    }

    ValidationReport rep;
    if (bestSize <= 0) {
        rep.bad = files;
        rep.reason = QObject::tr("No consistent pixel geometry in series.");
        return rep;
    }

    rep.good = buckets[bestKey];
    for (auto it = buckets.begin(); it != buckets.end(); ++it) {
        if (it.key() == bestKey) continue;
        rep.bad += it.value();
    }
    rep.bad += failed;

    if (!rep.bad.isEmpty())
        rep.reason = QObject::tr("Filtered %1 inconsistent file(s) by pixel geometry.")
        .arg(rep.bad.size());
    return rep;
}
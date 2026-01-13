#include "TransferFunction.h"
#include <vtkSmartPointer.h>
#include <vtkVolumeProperty.h>
#include <vtkColorTransferFunction.h>
#include <vtkPiecewiseFunction.h>

namespace {

    inline void fixRange(double& a, double& b)
    {
        // если min/max не задано или совпало — подставляем диапазон гистограммы
        if (a == b) {
            a = static_cast<double>(HistMin);
            b = static_cast<double>(HistMax);
        }
        if (a > b)
            std::swap(a, b);
    }

    inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

    template<typename F>
    vtkSmartPointer<vtkColorTransferFunction>
        buildCTF(double min, double max, F&& add)
    {
        auto c = vtkSmartPointer<vtkColorTransferFunction>::New();
        c->RemoveAllPoints();
        add(c.GetPointer(), min, max);
        return c;
    }

    template<typename F>
    vtkSmartPointer<vtkPiecewiseFunction>
        buildOTF(double min, double max, F&& add)
    {
        auto o = vtkSmartPointer<vtkPiecewiseFunction>::New();
        o->RemoveAllPoints();
        add(o.GetPointer(), min, max);
        return o;
    }

} // namespace

QMenu* TF::CreateMenu(QWidget* parent, std::function<void(TFPreset)> onChosen) {
    auto* m = new QMenu(parent);
    QObject::connect(m->addAction(QObject::tr("Grayscale")), &QAction::triggered, [onChosen] { onChosen(TFPreset::Grayscale); });
    QObject::connect(m->addAction(QObject::tr("Rainbow")), &QAction::triggered, [onChosen] { onChosen(TFPreset::Rainbow);   });
    QObject::connect(m->addAction(QObject::tr("Bone")), &QAction::triggered, [onChosen] { onChosen(TFPreset::Bone);      });
    QObject::connect(m->addAction(QObject::tr("Angio")), &QAction::triggered, [onChosen] { onChosen(TFPreset::Angio);      });
    QObject::connect(m->addAction(QObject::tr("SoftTissue")), &QAction::triggered, [onChosen] { onChosen(TFPreset::SoftTissue);      });
    QObject::connect(m->addAction(QObject::tr("Lungs")), &QAction::triggered, [onChosen] { onChosen(TFPreset::Lungs);      });

    QObject::connect(m->addAction(QObject::tr("Skin")), &QAction::triggered, [onChosen] { onChosen(TFPreset::Skin);      });
    QObject::connect(m->addAction(QObject::tr("Hot Metal")), &QAction::triggered, [onChosen] { onChosen(TFPreset::HotMetal);  });
    m->addSeparator();
    QObject::connect(m->addAction(QObject::tr("Invert current")), &QAction::triggered, [onChosen] { onChosen(TFPreset::InvertCurrent); });
    return m;
}

void TF::ApplyPreset(vtkVolumeProperty* prop, TFPreset preset, double min, double max)
{
    if (!prop) return;
    fixRange(min, max);

    if (preset == TFPreset::InvertCurrent) { InvertInPlace(prop, min, max); return; }

    vtkSmartPointer<vtkColorTransferFunction> ctf;
    vtkSmartPointer<vtkPiecewiseFunction>     otf;

    switch (preset) {
    case TFPreset::Grayscale: ctf = MakeCTF_Grayscale(min, max); otf = MakeOTF_Grayscale(min, max); break;
    case TFPreset::Rainbow:   ctf = MakeCTF_Rainbow(min, max);   otf = MakeOTF_Rainbow(min, max);   break;
    case TFPreset::Bone:      ctf = MakeCTF_Bone(min, max);      otf = MakeOTF_Bone(min, max);      break;
    case TFPreset::Angio:     ctf = MakeCTF_Angio(min, max);     otf = MakeOTF_Angio(min, max);     break;
    case TFPreset::SoftTissue:ctf = MakeCTF_SoftTissue(min, max);otf = MakeOTF_SoftTissue(min, max);break;
    case TFPreset::Lungs:     ctf = MakeCTF_Lungs(min, max);     otf = MakeOTF_Lungs(min, max);     break;
    case TFPreset::Skin:      ctf = MakeCTF_Skin(min, max);      otf = MakeOTF_Skin(min, max);      break;
    case TFPreset::HotMetal:  ctf = MakeCTF_Hot(min, max);       otf = MakeOTF_Hot(min, max);       break;
    default: return;
    }

    // Обязательно компонент 0
    prop->SetIndependentComponents(true);
    prop->SetColor(0, ctf);
    prop->SetScalarOpacity(0, otf);
    prop->SetInterpolationTypeToLinear();
    prop->Modified();
}

// -------------------- PRESETS --------------------
vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_Grayscale(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        c->AddRGBPoint(a, 0, 0, 0);
        c->AddRGBPoint(b, 1, 1, 1);
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_Grayscale(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        o->AddPoint(a, 0.00);
        o->AddPoint(Lerp(a, b, 0.25), 0.10);
        o->AddPoint(Lerp(a, b, 0.70), 0.40);
        o->AddPoint(b, 0.80);
        });
}

vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_Rainbow(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        c->AddRGBPoint(a, 0.0, 0.0, 0.0);
        c->AddRGBPoint(Lerp(a, b, 0.13), 0.0, 0.0, 0.3);
        c->AddRGBPoint(Lerp(a, b, 0.38), 0.0, 0.0, 1.0);
        c->AddRGBPoint(Lerp(a, b, 0.50), 1.0, 0.0, 0.0);
        c->AddRGBPoint(Lerp(a, b, 0.75), 0.0, 1.0, 0.0);
        c->AddRGBPoint(Lerp(a, b, 0.94), 1.0, 1.0, 1.0);
        c->AddRGBPoint(b, 1.0, 1.0, 1.0);
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_Rainbow(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        o->AddPoint(a, 0.00);
        o->AddPoint(Lerp(a, b, 0.24), 0.08);
        o->AddPoint(Lerp(a, b, 0.55), 0.35);
        o->AddPoint(b, 0.85);
        });
}

// ===== BONE =====
vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_Bone(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        c->AddRGBPoint(H(-1000), 0.00, 0.00, 0.00);
        c->AddRGBPoint(H(0), 0.20, 0.20, 0.20);
        c->AddRGBPoint(H(150), 0.70, 0.70, 0.68);
        c->AddRGBPoint(H(500), 0.95, 0.95, 0.94);
        c->AddRGBPoint(H(1200), 1.00, 1.00, 1.00);
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_Bone(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        o->AddPoint(H(-1000), 0.00);
        o->AddPoint(H(0), 0.00);
        o->AddPoint(H(150), 0.05);
        o->AddPoint(H(500), 0.45);
        o->AddPoint(H(1200), 0.65);
        });
}

// ===== ANGIO / VESSELS =====
vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_Angio(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        c->AddRGBPoint(H(-1000), 0.00, 0.00, 0.00);
        c->AddRGBPoint(H(-200), 0.16, 0.18, 0.20);   // фон тканей — холодноватый серый
        c->AddRGBPoint(H(40), 0.84, 0.80, 0.74);   // кровь/вода — тёплый беж
        c->AddRGBPoint(H(120), 0.90, 0.86, 0.80);   // сосуды — светлее/теплее
        c->AddRGBPoint(H(300), 0.97, 0.95, 0.92);
        c->AddRGBPoint(H(1000), 1.00, 1.00, 1.00);
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_Angio(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        o->AddPoint(H(-1000), 0.00);
        o->AddPoint(H(-50), 0.00);
        o->AddPoint(H(40), 0.10);
        o->AddPoint(H(120), 0.22);
        o->AddPoint(H(300), 0.60);   // сосуды/контраст хорошо видны
        o->AddPoint(H(1000), 0.70);
        });
}

// ===== SoftTissue =====
vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_SoftTissue(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        c->AddRGBPoint(H(-1000), 0.00, 0.00, 0.00);   // воздух — чёрный
        c->AddRGBPoint(H(-700), 0.12, 0.16, 0.24);   // лёгкие — холодно-синевато-серый
        c->AddRGBPoint(H(-100), 0.30, 0.34, 0.32);   // жир — нейтрально-серо-оливковый
        c->AddRGBPoint(H(40), 0.82, 0.78, 0.72);   // вода/кровь — тёплый бежевый
        c->AddRGBPoint(H(120), 0.88, 0.85, 0.80);   // мягкие ткани — светлый тёплый
        c->AddRGBPoint(H(300), 0.95, 0.93, 0.88);   // плотнее — почти слоновая кость
        c->AddRGBPoint(H(1200), 1.00, 1.00, 1.00);   // эмаль — белый
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_SoftTissue(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        o->AddPoint(H(-1000), 0.00);
        o->AddPoint(H(-700), 0.00);
        o->AddPoint(H(-100), 0.02);
        o->AddPoint(H(40), 0.06);
        o->AddPoint(H(120), 0.15);
        o->AddPoint(H(300), 0.35);
        o->AddPoint(H(1200), 0.60);
        });
}

// ===== LUNGS =====
vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_Lungs(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        c->AddRGBPoint(H(-1000), 0.00, 0.00, 0.00);
        c->AddRGBPoint(H(-850), 0.10, 0.14, 0.22);   // воздушные поля — холодные
        c->AddRGBPoint(H(-600), 0.22, 0.28, 0.34);
        c->AddRGBPoint(H(-50), 0.30, 0.33, 0.34);   // мягкие около нуля — нейтральные
        c->AddRGBPoint(H(40), 0.86, 0.83, 0.78);   // сосуды — тёплый светлый
        c->AddRGBPoint(H(300), 0.95, 0.95, 0.93);   // кость — почти белая
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_Lungs(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        auto H = [&](double hu) { return std::clamp(hu, a, b); };
        o->AddPoint(H(-1000), 0.00);
        o->AddPoint(H(-800), 0.00);
        o->AddPoint(H(-500), 0.02);
        o->AddPoint(H(40), 0.18);   // проявляем сосуды
        o->AddPoint(H(300), 0.35);
        });
}

vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_Skin(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        c->AddRGBPoint(a, 0.0, 0.0, 0.0);
        c->AddRGBPoint(Lerp(a, b, 0.27), 0.50, 0.35, 0.30);
        c->AddRGBPoint(Lerp(a, b, 0.47), 0.80, 0.55, 0.45);
        c->AddRGBPoint(Lerp(a, b, 0.70), 0.95, 0.75, 0.65);
        c->AddRGBPoint(b, 1.00, 0.90, 0.85);
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_Skin(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        o->AddPoint(a, 0.00);
        o->AddPoint(Lerp(a, b, 0.23), 0.08);
        o->AddPoint(Lerp(a, b, 0.47), 0.28);
        o->AddPoint(Lerp(a, b, 0.78), 0.55);
        o->AddPoint(b, 0.75);
        });
}
vtkSmartPointer<vtkColorTransferFunction> TF::MakeCTF_Hot(double min, double max) {
    fixRange(min, max);
    return buildCTF(min, max, [](auto* c, double a, double b) {
        c->AddRGBPoint(a, 0.0, 0.0, 0.0);
        c->AddRGBPoint(Lerp(a, b, 0.32), 0.8, 0.0, 0.0);
        c->AddRGBPoint(Lerp(a, b, 0.63), 1.0, 0.5, 0.0);
        c->AddRGBPoint(Lerp(a, b, 0.86), 1.0, 1.0, 0.0);
        c->AddRGBPoint(b, 1.0, 1.0, 1.0);
        });
}
vtkSmartPointer<vtkPiecewiseFunction> TF::MakeOTF_Hot(double min, double max) {
    fixRange(min, max);
    return buildOTF(min, max, [](auto* o, double a, double b) {
        o->AddPoint(a, 0.00);
        o->AddPoint(Lerp(a, b, 0.31), 0.10);
        o->AddPoint(Lerp(a, b, 0.63), 0.45);
        o->AddPoint(b, 0.90);
        });
}

void TF::InvertInPlace(vtkVolumeProperty* prop, double min, double max)
{
    if (!prop) return;
    fixRange(min, max);

    auto* oldC = prop->GetRGBTransferFunction(0);
    auto* oldO = prop->GetScalarOpacity(0);
    if (!oldC || !oldO) return;

    const double eps = 1e-9;

    // --- инвертируем CTF с фиксацией краёв и сортировкой ---
    struct CNode { double x, r, g, b, mid, sharp; };
    std::vector<CNode> cNodes; cNodes.reserve(oldC->GetSize());
    for (int i = 0, n = oldC->GetSize(); i < n; ++i) {
        double v[6]; oldC->GetNodeValue(i, v); // x,r,g,b,mid,sharp
        double x = v[0];
        // не трогаем крайние точки (минимум/максимум диапазона)
        if (std::abs(x - min) > eps && std::abs(x - max) > eps)
            x = min + max - x;
        cNodes.push_back({ x, v[1], v[2], v[3], v[4], v[5] });
    }
    std::sort(cNodes.begin(), cNodes.end(), [](auto& a, auto& b) { return a.x < b.x; });

    auto newC = vtkSmartPointer<vtkColorTransferFunction>::New();
    for (const auto& n : cNodes)
        newC->AddRGBPoint(n.x, n.r, n.g, n.b, n.mid, n.sharp);

    // --- инвертируем OTF с фиксацией краёв и сортировкой ---
    struct ONode { double x, val, mid, sharp; };
    std::vector<ONode> oNodes; oNodes.reserve(oldO->GetSize());
    for (int i = 0, n = oldO->GetSize(); i < n; ++i) {
        double v[4]; oldO->GetNodeValue(i, v); // x,val,mid,sharp
        double x = v[0];
        if (std::abs(x - min) > eps && std::abs(x - max) > eps)
            x = min + max - x;
        oNodes.push_back({ x, v[1], v[2], v[3] });
    }
    std::sort(oNodes.begin(), oNodes.end(), [](auto& a, auto& b) { return a.x < b.x; });

    auto newO = vtkSmartPointer<vtkPiecewiseFunction>::New();
    for (const auto& n : oNodes)
        newO->AddPoint(n.x, n.val, n.mid, n.sharp);

    prop->SetColor(0, newC);
    prop->SetScalarOpacity(0, newO);
    prop->Modified();
}

QString TF::PresetsRoot() {
    return QCoreApplication::applicationDirPath() + "/Presets";
}

static inline double mapX(double xHist, double a, double b)
{
    const double xClamped = std::clamp(
        xHist,
        static_cast<double>(HistMin),
        static_cast<double>(HistMax)
    );
    const double span = std::max(1.0, static_cast<double>(HistMax - HistMin));
    const double t = (xClamped - static_cast<double>(HistMin)) / span;
    return a + t * (b - a);
}

void TF::ApplyPoints(vtkVolumeProperty* prop,
    const QVector<TFPoint>& pts,
    double min, double max,
    const QString& colorSpace)
{
    if (!prop || pts.isEmpty()) return;
    fixRange(min, max);

    auto c = vtkSmartPointer<vtkColorTransferFunction>::New();
    auto o = vtkSmartPointer<vtkPiecewiseFunction>::New();

    const auto& p0 = pts.front();
    const auto& p1 = pts.back();

    // сторожевые точки теперь тоже в домене HistMin..HistMax
    c->AddRGBPoint(mapX(static_cast<double>(HistMin), min, max),
        p0.r, p0.g, p0.b);
    o->AddPoint(mapX(static_cast<double>(HistMin), min, max),
        std::clamp(p0.a, 0.0, 1.0));

    for (const auto& p : pts) {
        const double X = mapX(p.x, min, max);
        c->AddRGBPoint(X, p.r, p.g, p.b);
        o->AddPoint(X, std::clamp(p.a, 0.0, 1.0));
    }

    c->AddRGBPoint(mapX(static_cast<double>(HistMax), min, max),
        p1.r, p1.g, p1.b);
    o->AddPoint(mapX(static_cast<double>(HistMax), min, max),
        std::clamp(p1.a, 0.0, 1.0));

    if (colorSpace.compare("Lab", Qt::CaseInsensitive) == 0)
        c->SetColorSpaceToLab();
    else if (colorSpace.compare("HSV", Qt::CaseInsensitive) == 0)
        c->SetColorSpaceToHSV();
    else
        c->SetColorSpaceToRGB();

    prop->SetIndependentComponents(true);
    prop->SetColor(0, c);
    prop->SetScalarOpacity(0, o);
    prop->SetInterpolationTypeToLinear();
    prop->Modified();
}

bool TF::SaveCustomPreset(const CustomPreset& P)
{
    QDir().mkpath(PresetsRoot());
    const QString base = P.name.isEmpty() ? "Preset" : P.name;
    const QString file = PresetsRoot() + "/" + base + ".tf.json";

    QJsonObject root;
    root["name"] = base;
    root["version"] = 1;
    root["opacityK"] = P.opacityK;
    root["colorSpace"] = P.colorSpace;

    QJsonArray arr;
    for (const auto& p : P.points) {
        QJsonObject o;
        o["x"] = p.x; o["a"] = p.a; o["r"] = p.r; o["g"] = p.g; o["b"] = p.b;
        arr.push_back(o);
    }
    root["points"] = arr;

    QFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

QVector<TF::CustomPreset> TF::LoadCustomPresets()
{
    QVector<CustomPreset> out;
    QDir d(PresetsRoot());
    if (!d.exists()) return out;

    const auto lis = d.entryInfoList(QStringList() << "*.tf.json", QDir::Files, QDir::Name);
    for (const QFileInfo& fi : lis) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const auto root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();

        CustomPreset P;
        P.name = root.value("name").toString(fi.baseName());
        P.opacityK = root.value("opacityK").toDouble(1.0);
        P.colorSpace = root.value("colorSpace").toString("Lab");
        for (const auto& v : root.value("points").toArray()) {
            const auto o = v.toObject();
            TFPoint p;
            p.x = o.value("x").toDouble();
            p.a = o.value("a").toDouble();
            p.r = o.value("r").toDouble();
            p.g = o.value("g").toDouble();
            p.b = o.value("b").toDouble();
            P.points.push_back(p);
        }
        P.filePath = fi.absoluteFilePath();
        if (!P.points.isEmpty()) out.push_back(std::move(P));
    }
    return out;
}
#include <vtkSphereSource.h>
#include <vtkSuperquadricSource.h>
#include <vtkLineSource.h>
#include <vtkTubeFilter.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkAssembly.h>
#include <vtkProperty.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTransform.h>
#include <vtkOBJReader.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkAssembly.h>
#include <vtkPolyData.h>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>

// материал «пластик»
static inline void NiceMat(vtkActor* a, double r, double g, double b) {
    auto* p = a->GetProperty();
    p->SetColor(r, g, b);
    p->SetAmbient(0.15);
    p->SetDiffuse(0.8);
    p->SetSpecular(0.4);
    p->SetSpecularPower(40);
    p->SetInterpolationToPhong();
}

// трубка между p0–p1
static vtkSmartPointer<vtkActor> Tube(const double p0[3], const double p1[3], double radius, int sides = 24) {
    auto line = vtkSmartPointer<vtkLineSource>::New();
    line->SetPoint1(p0); line->SetPoint2(p1);

    auto tube = vtkSmartPointer<vtkTubeFilter>::New();
    tube->SetInputConnection(line->GetOutputPort());
    tube->SetRadius(radius);
    tube->SetNumberOfSides(sides);
    tube->CappingOn();

    auto map = vtkSmartPointer<vtkPolyDataMapper>::New();
    map->SetInputConnection(tube->GetOutputPort());

    auto act = vtkSmartPointer<vtkActor>::New();
    act->SetMapper(map);
    return act;
}

// шар-сустав / «ладонь» / «ступня»
static vtkSmartPointer<vtkActor> Ball(const double c[3], double r) {
    auto s = vtkSmartPointer<vtkSphereSource>::New();
    s->SetCenter(c); s->SetRadius(r);
    s->SetThetaResolution(32); s->SetPhiResolution(32);

    auto m = vtkSmartPointer<vtkPolyDataMapper>::New();
    m->SetInputConnection(s->GetOutputPort());

    auto a = vtkSmartPointer<vtkActor>::New();
    a->SetMapper(m);
    return a;
}

static vtkSmartPointer<vtkAssembly> MakeHumanMarker2()
{
    auto assm = vtkSmartPointer<vtkAssembly>::New();

    const double bodyCol[3]{ 0.70, 0.90, 0.82 };
    const double jointCol[3]{ 0.96, 0.96, 0.96 };

    // ---------- ТОРС: ещё стройнее ----------
    {
        auto torso = vtkSmartPointer<vtkSphereSource>::New();
        torso->SetCenter(0, 0, 0);
        torso->SetRadius(1.0);
        torso->SetThetaResolution(48);
        torso->SetPhiResolution(48);

        auto Tsp = vtkSmartPointer<vtkTransform>::New();
        vtkTransform* T = Tsp.GetPointer();
        // уже по X и особенно по Y
        T->Scale(0.28, 0.20, 0.40);

        auto tf = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
        tf->SetTransform(T);
        tf->SetInputConnection(torso->GetOutputPort());
        tf->Update();

        auto torsoMap = vtkSmartPointer<vtkPolyDataMapper>::New();
        torsoMap->SetInputConnection(tf->GetOutputPort());

        auto body = vtkSmartPointer<vtkActor>::New();
        body->SetMapper(torsoMap);
        NiceMat(body, bodyCol[0], bodyCol[1], bodyCol[2]);
        body->SetPosition(0, 0, 1.02);
        assm->AddPart(body);
    }

    // ---------- ШЕЯ и ГОЛОВА ----------
    double neckTop[3]{ 0, 0, 1.39 };
    double headC[3]{ 0, -0.05, 1.58 };
    auto neck = Tube(neckTop, headC, 0.075);
    NiceMat(neck, bodyCol[0], bodyCol[1], bodyCol[2]); assm->AddPart(neck);

    auto headSrc = vtkSmartPointer<vtkSphereSource>::New();
    headSrc->SetCenter(headC); headSrc->SetRadius(0.25);
    headSrc->SetThetaResolution(32); headSrc->SetPhiResolution(32);
    auto headMap = vtkSmartPointer<vtkPolyDataMapper>::New();
    headMap->SetInputConnection(headSrc->GetOutputPort());
    auto head = vtkSmartPointer<vtkActor>::New();
    head->SetMapper(headMap);
    NiceMat(head, bodyCol[0], bodyCol[1], bodyCol[2]); assm->AddPart(head);

    // ---------- Плечевой пояс: ниже, руки вниз, локти видны ----------
    double l_sh[3]{ -0.22, 0.02, 1.28 };
    double r_sh[3]{ +0.22, 0.02, 1.28 };
    auto clav = Tube(l_sh, r_sh, 0.050);
    NiceMat(clav, bodyCol[0], bodyCol[1], bodyCol[2]); assm->AddPart(clav);

    // локоть ниже плеча, немного внутрь по Y
    double l_el[3]{ -0.50, 0, 0.95 };
    double l_wr[3]{ -0.64, -0.20, 0.78 };
    double r_el[3]{ +0.50, 0, 0.95 };
    double r_wr[3]{ +0.64, -0.20, 0.78 };


    auto lUpper = Tube(l_sh, l_el, 0.064);
    auto lLower = Tube(l_el, l_wr, 0.056);
    auto rUpper = Tube(r_sh, r_el, 0.064);
    auto rLower = Tube(r_el, r_wr, 0.056);

    NiceMat(lUpper, bodyCol[0], bodyCol[1], bodyCol[2]);
    NiceMat(lLower, bodyCol[0], bodyCol[1], bodyCol[2]);
    NiceMat(rUpper, bodyCol[0], bodyCol[1], bodyCol[2]);
    NiceMat(rLower, bodyCol[0], bodyCol[1], bodyCol[2]);
    assm->AddPart(lUpper); assm->AddPart(lLower);
    assm->AddPart(rUpper); assm->AddPart(rLower);

    auto jl_sh = Ball(l_sh, 0.072), jr_sh = Ball(r_sh, 0.072);
    auto jl_el = Ball(l_el, 0.066), jr_el = Ball(r_el, 0.066);
    auto jl_wr = Ball(l_wr, 0.054), jr_wr = Ball(r_wr, 0.054);
    NiceMat(jl_sh, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jr_sh, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jl_el, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jr_el, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jl_wr, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jr_wr, jointCol[0], jointCol[1], jointCol[2]);
    assm->AddPart(jl_sh); assm->AddPart(jr_sh);
    assm->AddPart(jl_el); assm->AddPart(jr_el);
    assm->AddPart(jl_wr); assm->AddPart(jr_wr);

    // ---------- Таз и ноги: уже, стопы вперёд ----------
    double pelvisC[3]{ 0.0, 0.0, 0.77 };
    auto pelvis = Ball(pelvisC, 0.12); NiceMat(pelvis, bodyCol[0], bodyCol[1], bodyCol[2]); assm->AddPart(pelvis);

    double hipL[3]{ -0.14, 0.00, 0.77 };
    double hipR[3]{ +0.14, 0.00, 0.77 };
    auto pelvisBar = Tube(hipL, hipR, 0.070); NiceMat(pelvisBar, bodyCol[0], bodyCol[1], bodyCol[2]); assm->AddPart(pelvisBar);

    double l_hip[3]{ -0.13, 0.00, 0.75 };
    double l_knee[3]{ -0.13, 0.02, 0.50 };
    double l_ank[3]{ -0.12, 0.05, 0.16 };

    double r_hip[3]{ +0.13, 0.00, 0.75 };
    double r_knee[3]{ +0.13, 0.02, 0.50 };
    double r_ank[3]{ +0.12, 0.05, 0.16 };

    auto lThigh = Tube(l_hip, l_knee, 0.076);
    auto lShin = Tube(l_knee, l_ank, 0.070);
    auto rThigh = Tube(r_hip, r_knee, 0.076);
    auto rShin = Tube(r_knee, r_ank, 0.070);
    NiceMat(lThigh, bodyCol[0], bodyCol[1], bodyCol[2]);
    NiceMat(lShin, bodyCol[0], bodyCol[1], bodyCol[2]);
    NiceMat(rThigh, bodyCol[0], bodyCol[1], bodyCol[2]);
    NiceMat(rShin, bodyCol[0], bodyCol[1], bodyCol[2]);
    assm->AddPart(lThigh); assm->AddPart(lShin);
    assm->AddPart(rThigh); assm->AddPart(rShin);

    auto jl_hip = Ball(l_hip, 0.076);
    auto jl_knee = Ball(l_knee, 0.070);
    auto jl_ank = Ball(l_ank, 0.062);
    auto jr_hip = Ball(r_hip, 0.076);
    auto jr_knee = Ball(r_knee, 0.070);
    auto jr_ank = Ball(r_ank, 0.062);
    NiceMat(jl_hip, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jl_knee, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jl_ank, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jr_hip, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jr_knee, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(jr_ank, jointCol[0], jointCol[1], jointCol[2]);
    assm->AddPart(jl_hip); assm->AddPart(jl_knee); assm->AddPart(jl_ank);
    assm->AddPart(jr_hip); assm->AddPart(jr_knee); assm->AddPart(jr_ank);

    // «носки» вперёд (вперёд = -Y), слегка вниз
    double l_foot[3]{ l_ank[0], l_ank[1] - 0.060, l_ank[2] - 0.045 };
    double r_foot[3]{ r_ank[0], r_ank[1] - 0.060, r_ank[2] - 0.045 };
    auto lFoot = Ball(l_foot, 0.055);
    auto rFoot = Ball(r_foot, 0.055);
    NiceMat(lFoot, jointCol[0], jointCol[1], jointCol[2]);
    NiceMat(rFoot, jointCol[0], jointCol[1], jointCol[2]);
    assm->AddPart(lFoot); assm->AddPart(rFoot);

    assm->SetScale(1.0);
    return assm;
}



static QString EnsureHumanMarkerObjOnDisk()
{
    const QString targetPath = QStringLiteral("C:/Share/Features/Pictures/obj/ref.obj");
    QFileInfo fi(targetPath);

    // 1. создаём каталог, если его нет
    QDir dir(fi.path());
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            qDebug() << "EnsureHumanMarkerObjOnDisk: can't create dir" << fi.path();
            return QString();
        }
    }

    // 2. если файл уже существует и не пустой — ок, используем его
    if (fi.exists() && fi.isFile() && fi.size() > 0) {
        return targetPath;
    }

    // 3. файла нет — копируем из ресурсов
    const QString resPath = QStringLiteral(":/icons/Resources/ref.obj");
    QFile res(resPath);
    if (!res.open(QIODevice::ReadOnly)) {
        qDebug() << "EnsureHumanMarkerObjOnDisk: can't open resource" << resPath;
        return QString();
    }

    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly)) {
        qDebug() << "EnsureHumanMarkerObjOnDisk: can't open for write" << targetPath;
        return QString();
    }

    const qint64 written = out.write(res.readAll());
    out.close();
    res.close();

    if (written <= 0) {
        qDebug() << "EnsureHumanMarkerObjOnDisk: failed to write data to" << targetPath;
        return QString();
    }

    qDebug() << "EnsureHumanMarkerObjOnDisk: copied OBJ to" << targetPath;
    return targetPath;
}

// NiceMat как у тебя выше

static vtkSmartPointer<vtkAssembly> MakeHumanMarker()
{
    auto assm = vtkSmartPointer<vtkAssembly>::New();

    const QString objPath = EnsureHumanMarkerObjOnDisk();
    if (objPath.isEmpty()) {
        qDebug() << "MakeHumanMarker: no OBJ, using empty assembly";
        return MakeHumanMarker2();
    }

    auto reader = vtkSmartPointer<vtkOBJReader>::New();
    const QByteArray pathBA = objPath.toLocal8Bit();
    reader->SetFileName(pathBA.constData());
    reader->Update();

    vtkPolyData* poly = reader->GetOutput();
    if (!poly || poly->GetNumberOfPoints() == 0) {
        qDebug() << "MakeHumanMarker: OBJ is empty:" << objPath;
        return MakeHumanMarker2();
    }

    auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    mapper->SetInputConnection(reader->GetOutputPort());

    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);

    // нормируем размер и центрируем фигурку, чтобы она была аккуратной
    double b[6];
    poly->GetBounds(b);
    const double dx = b[1] - b[0];
    const double dy = b[3] - b[2];
    const double dz = b[5] - b[4];
    const double maxDim = std::max({ dx, dy, dz, 1e-6 });

    const double cx = 0.5 * (b[0] + b[1]);
    const double cy = 0.5 * (b[2] + b[3]);
    const double cz = 0.5 * (b[4] + b[5]);

    const double scale = 2.0 / maxDim;
    actor->SetScale(scale, scale, scale);
    actor->SetPosition(-cx * scale, -cy * scale, -cz * scale);

    NiceMat(actor, 0.70, 0.90, 0.82); // тот же пластик, что был

    assm->AddPart(actor);
    return assm;
}



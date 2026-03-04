#include "ElectrodeSurfaceDetector.h"

#include <queue>
#include <vector>
#include <array>
#include <algorithm>
#include <cmath>
#include <numeric>

#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkSphereSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>
#include <vtkMath.h>

#include <QDebug>

namespace
{
    inline int idx(int x, int y, int z, int nx, int ny) noexcept
    {
        return (z * ny + y) * nx + x;
    }

    inline bool inside(int x, int y, int z, int nx, int ny, int nz) noexcept
    {
        return (x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz);
    }

    struct Off3 { int dx, dy, dz; };

    // 26-соседство (всё 3x3x3 кроме (0,0,0))
    static constexpr Off3 kN26[26] = {
        {-1,-1,-1},{0,-1,-1},{1,-1,-1},
        {-1, 0,-1},{0, 0,-1},{1, 0,-1},
        {-1, 1,-1},{0, 1,-1},{1, 1,-1},

        {-1,-1, 0},{0,-1, 0},{1,-1, 0},
        {-1, 0, 0},           {1, 0, 0},
        {-1, 1, 0},{0, 1, 0},{1, 1, 0},

        {-1,-1, 1},{0,-1, 1},{1,-1, 1},
        {-1, 0, 1},{0, 0, 1},{1, 0, 1},
        {-1, 1, 1},{0, 1, 1},{1, 1, 1}
    };

    struct CompStats
    {
        int count = 0;
        double sx = 0, sy = 0, sz = 0;

        // моменты для ковариации
        double sxx = 0, syy = 0, szz = 0;
        double sxy = 0, sxz = 0, syz = 0;

        void add(int x, int y, int z)
        {
            count++;
            sx += x; sy += y; sz += z;
            sxx += double(x) * x;
            syy += double(y) * y;
            szz += double(z) * z;
            sxy += double(x) * y;
            sxz += double(x) * z;
            syz += double(y) * z;
        }
    };


    struct Dir3 { int dx, dy, dz; };

    // направления “почти сферой”, но без чистых ±Z.
    // dz ограничим, чтобы не улетать в верх/низ.
    static constexpr Dir3 kDirs3[] = {
        { 1, 0, 0 },{-1, 0, 0 },{ 0, 1, 0 },{ 0,-1, 0 },
        { 1, 1, 0 },{ 1,-1, 0 },{-1, 1, 0 },{-1,-1, 0 },
        { 2, 1, 0 },{ 2,-1, 0 },{-2, 1, 0 },{-2,-1, 0 },
        { 1, 2, 0 },{ 1,-2, 0 },{-1, 2, 0 },{-1,-2, 0 },

        // чуть-чуть по Z (для “перед/зад” поверхности)
        { 1, 0, 1 },{ 1, 0,-1 },{-1, 0, 1 },{-1, 0,-1 },
        { 0, 1, 1 },{ 0, 1,-1 },{ 0,-1, 1 },{ 0,-1,-1 },
        { 1, 1, 1 },{ 1, 1,-1 },{ 1,-1, 1 },{ 1,-1,-1 },
        {-1, 1, 1 },{-1, 1,-1 },{-1,-1, 1 },{-1,-1,-1 }
    };

    inline bool isBodyVoxel_1_253(int v) noexcept { return (v >= 1 && v <= 253); }
    inline bool isMetal_254_255(int v) noexcept { return (v >= 254); }
    inline bool isAir_0(int v) noexcept { return (v == 0); }

    // успех: встретили воздух (0) не проходя через тело (1..253).
    // металл (254-255) считаем прозрачным, чтобы можно было “выйти” из компоненты.
    // запрет: если луч пытается уйти через верх/низ (z==0 или z==nz-1) - этот луч считаем невалидным.
    template <class VAtFn>
    bool hasRayToAir_NoBody_NoTopBottom(int x0, int y0, int z0,
        int nx, int ny, int nz,
        VAtFn&& vAt,
        int maxSteps)
    {
        if (z0 <= 5 || z0 >= nz - 6) return false;
        if (x0 <= 5 || x0 >= nx - 6) return false;
        if (y0 <= 5 || y0 >= ny - 6) return false;

        for (const auto& d : kDirs3)
        {
            int x = x0, y = y0, z = z0;
            

            for (int step = 1; step <= maxSteps; ++step)
            {
                x += d.dx; y += d.dy; z += d.dz;

                // вышли за XY границы - это тоже “снаружи”
                if (x <= 0 || x >= nx - 1 || y <= 0 || y >= ny - 1)
                {
                    return true;
                }

                // верх/низ запрещены
                if (z <= 0 || z >= nz - 1)
                    break;

                const int v = vAt(x, y, z);

                if (isBodyVoxel_1_253(v))
                    break; // тело перекрыло

                // металл (254-255) просто пропускаем дальше
            }
        }
        return false;
    }



    struct Cand
    {
        // центроид в voxel
        double cxv = 0, cyv = 0, czv = 0;

        // признаки
        int count = 0;
        double axisRatio = 999.0;   // по sqrt(lambda_max/lambda_min)
        double meanR = 0.0;         // средний радиус до центроида (в вокселях)
        double relStdR = 999.0;     // std(r)/mean(r)
        double score = 0.0;

        std::array<double, 3> world{};
    };

    inline std::array<double, 3> voxelToWorld(const double origin[3], const double spacing[3],
        double vx, double vy, double vz)
    {
        return {
            origin[0] + vx * spacing[0],
            origin[1] + vy * spacing[1],
            origin[2] + vz * spacing[2]
        };
    }

    static double median(std::vector<double> v)
    {
        if (v.empty()) return 0.0;
        const size_t n = v.size();
        std::nth_element(v.begin(), v.begin() + n / 2, v.end());
        double m = v[n / 2];
        if (n % 2 == 0)
        {
            auto it = std::max_element(v.begin(), v.begin() + n / 2);
            m = 0.5 * (m + *it);
        }
        return m;
    }

    static double mad(std::vector<double> v, double med)
    {
        if (v.empty()) return 0.0;
        for (double& x : v) x = std::abs(x - med);
        return median(std::move(v));
    }
}

ElectrodeSurfaceDetector::ElectrodeSurfaceDetector() = default;
ElectrodeSurfaceDetector::~ElectrodeSurfaceDetector() = default;

void ElectrodeSurfaceDetector::clear(vtkRenderer* ren)
{
    if (!ren) { actors_.clear(); return; }
    for (auto& a : actors_) ren->RemoveActor(a);
    actors_.clear();
}

void ElectrodeSurfaceDetector::addSphere(vtkRenderer* ren, const std::array<double, 3>& world)
{
    if (!ren) return;

    vtkNew<vtkSphereSource> s;
    s->SetCenter(world[0], world[1], world[2]);
    s->SetRadius(opt_.sphereRadiusMm);
    s->SetThetaResolution(18);
    s->SetPhiResolution(18);

    vtkNew<vtkPolyDataMapper> m;
    m->SetInputConnection(s->GetOutputPort());

    auto a = vtkSmartPointer<vtkActor>::New();
    a->SetMapper(m);
    a->GetProperty()->SetColor(1.0, 1.0, 0.0);
    a->GetProperty()->SetOpacity(1.0);
    a->PickableOff();

    ren->AddActor(a);
    actors_.push_back(a);
}

std::vector<std::array<double, 3>> ElectrodeSurfaceDetector::detectAndShow(vtkImageData* img, vtkRenderer* ren)
{
    std::vector<std::array<double, 3>> centers;
    if (!img || !ren) return centers;

    clear(ren);

    int dims[3]{ 0,0,0 };
    img->GetDimensions(dims);
    const int nx = dims[0], ny = dims[1], nz = dims[2];
    if (nx <= 2 || ny <= 2 || nz <= 2) return centers;

    double spacing[3]{ 1,1,1 };
    double origin[3]{ 0,0,0 };
    img->GetSpacing(spacing);
    img->GetOrigin(origin);

    auto vAt = [&](int x, int y, int z) -> int {
        return int(img->GetScalarComponentAsDouble(x, y, z, 0) + 0.5);
        };

    const int nvox = nx * ny * nz;

    // 1) metal mask (254-255) внутри тела (если включено)
    std::vector<uint8_t> metal(nvox, 0);
    std::vector<uint8_t> visited(nvox, 0);

    int metalCount = 0;
    for (int z = 1; z < nz - 1; ++z)
        for (int y = 1; y < ny - 1; ++y)
            for (int x = 1; x < nx - 1; ++x)
            {
                const int v = vAt(x, y, z);
                if (v < opt_.metalMin || v > opt_.metalMax) continue;

                const int id = idx(x, y, z, nx, ny);

                metal[id] = 1;
                metalCount++;
            }

    if (opt_.debug)
    {
        qDebug() << "[ElectrodeDetector] dims:" << nx << ny << nz;
        qDebug() << "[ElectrodeDetector] spacing:" << spacing[0] << spacing[1] << spacing[2];
        qDebug() << "[ElectrodeDetector] metalCount:" << metalCount;
        qDebug() << "[ElectrodeDetector] sizeRangeVox=[" << opt_.minComponentVox << ".." << opt_.maxComponentVox << "]"
            << "maxAxisRatioVox=" << opt_.maxAxisRatioVox
            << "useRadiusConsistency=" << opt_.useRadiusConsistency
            << "maxRadiusStdRel=" << opt_.maxRadiusStdRel
            << "useBodyMask=" << opt_.useBodyMask
            << "bodyThr=" << opt_.bodyThreshold;
    }

    // 2) компоненты связности на metal (26 соседей)
    std::queue<std::array<int, 3>> q;

    int compsTotal = 0;
    int rejSize = 0, rejSphere = 0, rejRadius = 0;

    std::vector<Cand> cands;
    cands.reserve(512);

    for (int z = 1; z < nz - 1; ++z)
    {
        for (int y = 1; y < ny - 1; ++y)
        {
            for (int x = 1; x < nx - 1; ++x)
            {
                const int sid = idx(x, y, z, nx, ny);
                if (!metal[sid] || visited[sid]) continue;

                compsTotal++;
                visited[sid] = 1;
                q.push({ x,y,z });

                CompStats st;
                std::vector<std::array<int, 3>> voxels;
                voxels.reserve(opt_.maxComponentVox);

                bool oversize = false;

                while (!q.empty())
                {
                    auto p = q.front(); q.pop();
                    const int cx = p[0], cy = p[1], cz = p[2];

                    st.add(cx, cy, cz);
                    voxels.push_back({ cx,cy,cz });

                    if (st.count > opt_.maxComponentVox)
                    {
                        oversize = true;
                        // доочищать BFS не будем: это точно не электрод
                        // но очередь надо опустошить для этой компоненты: проще просто “продолжить”
                        // и игнорировать добавления дальше (мы уже отметили visited)
                    }

                    for (const auto& o : kN26)
                    {
                        const int xx = cx + o.dx, yy = cy + o.dy, zz = cz + o.dz;
                        if (!inside(xx, yy, zz, nx, ny, nz)) continue;

                        const int nid = idx(xx, yy, zz, nx, ny);
                        if (!metal[nid] || visited[nid]) continue;

                        visited[nid] = 1;
                        q.push({ xx,yy,zz });
                    }
                }

                if (oversize || st.count < opt_.minComponentVox || st.count > opt_.maxComponentVox)
                {
                    rejSize++;
                    continue;
                }

                // 3) центроид в voxel
                const double cxv = st.sx / st.count;
                const double cyv = st.sy / st.count;
                const double czv = st.sz / st.count;

                // 4) ковариация и axisRatio
                const double ex = cxv, ey = cyv, ez = czv;

                const double exx = st.sxx / st.count;
                const double eyy = st.syy / st.count;
                const double ezz = st.szz / st.count;
                const double exy = st.sxy / st.count;
                const double exz = st.sxz / st.count;
                const double eyz = st.syz / st.count;

                double A[3][3];
                A[0][0] = exx - ex * ex;
                A[1][1] = eyy - ey * ey;
                A[2][2] = ezz - ez * ez;
                A[0][1] = A[1][0] = exy - ex * ey;
                A[0][2] = A[2][0] = exz - ex * ez;
                A[1][2] = A[2][1] = eyz - ey * ez;

                double* Ap[3] = { A[0], A[1], A[2] };
                double w[3]{ 0,0,0 };
                double V[3][3];
                double* Vp[3] = { V[0], V[1], V[2] };

                vtkMath::Jacobi(Ap, w, Vp);

                std::sort(w, w + 3, [](double a, double b) { return a > b; });
                const double lmax = std::max(1e-12, w[0]);
                const double lmin = std::max(1e-12, w[2]);
                const double axisRatio = std::sqrt(lmax / lmin);

                if (axisRatio > opt_.maxAxisRatioVox)
                {
                    rejSphere++;
                    continue;
                }

                // 5) радиусная “стабильность” (std/mean)
                double meanR = 0.0;
                double relStd = 0.0;

                Cand c;
                c.cxv = cxv; c.cyv = cyv; c.czv = czv;
                c.count = st.count;
                c.axisRatio = axisRatio;
                c.meanR = meanR;
                c.relStdR = relStd;
                c.world = voxelToWorld(origin, spacing, cxv, cyv, czv);

                // базовый скор: маленький axisRatio и маленький relStd лучше
                // (чисто для ранжирования, а не решающего фильтра)
                const double s1 = std::max(0.0, opt_.maxAxisRatioVox - axisRatio);
                const double s2 = (opt_.useRadiusConsistency) ? std::max(0.0, opt_.maxRadiusStdRel - relStd) : 0.0;
                c.score = 1.0 + 2.0 * s1 + 2.0 * s2;

                cands.push_back(c);
            }
        }
    }

    if (cands.empty())
    {
        if (opt_.debug)
        {
            qDebug() << "[ElectrodeDetector] compsTotal=" << compsTotal
                << "rawCandidates=0"
                << "rejSize=" << rejSize
                << "rejSphere=" << rejSphere
                << "rejRadius=" << rejRadius;
        }
        return centers;
    }

    // 3) similarity-фильтр: оставить группы, которые “похожи друг на друга”
    //    Идея: берём робастный “типичный” электрод по медианам признаков и
    //    выкидываем то, что сильно выбивается.
    //
    //    Признаки: count, meanR, axisRatio, relStdR
    //    Метрика: L1 по нормализованным отклонениям (через MAD).
    std::vector<double> vCount, vMeanR, vAxis, vRel;
    vCount.reserve(cands.size());
    vMeanR.reserve(cands.size());
    vAxis.reserve(cands.size());
    vRel.reserve(cands.size());

    for (const auto& c : cands)
    {
        vCount.push_back(double(c.count));
        vMeanR.push_back(c.meanR);
        vAxis.push_back(c.axisRatio);
        vRel.push_back(c.relStdR);
    }

    const double mCount = median(vCount);
    const double mMeanR = median(vMeanR);
    const double mAxis = median(vAxis);
    const double mRel = median(vRel);

    double madCount = mad(vCount, mCount);
    double madMeanR = mad(vMeanR, mMeanR);
    double madAxis = mad(vAxis, mAxis);
    double madRel = mad(vRel, mRel);

    // защита от нулей
    madCount = std::max(1e-6, madCount);
    madMeanR = std::max(1e-6, madMeanR);
    madAxis = std::max(1e-6, madAxis);
    madRel = std::max(1e-6, madRel);

    const double kDistThr = 35;

    std::vector<Cand> picked;
    picked.reserve(cands.size());
    int rejRay = 0;
    int rejSimilar = 0;

    for (auto& c : cands)
    {
        const double dCount = std::abs(double(c.count) - mCount) / madCount;
        const double dMeanR = std::abs(c.meanR - mMeanR) / madMeanR;
        const double dAxis = std::abs(c.axisRatio - mAxis) / madAxis;
        const double dRel = std::abs(c.relStdR - mRel) / madRel;

        // веса: count и meanR обычно самые стабильные, чуть сильнее давим ими
        const double dist = 1.2 * dCount + 1.2 * dMeanR + 1.0 * dAxis + 0.8 * dRel;

        if (dist > kDistThr)
        {
            rejSimilar++;
            continue;
        }

        {
            const int x0 = int(std::lround(c.cxv));
            const int y0 = int(std::lround(c.cyv));
            const int z0 = int(std::lround(c.czv));

            const int xx = std::clamp(x0, 1, nx - 2);
            const int yy = std::clamp(y0, 1, ny - 2);
            const int zz = std::clamp(z0, 1, nz - 2);

            // шагов много не надо: до “воздуха” обычно близко.
            const int maxSteps = 512; // можно сделать опцией

            if (!hasRayToAir_NoBody_NoTopBottom(xx, yy, zz, nx, ny, nz, vAt, maxSteps))
            {
                rejRay++;
                continue;
            }
        }

        // усилим скор на похожесть, чтоб потом можно было сортировать
        c.score += 0.5 * (kDistThr - dist);
        picked.push_back(c);
    }

    // 4) финал: рисуем сферы в центрах
    // Можно ещё сделать NMS по расстоянию между центрами, но ты сейчас просил “центр группы”,
    // так что оставим как есть.
    std::sort(picked.begin(), picked.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });

    centers.reserve(picked.size());
    for (const auto& c : picked)
    {
        centers.push_back(c.world);
        addSphere(ren, c.world);
    }

    if (opt_.debug)
    {
        qDebug() << "[ElectrodeDetector] compsTotal=" << compsTotal
            << "rawCandidates=" << int(cands.size())
            << "keptAfterShape=" << int(cands.size())
            << "rejSize=" << rejSize
            << "rejSphere=" << rejSphere
            << "rejRadius=" << rejRadius;

        qDebug() << "[ElectrodeDetector] similarity: med(count/meanR/axis/rel)= "
            << mCount << mMeanR << mAxis << mRel
            << "mad=" << madCount << madMeanR << madAxis << madRel
            << "rejSimilar=" << rejSimilar
            << "final=" << int(centers.size());

        qDebug() << "[ElectrodeDetector] ... rejSimilar=" << rejSimilar
            << "rejRay=" << rejRay
            << "final=" << int(centers.size());
    }

    return centers;
}
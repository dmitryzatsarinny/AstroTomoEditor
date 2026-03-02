#include "ElectrodeSurfaceDetector.h"

#include <queue>
#include <vector>
#include <algorithm>
#include <cmath>

#include <vtkImageData.h>
#include <vtkRenderer.h>
#include <vtkSphereSource.h>
#include <vtkPolyDataMapper.h>
#include <vtkActor.h>
#include <vtkProperty.h>

#include <QDebug>

namespace
{
    struct Cand
    {
        std::array<double, 3> world{};
        std::array<double, 3> normalWorld{}; // можно потом рисовать стрелки, если захочешь
        double score = 0.0;
        int vox = 0;
        int boundaryFaces = 0;
    };

    inline double sqr(double x) { return x * x; }

    inline double norm3(const std::array<double, 3>& v)
    {
        return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    inline std::array<double, 3> normalize3(const std::array<double, 3>& v)
    {
        const double n = norm3(v);
        if (n <= 1e-12) return { 0,0,0 };
        return { v[0] / n, v[1] / n, v[2] / n };
    }

    inline double distMm2(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        return sqr(a[0] - b[0]) + sqr(a[1] - b[1]) + sqr(a[2] - b[2]);
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

    vtkNew<vtkSphereSource> sphere;
    sphere->SetCenter(world[0], world[1], world[2]);
    sphere->SetRadius(opt_.sphereRadiusMm);
    sphere->SetThetaResolution(18);
    sphere->SetPhiResolution(18);
    sphere->Update();

    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(sphere->GetOutputPort());

    vtkSmartPointer<vtkActor> actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(1.0, 1.0, 0.0);
    actor->GetProperty()->SetOpacity(1.0);
    actor->PickableOff();

    ren->AddActor(actor);
    actors_.push_back(actor);
}

std::vector<std::array<double, 3>> ElectrodeSurfaceDetector::detectAndShow(vtkImageData* img, vtkRenderer* ren)
{
    std::vector<std::array<double, 3>> centers;
    if (!img || !ren) return centers;

    clear(ren);

    int dims[3] = { 0,0,0 };
    img->GetDimensions(dims);
    const int nx = dims[0], ny = dims[1], nz = dims[2];
    if (nx <= 2 || ny <= 2 || nz <= 2) return centers;

    double spacing[3] = { 1,1,1 };
    double origin[3] = { 0,0,0 };
    img->GetSpacing(spacing);
    img->GetOrigin(origin);

    auto vAt = [&](int x, int y, int z) -> int
        {
            const double v = img->GetScalarComponentAsDouble(x, y, z, 0);
            return int(v + 0.5);
        };

    auto inside = [&](int x, int y, int z) -> bool
        {
            return (x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz);
        };

    double r[2] = { 0,0 };
    img->GetScalarRange(r);

    if (opt_.debug)
    {
        qDebug() << "[ElectrodeDetector] dims:" << nx << ny << nz;
        qDebug() << "[ElectrodeDetector] spacing:" << spacing[0] << spacing[1] << spacing[2];
        qDebug() << "[ElectrodeDetector] scalarType:" << img->GetScalarTypeAsString()
            << "range(vtk):" << r[0] << "to" << r[1];
        qDebug() << "[ElectrodeDetector] params:"
            << "metal=[" << opt_.metalMin << ".." << opt_.metalMax << "]"
            << "bodyThr=" << opt_.bodyThreshold
            << "surfaceRadiusVox=" << opt_.surfaceRadiusVox
            << "compVox=[" << opt_.minComponentVox << ".." << opt_.maxComponentVox << "]"
            << "minDistMm=" << opt_.minDistanceMm
            << "outMaxMm=" << opt_.outwardMaxMm
            << "inMinMm=" << opt_.inwardMinMm;
    }

    const int nvox = nx * ny * nz;
    std::vector<uint8_t> body(nvox, 0);
    std::vector<uint8_t> surface(nvox, 0);
    std::vector<uint8_t> candidate(nvox, 0);
    std::vector<uint8_t> visited(nvox, 0);

    const int dx6[6] = { 1,-1, 0, 0, 0, 0 };
    const int dy6[6] = { 0, 0, 1,-1, 0, 0 };
    const int dz6[6] = { 0, 0, 0, 0, 1,-1 };

    // 1) body mask
    int bodyCount = 0;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
            {
                const int v = vAt(x, y, z);
                if (v >= opt_.bodyThreshold)
                {
                    body[idx(x, y, z, nx, ny)] = 1;
                    bodyCount++;
                }
            }

    // 2) surface mask (граница тела)
    int surfaceCount = 0;
    for (int z = 1; z < nz - 1; ++z)
        for (int y = 1; y < ny - 1; ++y)
            for (int x = 1; x < nx - 1; ++x)
            {
                const int id = idx(x, y, z, nx, ny);
                if (!body[id]) continue;

                bool isSurf = false;
                for (int k = 0; k < 6; ++k)
                {
                    const int nid = idx(x + dx6[k], y + dy6[k], z + dz6[k], nx, ny);
                    if (!body[nid]) { isSurf = true; break; }
                }
                if (isSurf)
                {
                    surface[id] = 1;
                    surfaceCount++;
                }
            }

    // 3) candidate = metal && near surface
    const int R = std::max(0, opt_.surfaceRadiusVox);

    int metalCount = 0;
    int candCount = 0;
    for (int z = 1; z < nz - 1; ++z)
    {
        for (int y = 1; y < ny - 1; ++y)
        {
            for (int x = 1; x < nx - 1; ++x)
            {
                const int v = vAt(x, y, z);
                if (v < opt_.metalMin || v > opt_.metalMax) continue;
                metalCount++;

                const int id = idx(x, y, z, nx, ny);
                if (!body[id]) continue; // металл должен быть внутри маски тела

                bool nearSurf = false;
                if (surface[id]) nearSurf = true;
                else if (R > 0)
                {
                    for (int dz = -R; dz <= R && !nearSurf; ++dz)
                        for (int dy = -R; dy <= R && !nearSurf; ++dy)
                            for (int dx = -R; dx <= R; ++dx)
                            {
                                const int xx = x + dx, yy = y + dy, zz = z + dz;
                                if (!inside(xx, yy, zz)) continue;
                                if (surface[idx(xx, yy, zz, nx, ny)]) { nearSurf = true; break; }
                            }
                }

                if (nearSurf)
                {
                    candidate[id] = 1;
                    candCount++;
                }
            }
        }
    }

    if (opt_.debug)
    {
        qDebug() << "[ElectrodeDetector] bodyCount:" << bodyCount << "surfaceCount:" << surfaceCount;
        qDebug() << "[ElectrodeDetector] metalCount:" << metalCount << "candidateCount:" << candCount;
    }

    auto sampleBody = [&](double fx, double fy, double fz) -> uint8_t
        {
            const int x = int(std::lround(fx));
            const int y = int(std::lround(fy));
            const int z = int(std::lround(fz));
            if (!inside(x, y, z)) return 0;
            return body[idx(x, y, z, nx, ny)];
        };

    auto voxelToWorld = [&](double vx, double vy, double vz) -> std::array<double, 3>
        {
            return {
                origin[0] + vx * spacing[0],
                origin[1] + vy * spacing[1],
                origin[2] + vz * spacing[2]
            };
        };

    // 4) Connected components + оценка нормали наружу
    std::queue<std::array<int, 3>> q;
    int compTotal = 0, bfsPops = 0, rejSize = 0, rejElong = 0, rej資料 = 0;

    std::vector<Cand> cands;
    cands.reserve(256);

    for (int z = 1; z < nz - 1; ++z)
    {
        for (int y = 1; y < ny - 1; ++y)
        {
            for (int x = 1; x < nx - 1; ++x)
            {
                const int seedId = idx(x, y, z, nx, ny);
                if (!candidate[seedId] || visited[seedId]) continue;

                compTotal++;
                visited[seedId] = 1;
                q.push({ x,y,z });

                int count = 0;
                double sx = 0, sy = 0, sz = 0;
                int minx = x, maxx = x, miny = y, maxy = y, minz = z, maxz = z;

                // “нормаль наружу” по граням body->air
                // суммируем направления к соседу-воздуху (dx6[k],dy6[k],dz6[k])
                double nxSum = 0, nySum = 0, nzSum = 0;
                int boundaryFaces = 0;

                while (!q.empty())
                {
                    bfsPops++;
                    auto p = q.front(); q.pop();
                    const int cx = p[0], cy = p[1], cz = p[2];

                    count++;
                    sx += cx; sy += cy; sz += cz;

                    minx = std::min(minx, cx); maxx = std::max(maxx, cx);
                    miny = std::min(miny, cy); maxy = std::max(maxy, cy);
                    minz = std::min(minz, cz); maxz = std::max(maxz, cz);

                    // грани, выходящие в воздух
                    const int id = idx(cx, cy, cz, nx, ny);
                    for (int k = 0; k < 6; ++k)
                    {
                        const int xx = cx + dx6[k];
                        const int yy = cy + dy6[k];
                        const int zz = cz + dz6[k];
                        if (!inside(xx, yy, zz)) continue;

                        const int nid = idx(xx, yy, zz, nx, ny);
                        if (!body[nid]) // сосед не тело -> это наружная грань
                        {
                            boundaryFaces++;
                            nxSum += dx6[k];
                            nySum += dy6[k];
                            nzSum += dz6[k];
                        }
                    }

                    // BFS по candidate
                    for (int k = 0; k < 6; ++k)
                    {
                        const int xx = cx + dx6[k];
                        const int yy = cy + dy6[k];
                        const int zz = cz + dz6[k];
                        if (xx <= 0 || yy <= 0 || zz <= 0 || xx >= nx - 1 || yy >= ny - 1 || zz >= nz - 1)
                            continue;

                        const int nid = idx(xx, yy, zz, nx, ny);
                        if (!candidate[nid] || visited[nid]) continue;

                        visited[nid] = 1;
                        q.push({ xx,yy,zz });
                    }
                }

                if (count < opt_.minComponentVox || count > opt_.maxComponentVox)
                {
                    rejSize++;
                    continue;
                }

                // форма (отсекаем провода/края стола и т.д.)
                const int bx = (maxx - minx + 1);
                const int by = (maxy - miny + 1);
                const int bz = (maxz - minz + 1);
                const int bmax = std::max(bx, std::max(by, bz));
                const int bmin = std::max(1, std::min(bx, std::min(by, bz)));
                const double elong = double(bmax) / double(bmin);
                if (elong > opt_.maxElongation)
                {
                    rejElong++;
                    continue;
                }

                // если нет наружных граней — это не “на коже”
                const double normLen = std::sqrt(nxSum * nxSum + nySum * nySum + nzSum * nzSum);
                if (boundaryFaces <= 0 || (normLen / std::max(1, boundaryFaces)) < opt_.minNormalLen)
                {
                    rej資料++;
                    continue;
                }

                const double cxv = sx / double(count);
                const double cyv = sy / double(count);
                const double czv = sz / double(count);

                // нормаль в voxel space -> unit
                std::array<double, 3> nvox = normalize3({ nxSum, nySum, nzSum });

                // Проверка “смотрит наружу”:
                //  - по +nvox мы должны выйти из body за outwardMaxMm
                //  - по -nvox мы должны оставаться в body хотя бы inwardMinMm
                const double step = std::max(0.1, opt_.rayStepVox);

                const double outMaxVox = opt_.outwardMaxMm / std::max(1e-6, std::min({ spacing[0], spacing[1], spacing[2] }));
                const double inMinVox = opt_.inwardMinMm / std::max(1e-6, std::min({ spacing[0], spacing[1], spacing[2] }));

                bool okOut = false;
                {
                    double fx = cxv, fy = cyv, fz = czv;
                    double traveled = 0.0;
                    for (; traveled <= outMaxVox; traveled += step)
                    {
                        fx += nvox[0] * step;
                        fy += nvox[1] * step;
                        fz += nvox[2] * step;
                        if (sampleBody(fx, fy, fz) == 0) { okOut = true; break; } // вышли в воздух
                    }
                }

                bool okIn = true;
                {
                    double fx = cxv, fy = cyv, fz = czv;
                    double traveled = 0.0;
                    for (; traveled <= inMinVox; traveled += step)
                    {
                        fx -= nvox[0] * step;
                        fy -= nvox[1] * step;
                        fz -= nvox[2] * step;
                        if (sampleBody(fx, fy, fz) == 0) { okIn = false; break; } // “внутрь” провалились в воздух -> плохой знак
                    }
                }

                if (!okOut || !okIn)
                    continue;

                // скоринг: больше = лучше
                // boundaryFaces хорошо отделяет “на коже” от “внутри артефактов”
                const double boundaryFrac = double(boundaryFaces) / double(std::max(1, 6 * count));
                const double score = double(count) * (0.5 + boundaryFrac);

                Cand c;
                c.world = voxelToWorld(cxv, cyv, czv);

                // нормаль в world (с учётом spacing)
                std::array<double, 3> nWorld = { nvox[0] * spacing[0], nvox[1] * spacing[1], nvox[2] * spacing[2] };
                c.normalWorld = normalize3(nWorld);

                c.score = score;
                c.vox = count;
                c.boundaryFaces = boundaryFaces;

                cands.push_back(c);

                if (opt_.debug)
                {
                    qDebug() << "[ElectrodeDetector] comp"
                        << compTotal
                        << "vox:" << count
                        << "bbox:" << bx << by << bz
                        << "elong:" << elong
                        << "boundaryFaces:" << boundaryFaces
                        << "score:" << score
                        << "world:" << c.world[0] << c.world[1] << c.world[2];
                }
            }
        }
    }

    if (opt_.debug)
    {
        qDebug() << "[ElectrodeDetector] components total=" << compTotal
            << "rawCandidates=" << int(cands.size())
            << "rejSize=" << rejSize
            << "rejElong=" << rejElong
            << "rejNormalOrFacing=" << rej資料
            << "bfsPops=" << bfsPops;
    }

    // 5) NMS: в радиусе 2 см оставляем только лучшего
    std::sort(cands.begin(), cands.end(),
        [](const Cand& a, const Cand& b) { return a.score > b.score; });

    std::vector<Cand> picked;
    picked.reserve(cands.size());

    const double minD2 = opt_.minDistanceMm * opt_.minDistanceMm;

    for (const auto& c : cands)
    {
        bool tooClose = false;
        for (const auto& p : picked)
        {
            if (distMm2(c.world, p.world) < minD2)
            {
                tooClose = true;
                break;
            }
        }
        if (!tooClose)
            picked.push_back(c);
    }

    // 6) показать
    centers.reserve(picked.size());
    for (const auto& c : picked)
    {
        centers.push_back(c.world);
        addSphere(ren, c.world);
    }

    if (opt_.debug)
        qDebug() << "[ElectrodeDetector] final centers:" << int(centers.size());

    return centers;
}
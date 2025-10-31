#include "VolumeFix3DR.h"
#include <vtkImageData.h>
#include <vtkImagePermute.h>
#include <vtkImageFlip.h>
#include <vtkImageAlgorithm.h>  // важно!

inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | ((v & 0xFF) << 8)); }

vtkSmartPointer<vtkImageData> FixAxesFor3DR(vtkImageData* src, bool flipX, bool flipY, bool flipZ)
{
    if (!src) return nullptr;

    vtkAlgorithmOutput* port = nullptr;                         // текущий «выхлоп» пайплайна
    vtkSmartPointer<vtkImageAlgorithm> lastAlg = nullptr;       // последний фильтр (если был)

    // 1) Меняем Y<->Z (X, Z, Y)
    vtkNew<vtkImagePermute> perm;
    perm->SetInputData(src);
    perm->SetFilteredAxes(0, 2, 1);  // i->X, j->Z, k->Y
    perm->Update();
    port = perm->GetOutputPort();
    lastAlg = perm;

    // хелпер для аккуратного подключения флипа к текущему порту
    auto addFlip = [&](int axis) {
        vtkNew<vtkImageFlip> flip;
        flip->SetFilteredAxis(axis);         // 0=X, 1=Y, 2=Z (уже НОВЫЕ оси после permute)
        flip->SetInputConnection(port);      // <<<<< ВАЖНО: цепляемся к ТЕКУЩЕМУ портy
        flip->Update();
        port = flip->GetOutputPort();     // теперь «головой» цепочки становится flip
        lastAlg = flip;
        };

    // 2) Опциональные отражения в НОВОЙ системе осей
    if (flipX) addFlip(0);
    if (flipY) addFlip(1);
    if (flipZ) addFlip(2);

    // 3) Снять результат
    auto out = vtkSmartPointer<vtkImageData>::New();
    if (lastAlg) {
        out->DeepCopy(vtkImageData::SafeDownCast(lastAlg->GetOutput()));
    }
    else {
        // (теоретически не случится, т.к. permute всегда есть; но на всякий случай)
        out->DeepCopy(src);
    }
    return out;
}

vtkSmartPointer<vtkImageData> Load3DR_Normalized(const QString& path, bool& IsMRI, bool flipX, bool flipY, bool flipZ)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;

    _3Dinfo hdr{};
    if (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != sizeof(hdr)) return nullptr;

    const int nx = int(hdr.UIheader[0]);
    const int ny = int(hdr.UIheader[1]);
    const int nz = int(hdr.UIheader[2]);
    // const int t  = int(hdr.UIheader[3]); // если не используешь, можно игнорировать

    if (nx <= 0 || ny <= 0 || nz <= 0) return nullptr;

    // предполагаем uint16_t воксели (замени при необходимости)
    const size_t needBytes = size_t(nx) * ny * nz * sizeof(uint16_t);
    if (size_t(f.size()) < 512 + needBytes) return nullptr;

    // читаем «сырые» данные
    QByteArray vox = f.read(needBytes);
    if (size_t(vox.size()) != needBytes) return nullptr;

    // собираем vtkImageData
    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetExtent(0, nx - 1, 0, ny - 1, 0, nz - 1);
    img->AllocateScalars(VTK_UNSIGNED_SHORT, 1);

    std::memcpy(img->GetScalarPointer(), vox.constData(), needBytes);

    // физические размеры (spacing)
    // Doubheader хранит «размер в тех же единицах»: трактуем как пиксельный шаг
    img->SetSpacing(hdr.Doubheader[0],
        hdr.Doubheader[1],
        hdr.Doubheader[2]);
    img->SetOrigin(0.0, 0.0, 0.0);

    if (hdr.IsMRI)
        IsMRI = true;
    else
        IsMRI = false;

    // если в заголовке пометили, что нужно свапать Y/Z — делаем это сейчас
    const bool needSwapYZ = (hdr.IsYZSwap == 0);
    if (needSwapYZ) {
        img = FixAxesFor3DR(img, flipX, flipY, flipZ);
    }
    return img;
}

vtkSmartPointer<vtkImageData> fixAxesIfNeeded(vtkImageData* src, bool swapYZ, bool flipX, bool flipY, bool flipZ)
{
    if (!src) return nullptr;

    vtkAlgorithmOutput* port = nullptr;
    vtkSmartPointer<vtkImageAlgorithm> lastAlg = nullptr;

    // 1) Перестановка осей Y<->Z (если нужно)
    if (swapYZ) {
        vtkNew<vtkImagePermute> perm;
        perm->SetInputData(src);
        perm->SetFilteredAxes(0, 2, 1); // X, Z, Y
        perm->Update();
        port = perm->GetOutputPort();
        lastAlg = perm;
    }

    // Хелпер для добавления флипа к текущему порту (или напрямую к src)
    auto addFlip = [&](int axis) {
        vtkNew<vtkImageFlip> flip;
        flip->SetFilteredAxis(axis);          // 0=X, 1=Y, 2=Z
        if (port) flip->SetInputConnection(port);
        else      flip->SetInputData(src);
        flip->Update();
        port = flip->GetOutputPort();
        lastAlg = flip;
        };

    // 2) Опциональные отражения в НОВОЙ системе осей
    if (flipX) addFlip(0);
    if (flipY) addFlip(1);
    if (flipZ) addFlip(2);

    // 3) Снять результат
    auto out = vtkSmartPointer<vtkImageData>::New();
    if (lastAlg) {
        out->DeepCopy(vtkImageData::SafeDownCast(lastAlg->GetOutput()));
    }
    else {
        // ни permute, ни flip — возвращаем копию исходных данных
        out->DeepCopy(src);
    }
    return out;
}

vtkSmartPointer<vtkImageData> read3dr_asVtk(const QString& path, _3Dinfo* outHdr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    if (f.size() < 512) return nullptr;

    _3Dinfo hdr{};
    if (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != qint64(sizeof(hdr))) return nullptr;
    if (outHdr) *outHdr = hdr;

    const uint32_t nx = hdr.UIheader[0];
    const uint32_t ny = hdr.UIheader[1];
    const uint32_t nz = hdr.UIheader[2];
    if (!nx || !ny || !nz) return nullptr;

    // формат: 1 байт на воксель (u8), X-fast, затем Y, затем Z
    const quint64 voxels = quint64(nx) * ny * nz;
    const quint64 need = 512 + voxels;
    if (quint64(f.size()) < need) return nullptr;

    QByteArray vox = f.read(voxels);
    if (vox.size() != qint64(voxels)) return nullptr;

    // собираем u8-объём
    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetExtent(0, int(nx) - 1, 0, int(ny) - 1, 0, int(nz) - 1);
    img->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::memcpy(img->GetScalarPointer(), vox.constData(), size_t(voxels));

    // физика
    img->SetSpacing(hdr.Doubheader[0], hdr.Doubheader[1], hdr.Doubheader[2]);
    img->SetOrigin(0.0, 0.0, 0.0);

    // если флаг = 1 — меняем местами Y и Z сразу при загрузке
    const bool needSwapYZ = (hdr.IsYZSwap == 0);
    // при необходимости можно эвристически включить flipZ (зависит от твоего 3dr)
    const bool flipZ = false;

    return fixAxesIfNeeded(img, needSwapYZ, flipZ);
}

vtkSmartPointer<vtkImageData> read3dr_asVtk_noflip(const QString& path, _3Dinfo* outHdr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return nullptr;
    if (f.size() < 512) return nullptr;

    _3Dinfo hdr{};
    if (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != qint64(sizeof(hdr))) return nullptr;
    if (outHdr) *outHdr = hdr;

    const uint32_t nx = hdr.UIheader[0];
    const uint32_t ny = hdr.UIheader[1];
    const uint32_t nz = hdr.UIheader[2];
    if (!nx || !ny || !nz) return nullptr;

    // формат: 1 байт на воксель (u8), X-fast, затем Y, затем Z
    const quint64 voxels = quint64(nx) * ny * nz;
    const quint64 need = 512 + voxels;
    if (quint64(f.size()) < need) return nullptr;

    QByteArray vox = f.read(voxels);
    if (vox.size() != qint64(voxels)) return nullptr;

    // собираем u8-объём
    auto img = vtkSmartPointer<vtkImageData>::New();
    img->SetExtent(0, int(nx) - 1, 0, int(ny) - 1, 0, int(nz) - 1);
    img->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::memcpy(img->GetScalarPointer(), vox.constData(), size_t(voxels));

    // физика
    img->SetSpacing(hdr.Doubheader[0], hdr.Doubheader[1], hdr.Doubheader[2]);
    img->SetOrigin(0.0, 0.0, 0.0);

    return img;
}
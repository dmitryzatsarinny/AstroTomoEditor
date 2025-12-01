// Volume.h (тот же файл, что ты прислал — дополняем класс Volume)
#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include <vtkSmartPointer.h>
#include <vtkImageData.h>
#include "../../Services/DicomRange.h"

struct U8Span {
    bool      valid{ false };
    int       ext[6]{ 0,0,0,0,0,0 };
    int       nx{ 0 }, ny{ 0 }, nz{ 0 };
    vtkIdType incX{ 0 }, incY{ 0 }, incZ{ 0 };   // байт-инкременты
    const uint8_t* p0{ nullptr };                // чтение
    uint8_t* p0w{ nullptr };                     // запись

    void invalidate() { *this = U8Span{}; }
    void reset() { *this = U8Span{}; }

    size_t idxRel(int i, int j, int k) const {
        return size_t(k) * size_t(nx) * size_t(ny) + size_t(j) * size_t(nx) + size_t(i);
    }
    size_t size() const { return size_t(nx) * size_t(ny) * size_t(nz); }
};

class Volume {
public:
    Volume() = default;

    void set(vtkSmartPointer<vtkImageData> im) {
        mIm = std::move(im);
        rebuildU8();
    }
    void set(vtkImageData* im) {
        mIm = im;
        rebuildU8();
    
    }

    vtkSmartPointer<vtkImageData> cloneImage(vtkImageData* src)
    {
        if (!src) return nullptr;
        auto out = vtkSmartPointer<vtkImageData>::New();
        out->DeepCopy(src); 
        return out;
    }

    void copy(vtkImageData* im) 
    {
        mIm = cloneImage(im);
        rebuildU8();
    }

    vtkImageData* raw() const { return mIm.GetPointer(); }
    vtkSmartPointer<vtkImageData> smart() const { return mIm; }

    const U8Span& u8() const { return mU8; }
    U8Span& u8() { return mU8; }

    inline uint8_t& at(size_t n) {
        const auto& S = mU8;
        return *reinterpret_cast<uint8_t*>(
            reinterpret_cast<uint8_t*>(S.p0w) + n);
    }

    inline const uint8_t& at(size_t n) const {
        const auto& S = mU8;
        return *reinterpret_cast<const uint8_t*>(
            reinterpret_cast<const uint8_t*>(S.p0) + n);
    }

    inline uint8_t& at(int i, int j, int k) {
        const auto& S = mU8;
        auto* p = reinterpret_cast<uint8_t*>(S.p0w)
            + (i - S.ext[0]) * S.incX
            + (j - S.ext[2]) * S.incY
            + (k - S.ext[4]) * S.incZ;
        return *p;
    }

    inline const uint8_t& at(int i, int j, int k) const {
        const auto& S = mU8;
        auto* p = reinterpret_cast<const uint8_t*>(S.p0)
            + (i - S.ext[0]) * S.incX
            + (j - S.ext[2]) * S.incY
            + (k - S.ext[4]) * S.incZ;
        return *p;
    }

    vtkSmartPointer<vtkImageData> clone() const {
        if (!mIm) return nullptr;
        auto c = vtkSmartPointer<vtkImageData>::New();
        c->DeepCopy(mIm);
        return c;
    }

    // Пустой U8×1 том той же геометрии (extent/spacing/origin/direction)
    vtkSmartPointer<vtkImageData> makeLikeU8() const {
        if (!mIm) return nullptr;
        auto out = vtkSmartPointer<vtkImageData>::New();
        out->CopyStructure(mIm);
        out->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        return out;
    }

    // Создать бинарную (0/1) маску той же геометрии и заполнить по предикату
    // pred(v) -> true => 1, иначе 0
    vtkSmartPointer<vtkImageData> toBinary(const std::function<bool(uint8_t)>& pred) const {
        if (!mU8.valid) return nullptr;
        auto bin = makeLikeU8();
        if (!bin) return nullptr;

        const auto& S = mU8;
        auto* dst0 = static_cast<unsigned char*>(bin->GetScalarPointer(S.ext[0], S.ext[2], S.ext[4]));
        if (!dst0) return nullptr;

        vtkIdType bix, biy, biz;
        bin->GetIncrements(bix, biy, biz);

        const uint8_t* src_z = S.p0;
        unsigned char* dst_z = dst0;

        for (int k = 0; k < S.nz; ++k) {
            const uint8_t* src_y = src_z;
            unsigned char* dst_y = dst_z;
            for (int j = 0; j < S.ny; ++j) {
                const uint8_t* src_x = src_y;
                unsigned char* dst_x = dst_y;
                for (int i = 0; i < S.nx; ++i) {
                    const uint8_t v = *src_x;
                    *dst_x = pred(v) ? 1u : 0u;
                    src_x += S.incX;
                    dst_x += bix;
                }
                src_y += S.incY;
                dst_y += biy;
            }
            src_z += S.incZ;
            dst_z += biz;
        }
        return bin;
    }

    // Удобный вариант: заполнить бинарную маску по LUT[256] (0/1)
    vtkSmartPointer<vtkImageData> toBinaryLUT(const std::array<uint8_t, 256>& lut) const {
        return toBinary([&](uint8_t v) { return lut[v] != 0; });
    }

    // Быстро перезаполнить уже созданную бинарную маску (той же геометрии)
    void fillBinary(vtkImageData* bin, const std::function<bool(uint8_t)>& pred) const {
        if (!bin || !mU8.valid) return;
        const auto& S = mU8;

        // Проверим совпадение extent (иначе лучше пересоздать)
        int bext[6]; bin->GetExtent(bext);
        for (int i = 0; i < 6; ++i) if (bext[i] != S.ext[i]) return;

        auto* dst0 = static_cast<unsigned char*>(bin->GetScalarPointer(S.ext[0], S.ext[2], S.ext[4]));
        if (!dst0) return;

        vtkIdType bix, biy, biz;
        bin->GetIncrements(bix, biy, biz);

        const uint8_t* src_z = S.p0;
        unsigned char* dst_z = dst0;

        for (int k = 0; k < S.nz; ++k) {
            const uint8_t* src_y = src_z;
            unsigned char* dst_y = dst_z;
            for (int j = 0; j < S.ny; ++j) {
                const uint8_t* src_x = src_y;
                unsigned char* dst_x = dst_y;
                for (int i = 0; i < S.nx; ++i) {
                    const uint8_t v = *src_x;
                    *dst_x = pred(v) ? 1u : 0u;
                    src_x += S.incX;
                    dst_x += bix;
                }
                src_y += S.incY;
                dst_y += biy;
            }
            src_z += S.incZ;
            dst_z += biz;
        }
    }

    void rebuildU8() {
        mU8.reset();
        if (!mIm) return;

        if (mIm->GetScalarType() != VTK_UNSIGNED_CHAR ||
            mIm->GetNumberOfScalarComponents() != 1) {
            return; // оставляем невалидным
        }

        int ext[6]; mIm->GetExtent(ext);
        vtkIdType incX{ 0 }, incY{ 0 }, incZ{ 0 };
        mIm->GetIncrements(incX, incY, incZ);

        auto* p0 = static_cast<uint8_t*>(mIm->GetScalarPointer(ext[0], ext[2], ext[4]));
        if (!p0) return;

        mU8.valid = true;
        for (int i = 0; i < 6; ++i) mU8.ext[i] = ext[i];
        mU8.nx = ext[1] - ext[0] + 1;
        mU8.ny = ext[3] - ext[2] + 1;
        mU8.nz = ext[5] - ext[4] + 1;
        mU8.incX = incX; mU8.incY = incY; mU8.incZ = incZ;
        mU8.p0 = p0;
        mU8.p0w = p0;
    }

    vtkImageData* getbin(const std::function<bool(uint8_t)>& pred) const {
        if (!mU8.valid) { mBinCache = nullptr; return nullptr; }
        mBinCache = toBinary(pred);                   // создаём/пересоздаём кэш
        return mBinCache.GetPointer();
    }

    void set(vtkImageData* src, std::function<bool(uint8_t)> pred)
    {
        if (!src) { clear(); return; }

        // --- 1) геометрия исходника
        int ext[6]; src->GetExtent(ext);
        const int nx = ext[1] - ext[0] + 1;
        const int ny = ext[3] - ext[2] + 1;
        const int nz = ext[5] - ext[4] + 1;

        // --- 2) готовим новый vtkImageData под бинарную маску (UCHAR x 1)
        vtkNew<vtkImageData> mask;
        mask->SetExtent(ext);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

        // --- 3) указатели и инкременты
        const auto* srcPtr = static_cast<const uint8_t*>(src->GetScalarPointer());
        auto* dstPtr = static_cast<uint8_t*>(mask->GetScalarPointer());

        const vtkIdType* sInc = src->GetIncrements();
        const vtkIdType* dInc = mask->GetIncrements();
        const vtkIdType sx = sInc[0], sy = sInc[1], sz = sInc[2];
        const vtkIdType dx = dInc[0], dy = dInc[1], dz = dInc[2];

        // --- 4) проход по объему с учетом страйдов
        for (int z = 0; z < nz; ++z) {
            const auto* sZ = srcPtr + (z + ext[4]) * sz;
            auto* dZ = dstPtr + (z + ext[4]) * dz;
            for (int y = 0; y < ny; ++y) {
                const auto* sY = sZ + (y + ext[2]) * sy;
                auto* dY = dZ + (y + ext[2]) * dy;
                for (int x = 0; x < nx; ++x) {
                    const uint8_t v = sY[x * sx];
                    dY[x * dx] = pred(v) ? HistMax : HistMin;
                }
            }
        }

        // --- 5) фиксируем как текущий том и настраиваем быстрый U8-доступ
        mIm = mask;          // сохраняем новую маску
        rebuildU8();         // mU8.valid=true, p0 указывает на маску
    }

    void clear() {
        mIm = nullptr;
        mU8.reset();
    }

    bool isEmpty() const { return mIm == nullptr; }
    explicit operator bool() const { return !isEmpty(); }

private:
    vtkSmartPointer<vtkImageData> mIm;
    U8Span mU8;

    mutable vtkSmartPointer<vtkImageData> mBinCache;
};

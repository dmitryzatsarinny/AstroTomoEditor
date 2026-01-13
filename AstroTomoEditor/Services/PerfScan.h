#pragma once
// PerfScan.h (или прямо сверху файла)
// Включай только в Debug, чтобы в Release не было вообще ничего.
#ifdef QT_DEBUG

#include <atomic>
#include <array>
#include <algorithm>
#include <QString>
#include <QElapsedTimer>
#include <QDebug>

namespace PerfScan
{


    enum class Id : int
    {
        // PASS 1
        Pass1_Total,
        Pass1_IterateFiles,
        Pass1_ExtSizeChecks,

        // PASS 2 (map)
        Pass2_Total,
        Map_FastDicomHeader,
        Map_ParseMeta_Update,
        Map_HasGeometryPixelChecks,
        Map_MakeSeriesKey,
        Map_ExtractFields,
        Map_TryFillPatientInfo,

        // PASS 3
        Pass3_Total,
        Pass3_SortEntries,
        Pass3_FilterConsistency,
        Pass3_PickMiddle,
        Pass3_MakeThumb,

        // Final sorts
        Final_SortSeriesByName,

        // PlanarView loadSeriesFiles
        PV_Total,
        PV_UiReset,
        PV_BuildNames,
        PV_md_UpdateInformation,
        PV_CheckTransferSyntax,
        PV_Geometry,
        PV_Pix_UpdateInformation,
        PV_Pix_Update,
        PV_Gdcm_UpdateInformation,
        PV_Gdcm_Update,
        PV_Geometry_FromMeta,
        PV_GetDicomRanges,
        PV_FinalizeSpacing_Total,
        PV_FinalizeSpacing_ParseIPP,
        PV_FinalizeSpacing_SortMedian,
        PV_InterpretModality,
        PV_BuildCache,
        PV_Pump_ProcessEvents,   // optional: сколько времени уходит в processEvents

        // buildCache internals (примерно)
        BC_Total,
        BC_PerSlice_Total,
        BC_Slice_Extract,
        BC_Slice_RescaleOrInvert,
        BC_Slice_ToQImage,
        BC_Slice_ToPixmap,
        BC_Slice_PushStore,

        Count
    };

    struct Stat
    {
        std::atomic<long long> nsTotal{ 0 };
        std::atomic<long long> nsMax{ 0 };
        std::atomic<long long> calls{ 0 };

        void add(long long ns)
        {
            nsTotal.fetch_add(ns, std::memory_order_relaxed);
            calls.fetch_add(1, std::memory_order_relaxed);

            long long cur = nsMax.load(std::memory_order_relaxed);
            while (ns > cur && !nsMax.compare_exchange_weak(cur, ns, std::memory_order_relaxed))
            {
                // cur обновится
            }
        }
    };


    inline constexpr int kCount = static_cast<int>(Id::Count);
    inline std::array<Stat, kCount> g;

    inline void addNs(Id id, long long ns)
    {
        g[static_cast<int>(id)].add(ns);
    }

    inline void reset()
    {
        for (auto& s : g)
        {
            s.nsTotal.store(0, std::memory_order_relaxed);
            s.nsMax.store(0, std::memory_order_relaxed);
            s.calls.store(0, std::memory_order_relaxed);
        }
    }

    inline const char* name(Id id)
    {
        switch (id)
        {
        case Id::Pass1_Total: return "PASS1 total";
        case Id::Pass1_IterateFiles: return "PASS1 iterate QDirIterator";
        case Id::Pass1_ExtSizeChecks: return "PASS1 ext/size checks";

        case Id::Pass2_Total: return "PASS2 total (mappedReduced)";
        case Id::Map_FastDicomHeader: return "PASS2 map: Map_FastDicomHeader";
        case Id::Map_ParseMeta_Update: return "PASS2 map: vtkDICOMParser::Update";
        case Id::Map_HasGeometryPixelChecks: return "PASS2 map: hasGeometry/hasPixel checks";
        case Id::Map_MakeSeriesKey: return "PASS2 map: makeSeriesKey";
        case Id::Map_ExtractFields: return "PASS2 map: extract fields";
        case Id::Map_TryFillPatientInfo: return "PASS2 map: tryFillPatientInfo";

        case Id::Pass3_Total: return "PASS3 total";
        case Id::Pass3_SortEntries: return "PASS3 sort entries";
        case Id::Pass3_FilterConsistency: return "PASS3 filterSeriesByConsistency";
        case Id::Pass3_PickMiddle: return "PASS3 pickMiddleSliceFile";
        case Id::Pass3_MakeThumb: return "PASS3 makeThumbImageFromDicom";

        case Id::Final_SortSeriesByName: return "FINAL sortSeriesByName";

        case Id::PV_Total: return "PV total loadSeriesFiles";
        case Id::PV_UiReset: return "PV UI reset";
        case Id::PV_BuildNames: return "PV build vtkStringArray names";
        case Id::PV_md_UpdateInformation:  return "PV mdReader::UpdateInformation";
        case Id::PV_Geometry:              return "PV geometry from metadata";
        case Id::PV_CheckTransferSyntax: return "PV check transfer syntax";
        case Id::PV_Pix_UpdateInformation: return "PV pixReader::UpdateInformation";
        case Id::PV_Pix_Update: return "PV pixReader::Update";
        case Id::PV_Gdcm_UpdateInformation: return "PV gdcm::UpdateInformation";
        case Id::PV_Gdcm_Update: return "PV gdcm::Update";
        case Id::PV_Geometry_FromMeta: return "PV geometry from metadata";
        case Id::PV_GetDicomRanges: return "PV GetDicomRangesVTK";
        case Id::PV_FinalizeSpacing_Total: return "PV finalizeSpacing total";
        case Id::PV_FinalizeSpacing_ParseIPP: return "PV finalizeSpacing parse IPP";
        case Id::PV_FinalizeSpacing_SortMedian: return "PV finalizeSpacing sort+median";
        case Id::PV_InterpretModality: return "PV interpret modality";
        case Id::PV_BuildCache: return "PV buildCache";
        case Id::PV_Pump_ProcessEvents: return "PV pump processEvents";

        case Id::BC_Total: return "BC total";
        case Id::BC_PerSlice_Total: return "BC per-slice total";
        case Id::BC_Slice_Extract: return "BC slice extract";
        case Id::BC_Slice_RescaleOrInvert: return "BC slice rescale/invert";
        case Id::BC_Slice_ToQImage: return "BC slice to QImage";
        case Id::BC_Slice_ToPixmap: return "BC slice to QPixmap";
        case Id::BC_Slice_PushStore: return "BC slice store/push";
        default: return "unknown";
        }
    }

    class Scope
    {
    public:
        explicit Scope(Id id) : mId(id) { mT.start(); }
        ~Scope()
        {
            const long long ns = mT.nsecsElapsed();
            g[static_cast<int>(mId)].add(ns);
        }
    private:
        Id mId;
        QElapsedTimer mT;
    };

    inline double nsToMs(long long ns) { return double(ns) / 1'000'000.0; }

    inline void dumpTop(int topN = 20)
    {
        struct Row { Id id; long long total; long long calls; long long mx; };
        std::array<Row, kCount> rows{};
        for (int i = 0; i < kCount; ++i)
        {
            rows[i] = {
                static_cast<Id>(i),
                g[i].nsTotal.load(std::memory_order_relaxed),
                g[i].calls.load(std::memory_order_relaxed),
                g[i].nsMax.load(std::memory_order_relaxed)
            };
        }

        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.total > b.total;
            });

        qDebug().noquote() << "\n=== Series scan perf (top) ===";
        int printed = 0;
        for (const auto& r : rows)
        {
            if (r.calls == 0) continue;
            const double totalMs = nsToMs(r.total);
            const double avgMs = nsToMs(r.total) / double(r.calls);
            const double maxMs = nsToMs(r.mx);

            qDebug().noquote()
                << QString("%1 | total %2 ms | calls %3 | avg %4 ms | max %5 ms")
                .arg(QString::fromLatin1(name(r.id)).leftJustified(36))
                .arg(totalMs, 10, 'f', 2)
                .arg(r.calls, 8)
                .arg(avgMs, 9, 'f', 3)
                .arg(maxMs, 9, 'f', 3);

            if (++printed >= topN) break;
        }
        qDebug().noquote() << "=== end ===\n";
    }
}

#define PERF_SCOPE(id) PerfScan::Scope _perf_scope_##__LINE__(id)

#else

#define PERF_SCOPE(id) do{}while(0)

#endif

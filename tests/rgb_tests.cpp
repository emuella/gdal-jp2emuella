#include "rgb_support.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
void Layout(GDALDataset *dataset, const std::vector<int> &bands,
            GSpacing pixel, GSpacing line, GSpacing band) {
    constexpr int width = 13;
    constexpr int height = 9;
    constexpr size_t guard = 17;
    const auto count = static_cast<int>(bands.size());
    const auto extent = static_cast<size_t>((width - 1) * pixel +
        (height - 1) * line + (count - 1) * band + 1);
    std::vector<std::uint8_t> pixels(extent + guard * 2, 0xa5);
    std::vector<bool> touched(pixels.size(), false);
    auto map = bands;
    rgb::Check(dataset->RasterIO(GF_Read, 7, 11, width, height,
        pixels.data() + guard, width, height, GDT_Byte, count, map.data(),
        pixel, line, band, nullptr) == CE_None, "combined layout read failed");
    for (int b = 0; b < count; ++b)
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const auto index = guard + static_cast<size_t>(b * band + y * line + x * pixel);
                rgb::Check(pixels[index] == rgb::Expected(bands[static_cast<size_t>(b)], 7 + x, 11 + y),
                           "layout pixels or ordering differ");
                touched[index] = true;
            }
    for (size_t index = 0; index < pixels.size(); ++index)
        rgb::Check(touched[index] || pixels[index] == 0xa5, "layout overwrote a guard or padding");
}
void CombinedAndReuse() {
    auto dataset = rgb::Open(true);
    rgb::Check(dataset->GetMetadata(nullptr) != nullptr &&
        dataset->GetMetadataItem("CODEC", nullptr) != nullptr,
        "null metadata domain did not preserve default-domain semantics");
    auto before = rgb::Snapshot(dataset.get());
    Layout(dataset.get(), {1, 2, 3}, 1, 17, 17 * 9 + 7);
    auto first = rgb::Snapshot(dataset.get());
    rgb::Check(rgb::Value(first, "DECODE_COUNT") == 1 &&
        rgb::Value(first, "preparation_count") == 1 &&
        rgb::Value(first, "WORKSPACE_CREATIONS") == 1, "combined decode was split");
    Layout(dataset.get(), {3, 1, 2}, 3, 13 * 3 + 11, 1);
    Layout(dataset.get(), {3, 1, 3, 2, 1}, 12, 13 * 12 + 3, 2);
    Layout(dataset.get(), {2, 2, 1}, 2, 13 * 2 + 5, (13 * 2 + 5) * 9 + 7);
    auto after = rgb::Snapshot(dataset.get());
    rgb::Check(rgb::Value(after, "DECODE_COUNT") == 4 &&
        rgb::Value(after, "WORKSPACE_CREATIONS") == 1,
        "repeated layouts did not reuse one workspace");
    rgb::Check(rgb::Value(after, "SOURCE_BYTES_REQUESTED") >
        rgb::Value(before, "SOURCE_BYTES_REQUESTED"), "source bytes were not observed");

    auto separate = rgb::Open(true);
    std::vector<std::uint8_t> pixels(13 * 9);
    for (int band = 1; band <= 3; ++band)
        rgb::Check(separate->GetRasterBand(band)->RasterIO(GF_Read, 7, 11, 13, 9,
            pixels.data(), 13, 9, GDT_Byte, 1, 13, nullptr) == CE_None,
            "separate band read failed");
    const auto individual = rgb::Snapshot(separate.get());
    for (const auto *key : {"preparation_count", "code_blocks_decoded",
                            "tier1_coefficients", "synthesis_output_samples"})
        rgb::Check(rgb::Value(individual, key) == 3 * rgb::Value(first, key),
                   std::string("combined MCT work was not shared: ") + key);
    rgb::Check(rgb::Value(individual, "WORKSPACE_CREATIONS") == 1,
               "band reads did not reuse their workspace");
    std::vector<std::uint8_t> full(rgb::Width * rgb::Height * 3);
    int map[] = {1, 2, 3};
    rgb::Check(dataset->RasterIO(GF_Read, 0, 0, rgb::Width, rgb::Height, full.data(),
        rgb::Width, rgb::Height, GDT_Byte, 3, map, 1, rgb::Width,
        rgb::Width * rgb::Height, nullptr) == CE_None, "full combined RGB read failed");
    rgb::ValidatePlanar(full, 0, 0, rgb::Width, rgb::Height, 3);
    std::vector<std::uint8_t> defaults(5 * 4 * 3);
    rgb::Check(dataset->RasterIO(GF_Read, 251, 188, 5, 4, defaults.data(),
        5, 4, GDT_Byte, 3, nullptr, 0, 0, 0, nullptr) == CE_None,
        "default band map and strides at image edge failed");
    rgb::ValidatePlanar(defaults, 251, 188, 5, 4, 3);
    auto disabled = rgb::Open();
    rgb::Check(rgb::Snapshot(disabled.get()).empty(), "diagnostics enabled by default");
}
void Generic() {
    auto dataset = rgb::Open(true);
    int map[] = {3, 1, 2};
    // Overlapping positive planes must retain the generic band's write order.
    std::vector<std::uint8_t> overlap(13 * 9 + 2), reference(overlap.size());
    rgb::Check(dataset->RasterIO(GF_Read, 7, 11, 13, 9, overlap.data(),
        13, 9, GDT_Byte, 3, map, 1, 13, 1, nullptr) == CE_None,
        "overlapping-plane fallback failed");
    for (int band = 0; band < 3; ++band)
        rgb::Check(dataset->GetRasterBand(map[band])->RasterIO(GF_Read,
            7, 11, 13, 9, reference.data() + band, 13, 9, GDT_Byte, 1, 13,
            nullptr) == CE_None, "overlapping-plane reference failed");
    rgb::Check(overlap == reference, "overlapping planes changed generic write order");
    // Negative line spacing must retain GDAL's generic semantics.
    std::vector<std::uint8_t> reversed(13 * 9 * 3);
    rgb::Check(dataset->RasterIO(GF_Read, 7, 11, 13, 9, reversed.data() + 13 * 8,
        13, 9, GDT_Byte, 3, map, 1, -13, 13 * 9, nullptr) == CE_None,
        "negative-stride fallback failed");
    for (int band = 0; band < 3; ++band)
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 13; ++x)
                rgb::Check(reversed[static_cast<size_t>(band * 13 * 9 + (8 - y) * 13 + x)] ==
                    rgb::Expected(map[band], 7 + x, 11 + y), "negative-stride pixel mismatch");
    std::vector<std::uint16_t> wide(13 * 9 * 3);
    rgb::Check(dataset->RasterIO(GF_Read, 7, 11, 13, 9, wide.data(), 13, 9,
        GDT_UInt16, 3, map, 2, 26, 13 * 9 * 2, nullptr) == CE_None,
        "UInt16 fallback failed");
    for (int band = 0; band < 3; ++band)
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 13; ++x)
                rgb::Check(wide[static_cast<size_t>((band * 9 + y) * 13 + x)] ==
                    rgb::Expected(map[band], 7 + x, 11 + y), "UInt16 pixel mismatch");
    for (const auto algorithm : {GRIORA_NearestNeighbour, GRIORA_Bilinear}) {
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.eResampleAlg = algorithm;
        extra.bFloatingPointWindowValidity = TRUE;
        extra.dfXOff = 7.75;
        extra.dfYOff = 11.25;
        extra.dfXSize = 13;
        extra.dfYSize = 9;
        std::vector<std::uint8_t> actual(7 * 5 * 3), expected(actual.size());
        auto copy = extra;
        rgb::Check(dataset->RasterIO(GF_Read, 7, 11, 13, 9, actual.data(), 7, 5,
            GDT_Byte, 3, map, 1, 7, 7 * 5, &extra) == CE_None, "fractional dataset read failed");
        for (int band = 0; band < 3; ++band)
            rgb::Check(dataset->GetRasterBand(map[band])->RasterIO(GF_Read,
                7, 11, 13, 9, expected.data() + band * 7 * 5, 7, 5,
                GDT_Byte, 1, 7, &copy) == CE_None, "fractional reference read failed");
        rgb::Check(actual == expected, "fractional/resampled dataset pixels differ");
    }
    for (const bool cancel : {false, true}) {
        struct State { int calls; bool cancel; } state{0, cancel};
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.pfnProgress = [](double, const char *, void *data) {
            auto &progress = *static_cast<State *>(data);
            ++progress.calls;
            return progress.cancel ? FALSE : TRUE;
        };
        extra.pProgressData = &state;
        std::vector<std::uint8_t> pixels(13 * 9 * 3);
        CPLPushErrorHandler(CPLQuietErrorHandler);
        const auto result = dataset->RasterIO(GF_Read, 7, 11, 13, 9, pixels.data(),
            13, 9, GDT_Byte, 3, map, 1, 13, 13 * 9, &extra);
        CPLPopErrorHandler();
        rgb::Check(state.calls > 0 && result == (cancel ? CE_Failure : CE_None),
                   "dataset progress/cancellation bypassed");
    }
    rgb::Check(rgb::Value(rgb::Snapshot(dataset.get()), "WORKSPACE_CREATIONS") == 1,
               "generic block path did not reuse workspace");
}
void Concurrent() {
    auto dataset = rgb::Open(true);
    std::atomic<bool> success{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            try {
                for (int iteration = 0; iteration < 4; ++iteration)
                    Layout(dataset.get(), {3, 1, 3, 2}, 4 + t, 13 * (4 + t) + 7, 1);
            } catch (...) { success = false; }
        });
    for (auto &thread : threads)
        thread.join();
    rgb::Check(success, "concurrent combined pixels differ");
    const auto metrics = rgb::Snapshot(dataset.get());
    rgb::Check(rgb::Value(metrics, "WORKSPACE_CREATIONS") == 1 &&
               rgb::Value(metrics, "DECODE_COUNT") == 16, "concurrent workspace accounting differs");
}
} // namespace
int main() {
    try {
        GDALAllRegister();
        CombinedAndReuse();
        Generic();
        Concurrent();
        GDALDestroyDriverManager();
        std::cout << "JP2Emuella RGB tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        GDALDestroyDriverManager();
        return 1;
    }
}

#include "rgb_support.h"

#include <chrono>
#include <iostream>

namespace {
using Clock = std::chrono::steady_clock;
std::int64_t Nanos(Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}
void JsonMetrics(const rgb::Metrics &metrics) {
    std::cout << '{';
    bool first = true;
    for (const auto &entry : metrics) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << '"' << entry.first << "\":" << entry.second;
    }
    std::cout << '}';
}
void Read(GDALDataset *dataset, const std::string &mode, int x, int width,
          int height, std::vector<std::uint8_t> &pixels) {
    if (mode == "combined_rgb") {
        int bands[] = {1, 2, 3};
        rgb::Check(dataset->RasterIO(GF_Read, x, 0, width, height, pixels.data(),
            width, height, GDT_Byte, 3, bands, 1, width, width * height,
            nullptr) == CE_None, "benchmark combined read failed");
    } else {
        const int bands = mode == "one_band" ? 1 : 3;
        for (int band = 0; band < bands; ++band)
            rgb::Check(dataset->GetRasterBand(band + 1)->RasterIO(GF_Read,
                x, 0, width, height, pixels.data() + band * width * height,
                width, height, GDT_Byte, 1, width, nullptr) == CE_None,
                "benchmark band read failed");
    }
}
void Cell(const std::string &mode, const std::string &scenario, int iterations,
          bool diagnosticsAvailable) {
    const bool full = scenario == "full_image";
    const int width = full ? rgb::Width : 64;
    const int height = full ? rgb::Height : 64;
    const int bands = mode == "one_band" ? 1 : 3;
    std::vector<std::vector<std::uint8_t>> outputs(static_cast<size_t>(iterations),
        std::vector<std::uint8_t>(static_cast<size_t>(width * height * bands)));
    std::vector<std::int64_t> timings(static_cast<size_t>(iterations));
    const auto openStart = Clock::now();
    auto dataset = rgb::Open();
    const auto openNs = Nanos(Clock::now() - openStart);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int x = scenario == "adjacent_windows" ? (iteration % 4) * 64 : 0;
        const auto start = Clock::now();
        Read(dataset.get(), mode, x, width, height, outputs[static_cast<size_t>(iteration)]);
        timings[static_cast<size_t>(iteration)] = Nanos(Clock::now() - start);
    }
    // Each output is retained so all validation remains outside timed reads.
    for (int iteration = 0; iteration < iterations; ++iteration)
        rgb::ValidatePlanar(outputs[static_cast<size_t>(iteration)],
            scenario == "adjacent_windows" ? (iteration % 4) * 64 : 0,
            0, width, height, bands);
    dataset.reset();
    std::cout << "{\"schema_version\":1,\"mode\":\"" << mode
              << "\",\"scenario\":\"" << scenario
              << "\",\"iterations\":" << iterations << ",\"width\":" << width
              << ",\"height\":" << height << ",\"open_ns\":" << openNs
              << ",\"read_ns\":[";
    for (int iteration = 0; iteration < iterations; ++iteration) {
        if (iteration != 0) std::cout << ',';
        std::cout << timings[static_cast<size_t>(iteration)];
    }
    std::cout << "],\"metrics_available\":" << (diagnosticsAvailable ? "true" : "false");
    if (diagnosticsAvailable) {
        auto observed = rgb::Open(true);
        const auto initial = rgb::Snapshot(observed.get());
        rgb::Metrics first;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            const int x = scenario == "adjacent_windows" ? (iteration % 4) * 64 : 0;
            Read(observed.get(), mode, x, width, height, outputs[static_cast<size_t>(iteration)]);
            if (iteration == 0) first = rgb::Snapshot(observed.get());
        }
        const auto final = rgb::Snapshot(observed.get());
        for (int iteration = 0; iteration < iterations; ++iteration)
            rgb::ValidatePlanar(outputs[static_cast<size_t>(iteration)],
                scenario == "adjacent_windows" ? (iteration % 4) * 64 : 0,
                0, width, height, bands);
        std::cout << ",\"metrics_after_open\":";
        JsonMetrics(initial);
        std::cout << ",\"metrics_after_first_read\":";
        JsonMetrics(first);
        std::cout << ",\"metrics_after_all_reads\":";
        JsonMetrics(final);
    }
    std::cout << "}\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        rgb::Check(argc <= 2, "usage: jp2emuella_rgb_benchmark [iterations: 1..1000]");
        const int iterations = argc == 2 ? std::stoi(argv[1]) : 9;
        rgb::Check(iterations >= 1 && iterations <= 1000, "iterations must be 1..1000");
        GDALAllRegister();
        auto *driver = GetGDALDriverManager()->GetDriverByName("JP2Emuella");
        rgb::Check(driver != nullptr, "JP2Emuella plugin did not autoload");
        const char *options = driver->GetMetadataItem(GDAL_DMD_OPENOPTIONLIST);
        const bool diagnostics = options != nullptr &&
            std::string(options).find("DIAGNOSTICS") != std::string::npos;
        for (const auto *scenario : {"same_window", "adjacent_windows", "full_image"})
            for (const auto *mode : {"one_band", "separate_rgb", "combined_rgb"})
                Cell(mode, scenario, iterations, diagnostics);
        GDALDestroyDriverManager();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        GDALDestroyDriverManager();
        return 1;
    }
}

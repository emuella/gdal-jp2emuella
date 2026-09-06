#pragma once

#include "cpl_string.h"
#include "gdal_priv.h"

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rgb {
inline constexpr int Width = 256;
inline constexpr int Height = 192;
inline constexpr const char *Domain = "EMUELLA_DIAGNOSTICS";

inline void Check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Closer {
    void operator()(GDALDataset *dataset) const {
        if (dataset != nullptr)
            GDALClose(dataset);
    }
};
using Dataset = std::unique_ptr<GDALDataset, Closer>;
using Metrics = std::map<std::string, std::uint64_t>;

inline std::uint8_t Expected(int band, int x, int y) {
    switch (band) {
    case 1: return static_cast<std::uint8_t>((x * 13 + y * 7 + x * y * 3 + 17) & 255);
    case 2: return static_cast<std::uint8_t>((x * 5 + y * 19 + (x ^ y) * 11 + 29) & 255);
    case 3: return static_cast<std::uint8_t>((x * 23 + y * 3 + x * y * 5 + 41) & 255);
    default: throw std::runtime_error("invalid RGB oracle band");
    }
}
inline Dataset Open(bool diagnostics = false) {
    const auto path = std::string(JP2EMUELLA_FIXTURE_DIR) + "/rgb-mct-256x192.j2k";
    const char *allowed[] = {"JP2Emuella", nullptr};
    const char *options[] = {"DIAGNOSTICS=YES", nullptr};
    Dataset result(static_cast<GDALDataset *>(GDALOpenEx(path.c_str(),
        GDAL_OF_RASTER | GDAL_OF_READONLY, allowed,
        diagnostics ? options : nullptr, nullptr)));
    Check(result != nullptr, "RGB fixture did not open");
    Check(result->GetRasterXSize() == Width && result->GetRasterYSize() == Height &&
          result->GetRasterCount() == 3, "RGB fixture geometry differs");
    return result;
}
inline Metrics Snapshot(GDALDataset *dataset) {
    Metrics result;
    CSLConstList items = dataset->GetMetadata(Domain);
    for (int i = 0; items != nullptr && items[i] != nullptr; ++i) {
        const std::string item(items[i]);
        const auto separator = item.find('=');
        Check(separator != std::string::npos, "malformed diagnostics metadata");
        result.emplace(item.substr(0, separator), std::stoull(item.substr(separator + 1)));
    }
    return result;
}
inline std::uint64_t Value(const Metrics &metrics, const std::string &key) {
    const auto found = metrics.find(key);
    Check(found != metrics.end(), "missing diagnostic " + key);
    return found->second;
}
inline void ValidatePlanar(const std::vector<std::uint8_t> &pixels,
                           int x, int y, int width, int height, int bands) {
    for (int band = 0; band < bands; ++band)
        for (int row = 0; row < height; ++row)
            for (int column = 0; column < width; ++column)
                Check(pixels[static_cast<size_t>((band * height + row) * width + column)] ==
                      Expected(band + 1, x + column, y + row), "RGB pixel mismatch");
}
} // namespace rgb

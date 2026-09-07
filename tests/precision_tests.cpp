#include "cpl_string.h"
#include "gdal_priv.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
std::uint16_t Expected(int x, int y, int bits, int component = 0) {
    const int mask = (1 << bits) - 1;
    if (x == 0 && y == 0)
        return 0;
    if (x == 66 && y == 52)
        return static_cast<std::uint16_t>(mask);
    return static_cast<std::uint16_t>(
        (x * 997 + y * 617 + x * y * 13 + component * 1237) & mask);
}
std::uint64_t Metric(GDALDataset *ds, const char *name) {
    const char *value = ds->GetMetadataItem(name, "EMUELLA_DIAGNOSTICS");
    Check(value != nullptr, "missing precision metric");
    return std::stoull(value);
}
void Window(GDALDataset *ds, int x, int y, int w, int h, int copies, int pixel,
            int line, int band, int bits, std::vector<int> map = {}) {
    const auto extent =
        static_cast<size_t>((h - 1) * line + (w - 1) * pixel + (copies - 1) * band + 2);
    std::vector<std::uint8_t> output(extent + 16, 0xa5);
    std::vector<bool> touched(output.size(), false);
    if (map.empty())
        map.assign(static_cast<size_t>(copies), 1);
    Check(map.size() == static_cast<size_t>(copies), "test band map mismatch");
    const auto before = Metric(ds, "DECODE_COUNT");
    Check(ds->RasterIO(GF_Read, x, y, w, h, output.data() + 7, w, h, GDT_UInt16, copies,
                       map.data(), pixel, line, band, nullptr) == CE_None,
          "precision window read failed");
    Check(Metric(ds, "DECODE_COUNT") == before + 1,
          "combined precision read was not deduplicated");
    for (int b = 0; b < copies; ++b)
        for (int row = 0; row < h; ++row)
            for (int col = 0; col < w; ++col) {
                const auto offset =
                    static_cast<size_t>(7 + b * band + row * line + col * pixel);
                std::uint16_t value;
                std::memcpy(&value, output.data() + offset, 2);
                Check(value == Expected(x + col, y + row, bits,
                                        map[static_cast<size_t>(b)] - 1),
                      "precision sample mismatch");
                touched[offset] = touched[offset + 1] = true;
            }
    for (size_t i = 0; i < output.size(); ++i)
        if (!touched[i])
            Check(output[i] == 0xa5, "precision padding overwritten");
}
void TestGrayscale(int bits) {
    const char *allowed[] = {"JP2Emuella", nullptr};
    const char *options[] = {"DIAGNOSTICS=YES", nullptr};
    const std::string path = std::string(JP2EMUELLA_FIXTURE_DIR) + "/precision-" +
                             std::to_string(bits) + "-1.j2k";
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> ds(
        static_cast<GDALDataset *>(
            GDALOpenEx(path.c_str(), GDAL_OF_RASTER, allowed, options, nullptr)),
        GDALClose);
    Check(ds != nullptr, "high-precision grayscale did not open");
    Check(ds->GetRasterCount() == 1 && ds->GetRasterXSize() == 67 &&
              ds->GetRasterYSize() == 53,
          "precision geometry mismatch");
    auto *band = ds->GetRasterBand(1);
    Check(band->GetRasterDataType() == GDT_UInt16, "precision data type mismatch");
    Check(std::string(band->GetMetadataItem("NBITS", "IMAGE_STRUCTURE")) ==
              std::to_string(bits),
          "precision NBITS mismatch");
    const auto openReads = Metric(ds.get(), "SOURCE_READ_REQUESTS");
    const auto openBytes = Metric(ds.get(), "SOURCE_BYTES_REQUESTED");
    Check(openReads > 0 && openBytes >= openReads,
          "source inspection counters missing");
    Window(ds.get(), 29, 27, 9, 11, 1, 2, 18, 2, bits);
    const auto regionWork = Metric(ds.get(), "tier1_coefficients");
    Check(Metric(ds.get(), "PEAK_output_capacity_bytes") == 9 * 11 * 2,
          "regional output allocated more than requested plane");
    Check(Metric(ds.get(), "PEAK_full_coefficient_plane_capacity") <= 32 * 32,
          "workspace exceeded fixture tile size");
    Window(ds.get(), 29, 27, 9, 11, 3, 8, 80, 2, bits);
    Window(ds.get(), 2, 3, 5, 7, 2, 4, 26, 190, bits);
    Window(ds.get(), 66, 52, 1, 1, 1, 2, 2, 2, bits);
    const auto beforeFull = Metric(ds.get(), "tier1_coefficients");
    Window(ds.get(), 0, 0, 67, 53, 1, 2, 134, 2, bits);
    Check(Metric(ds.get(), "tier1_coefficients") - beforeFull > regionWork,
          "region did not reduce coefficient work");
    Check(Metric(ds.get(), "WORKSPACE_CREATIONS") == 1, "workspace was not reused");
    Check(Metric(ds.get(), "SOURCE_READ_REQUESTS") > openReads &&
              Metric(ds.get(), "SOURCE_BYTES_REQUESTED") > openBytes,
          "regional I/O not counted");
    std::array<float, 20> floats{};
    Check(band->RasterIO(GF_Read, 31, 30, 5, 4, floats.data(), 5, 4, GDT_Float32, 4, 20,
                         nullptr) == CE_None,
          "Float32 fallback failed");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            Check(floats[static_cast<size_t>(y * 5 + x)] ==
                      Expected(x + 31, y + 30, bits),
                  "Float32 precision lost");
    std::array<std::uint16_t, 20> reverse{};
    Check(band->RasterIO(GF_Read, 31, 30, 5, 4, reverse.data() + 15, 5, 4, GDT_UInt16,
                         2, -10, nullptr) == CE_None,
          "negative stride fallback failed");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            Check(reverse[static_cast<size_t>((3 - y) * 5 + x)] ==
                      Expected(x + 31, y + 30, bits),
                  "negative stride mismatch");
    std::vector<std::uint16_t> block(67 * 53);
    Check(band->ReadBlock(0, 0, block.data()) == CE_None, "UInt16 block read failed");
    for (int y = 0; y < 53; ++y)
        for (int x = 0; x < 67; ++x)
            Check(block[static_cast<size_t>(y * 67 + x)] == Expected(x, y, bits),
                  "UInt16 block mismatch");
    std::cout << "precision grayscale bits=" << bits
              << " passed; source requests=" << Metric(ds.get(), "SOURCE_READ_REQUESTS")
              << " bytes=" << Metric(ds.get(), "SOURCE_BYTES_REQUESTED") << '\n';
}
void TestRGB16() {
    const char *allowed[] = {"JP2Emuella", nullptr};
    const char *options[] = {"DIAGNOSTICS=YES", nullptr};
    const std::string path =
        std::string(JP2EMUELLA_FIXTURE_DIR) + "/precision-16-3.j2k";
    std::unique_ptr<GDALDataset, decltype(&GDALClose)> ds(
        static_cast<GDALDataset *>(
            GDALOpenEx(path.c_str(), GDAL_OF_RASTER, allowed, options, nullptr)),
        GDALClose);
    Check(ds != nullptr && ds->GetRasterCount() == 3, "RGB16 did not open");
    for (int band = 1; band <= 3; ++band) {
        Check(ds->GetRasterBand(band)->GetRasterDataType() == GDT_UInt16,
              "RGB16 type mismatch");
        Check(std::string(ds->GetRasterBand(band)->GetMetadataItem(
                  "NBITS", "IMAGE_STRUCTURE")) == "16",
              "RGB16 NBITS mismatch");
    }
    Window(ds.get(), 29, 27, 9, 11, 4, 10, 94, 2, 16, {3, 1, 2, 3});
    Check(Metric(ds.get(), "PEAK_output_capacity_bytes") == 9 * 11 * 2 * 3,
          "RGB16 region allocation exceeds distinct output planes");
    Window(ds.get(), 0, 0, 67, 53, 3, 2, 134, 7102, 16, {1, 2, 3});
    Window(ds.get(), 66, 52, 1, 1, 3, 6, 6, 2, 16, {3, 2, 1});
    std::uint16_t value = 0;
    Check(ds->GetRasterBand(2)->RasterIO(GF_Read, 31, 30, 1, 1, &value, 1, 1,
                                         GDT_UInt16, 2, 2, nullptr) == CE_None,
          "RGB16 single-band read failed");
    Check(value == Expected(31, 30, 16, 1), "RGB16 single-band MCT value mismatch");
    Check(Metric(ds.get(), "WORKSPACE_CREATIONS") == 1, "RGB16 workspace not reused");
}
} // namespace
int main() {
    try {
        GDALAllRegister();
        for (int bits = 9; bits <= 16; ++bits)
            TestGrayscale(bits);
        TestRGB16();
        GDALDestroyDriverManager();
        std::cout << "precision grayscale9–16 and RGB16 tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

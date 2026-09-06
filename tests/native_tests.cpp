#include "cpl_conv.h"
#include "cpl_vsi.h"
#include "gdal_priv.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct DatasetCloser {
    void operator()(GDALDataset *dataset) const noexcept {
        if (dataset != nullptr)
            GDALClose(dataset);
    }
};
using DatasetPtr = std::unique_ptr<GDALDataset, DatasetCloser>;

void Check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<std::uint8_t> ReadFixture() {
    const std::string path =
        std::string(JP2EMUELLA_FIXTURE_DIR) + "/gray-gradient-17x19.j2k";
    std::ifstream stream(path, std::ios::binary);
    Check(stream.good(), "could not open project-authored fixture");
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

std::uint8_t Expected(int x, int y) {
    return static_cast<std::uint8_t>((x * 13 + y * 29 + x * y * 3) & 0xff);
}

DatasetPtr Open(const char *path) {
    const char *allowed[] = {"JP2Emuella", nullptr};
    return DatasetPtr(static_cast<GDALDataset *>(GDALOpenEx(
        path, GDAL_OF_RASTER | GDAL_OF_READONLY, allowed, nullptr, nullptr)));
}

void PutVsiMem(const char *path, const std::vector<std::uint8_t> &bytes) {
    auto *copy = static_cast<GByte *>(CPLMalloc(bytes.size()));
    Check(copy != nullptr, "could not allocate /vsimem buffer");
    std::memcpy(copy, bytes.data(), bytes.size());
    VSILFILE *file = VSIFileFromMemBuffer(path, copy, bytes.size(), TRUE);
    if (file == nullptr) {
        CPLFree(copy);
        throw std::runtime_error("could not create /vsimem fixture");
    }
    Check(VSIFCloseL(file) == 0, "could not close /vsimem fixture handle");
}

void TestMetadataAndPixels(const std::vector<std::uint8_t> &fixture) {
    PutVsiMem("/vsimem/jp2emuella-valid.j2k", fixture);
    auto dataset = Open("/vsimem/jp2emuella-valid.j2k");
    Check(dataset != nullptr, "valid raw codestream did not open");
    Check(std::string(dataset->GetDriverName()) == "JP2Emuella",
          "unexpected driver");
    Check(dataset->GetRasterXSize() == 17 && dataset->GetRasterYSize() == 19,
          "unexpected raster dimensions");
    Check(dataset->GetRasterCount() == 1, "unexpected component count");
    Check(dataset->GetRasterBand(1)->GetRasterDataType() == GDT_Byte,
          "component was not exposed as Byte");
    Check(std::string(dataset->GetMetadataItem("CODESTREAM_KIND")) ==
              "JPEG 2000 Part 1 raw codestream",
          "codestream metadata missing");

    int blockWidth = 0;
    int blockHeight = 0;
    dataset->GetRasterBand(1)->GetBlockSize(&blockWidth, &blockHeight);
    Check(blockWidth == 17 && blockHeight == 19, "unexpected block geometry");
    std::vector<std::uint8_t> block(static_cast<size_t>(blockWidth) *
                                    static_cast<size_t>(blockHeight));
    Check(dataset->GetRasterBand(1)->ReadBlock(0, 0, block.data()) == CE_None,
          "GDAL block read failed");
    Check(block.front() == Expected(0, 0) && block.back() == Expected(16, 18),
          "GDAL block pixels mismatch");

    std::vector<std::uint8_t> pixels(17 * 19);
    Check(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 17, 19,
                                              pixels.data(), 17, 19, GDT_Byte,
                                              1, 17, nullptr) == CE_None,
          "full raster read failed");
    for (int y = 0; y < 19; ++y)
        for (int x = 0; x < 17; ++x)
            Check(pixels[static_cast<size_t>(y * 17 + x)] == Expected(x, y),
                  "full raster pixel mismatch");

    std::vector<std::uint8_t> window(5 * 4);
    Check(dataset->GetRasterBand(1)->RasterIO(GF_Read, 3, 7, 5, 4,
                                              window.data(), 5, 4, GDT_Byte, 1,
                                              5, nullptr) == CE_None,
          "window read failed");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            Check(window[static_cast<size_t>(y * 5 + x)] ==
                      Expected(x + 3, y + 7),
                  "window pixel mismatch");

    constexpr size_t paddedStride = 8;
    constexpr size_t paddedRows = 4;
    constexpr size_t logicalExtent = (paddedRows - 1) * paddedStride + 5;
    std::vector<std::uint8_t> padded(paddedStride * paddedRows, 0xa5);
    Check(dataset->GetRasterBand(1)->RasterIO(
              GF_Read, 3, 7, 5, 4, padded.data(), 5, 4, GDT_Byte, 1,
              static_cast<GSpacing>(paddedStride), nullptr) == CE_None,
          "padded window read failed");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            Check(padded[static_cast<size_t>(y) * paddedStride +
                         static_cast<size_t>(x)] == Expected(x + 3, y + 7),
                  "padded window pixel mismatch");
    for (size_t index = logicalExtent; index < padded.size(); ++index)
        Check(padded[index] == 0xa5,
              "padded window wrote beyond the logical destination extent");

    std::uint8_t edge = 0;
    Check(dataset->GetRasterBand(1)->RasterIO(GF_Read, 16, 18, 1, 1, &edge, 1,
                                              1, GDT_Byte, 1, 1,
                                              nullptr) == CE_None &&
              edge == Expected(16, 18),
          "edge read failed");
    dataset.reset();
    VSIUnlink("/vsimem/jp2emuella-valid.j2k");
}

void TestExtraArgs(const std::vector<std::uint8_t> &fixture) {
    PutVsiMem("/vsimem/jp2emuella-extra-args.j2k", fixture);
    auto dataset = Open("/vsimem/jp2emuella-extra-args.j2k");
    Check(dataset != nullptr, "extra-argument fixture did not open");
    auto *band = dataset->GetRasterBand(1);
    for (const auto algorithm : {GRIORA_NearestNeighbour, GRIORA_Bilinear}) {
        for (const int bufferWidth : {5, 3}) {
            GDALRasterIOExtraArg extra;
            INIT_RASTERIO_EXTRA_ARG(extra);
            extra.eResampleAlg = algorithm;
            extra.bFloatingPointWindowValidity = TRUE;
            extra.dfXOff = 3.75;
            extra.dfYOff = 7.75;
            extra.dfXSize = 5.0;
            extra.dfYSize = 4.0;
            const auto count = static_cast<size_t>(bufferWidth * 4);
            std::vector<std::uint8_t> actual(count);
            std::vector<std::uint8_t> generic(count * 2, 0xa5);
            // A two-byte pixel stride forces the generic GDAL block path.
            auto referenceExtra = extra;
            Check(band->RasterIO(GF_Read, 3, 7, 5, 4, generic.data(),
                                 bufferWidth, 4, GDT_Byte, 2, bufferWidth * 2,
                                 &referenceExtra) == CE_None,
                  "generic fractional-window read failed");
            Check(band->RasterIO(GF_Read, 3, 7, 5, 4, actual.data(),
                                 bufferWidth, 4, GDT_Byte, 1, bufferWidth,
                                 &extra) == CE_None,
                  "fractional-window read failed");
            bool differsFromInteger = false;
            for (size_t index = 0; index < count; ++index) {
                Check(actual[index] == generic[index * 2],
                      "fractional window differs from generic GDAL path");
                Check(generic[index * 2 + 1] == 0xa5,
                      "generic read overwrote pixel padding");
                if (bufferWidth == 5 &&
                    generic[index * 2] !=
                        Expected(3 + static_cast<int>(index % 5),
                                 7 + static_cast<int>(index / 5)))
                    differsFromInteger = true;
            }
            Check(bufferWidth != 5 || differsFromInteger,
                  "fractional probe did not distinguish the integer window");
        }
    }

    struct Progress {
        int calls = 0;
        double last = 0;
        bool cancel = false;
    };
    for (const bool cancel : {false, true}) {
        Progress progress;
        progress.cancel = cancel;
        GDALRasterIOExtraArg extra;
        INIT_RASTERIO_EXTRA_ARG(extra);
        extra.pfnProgress = [](double complete, const char *, void *data) {
            auto &state = *static_cast<Progress *>(data);
            ++state.calls;
            state.last = complete;
            return state.cancel ? FALSE : TRUE;
        };
        extra.pProgressData = &progress;
        std::vector<std::uint8_t> pixels(20);
        CPLPushErrorHandler(CPLQuietErrorHandler);
        const auto result = band->RasterIO(GF_Read, 3, 7, 5, 4, pixels.data(),
                                            5, 4, GDT_Byte, 1, 5, &extra);
        CPLPopErrorHandler();
        Check(progress.calls > 0, "progress callback was bypassed");
        Check(result == (cancel ? CE_Failure : CE_None),
              "progress cancellation result mismatch");
        if (!cancel) {
            Check(progress.last == 1.0, "progress did not reach completion");
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 5; ++x)
                    Check(pixels[static_cast<size_t>(y * 5 + x)] ==
                              Expected(x + 3, y + 7),
                          "progress read pixel mismatch");
        }
    }
    dataset.reset();
    VSIUnlink("/vsimem/jp2emuella-extra-args.j2k");
}

void TestSubfile(const std::vector<std::uint8_t> &fixture) {
    std::vector<std::uint8_t> container(23 + fixture.size() + 11, 0x35);
    std::copy(fixture.begin(), fixture.end(), container.begin() + 23);
    std::fill(container.end() - 11, container.end(), 0x79);
    PutVsiMem("/vsimem/jp2emuella-container.bin", container);
    const std::string path = "/vsisubfile/23_" +
                             std::to_string(fixture.size()) +
                             ",/vsimem/jp2emuella-container.bin";
    auto dataset = Open(path.c_str());
    Check(dataset != nullptr, "/vsisubfile/ codestream did not open");
    std::uint8_t pixel = 0;
    Check(dataset->GetRasterBand(1)->RasterIO(GF_Read, 4, 6, 1, 1, &pixel, 1, 1,
                                              GDT_Byte, 1, 1,
                                              nullptr) == CE_None &&
              pixel == Expected(4, 6),
          "/vsisubfile/ pixel mismatch");
    dataset.reset();
    VSIUnlink("/vsimem/jp2emuella-container.bin");
}

void TestRejections(const std::vector<std::uint8_t> &fixture) {
    const std::vector<std::uint8_t> jp2Signature = {
        0x00, 0x00, 0x00, 0x0c, 0x6a, 0x50, 0x20, 0x20, 0x0d, 0x0a, 0x87, 0x0a};
    PutVsiMem("/vsimem/jp2emuella-wrapper.jp2", jp2Signature);
    CPLPushErrorHandler(CPLQuietErrorHandler);
    Check(Open("/vsimem/jp2emuella-wrapper.jp2") == nullptr,
          "JP2 wrapper was incorrectly identified");
    CPLPopErrorHandler();
    VSIUnlink("/vsimem/jp2emuella-wrapper.jp2");

    PutVsiMem("/vsimem/jp2emuella-malformed.j2k", {0xff, 0x4f, 0xff, 0x51});
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    Check(Open("/vsimem/jp2emuella-malformed.j2k") == nullptr,
          "malformed codestream opened");
    CPLPopErrorHandler();
    Check(CPLGetLastErrorType() == CE_Failure &&
              std::string(CPLGetLastErrorMsg()).find("JP2Emuella") !=
                  std::string::npos,
          "malformed codestream did not report a GDAL error");
    VSIUnlink("/vsimem/jp2emuella-malformed.j2k");

    auto unsupported = fixture;
    Check(unsupported.size() > 42, "fixture is too short for SIZ mutation");
    unsupported[42] = 8; // Unsigned 9-bit component in the authored SIZ marker.
    PutVsiMem("/vsimem/jp2emuella-unsupported.j2k", unsupported);
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    Check(Open("/vsimem/jp2emuella-unsupported.j2k") == nullptr,
          "unsupported sample precision opened");
    CPLPopErrorHandler();
    Check(CPLGetLastErrorType() == CE_Failure &&
              std::string(CPLGetLastErrorMsg()).find("JP2Emuella") !=
                  std::string::npos,
          "unsupported codestream did not report a GDAL error");
    VSIUnlink("/vsimem/jp2emuella-unsupported.j2k");

    auto incompatiblePacketBody = fixture;
    Check(incompatiblePacketBody.size() > 55,
          "fixture is too short for COD mutation");
    incompatiblePacketBody[55] = 1; // One decomposition with a zero-level body.
    PutVsiMem("/vsimem/jp2emuella-incompatible-packet.j2k",
              incompatiblePacketBody);
    auto incompatibleDataset =
        Open("/vsimem/jp2emuella-incompatible-packet.j2k");
    Check(incompatibleDataset != nullptr,
          "incompatible packet body did not reach the decode path");
    std::vector<std::uint8_t> destination(20, 0xa5);
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    const auto decodeResult = incompatibleDataset->GetRasterBand(1)->RasterIO(
        GF_Read, 3, 7, 5, 4, destination.data(), 5, 4, GDT_Byte, 1, 5, nullptr);
    CPLPopErrorHandler();
    Check(decodeResult == CE_Failure, "incompatible packet body decoded");
    Check(std::all_of(destination.begin(), destination.end(),
                      [](std::uint8_t value) { return value == 0xa5; }),
          "failed decode modified the GDAL destination");
    Check(CPLGetLastErrorType() == CE_Failure &&
              std::string(CPLGetLastErrorMsg()).find("JP2Emuella") !=
                  std::string::npos,
          "decode failure did not report a GDAL error");
    incompatibleDataset.reset();
    VSIUnlink("/vsimem/jp2emuella-incompatible-packet.j2k");
}

void TestRepeatedConcurrentAndLifecycle(
    const std::vector<std::uint8_t> &fixture) {
    PutVsiMem("/vsimem/jp2emuella-concurrent.j2k", fixture);
    auto dataset = Open("/vsimem/jp2emuella-concurrent.j2k");
    Check(dataset != nullptr, "concurrency fixture did not open");

    std::atomic<bool> succeeded{true};
    std::vector<std::thread> threads;
    for (int threadIndex = 0; threadIndex < 6; ++threadIndex) {
        threads.emplace_back([&, threadIndex] {
            for (int iteration = 0; iteration < 20; ++iteration) {
                const int x = (threadIndex + iteration) % 13;
                const int y = (threadIndex * 3 + iteration) % 16;
                std::uint8_t pixels[12]{};
                if (dataset->GetRasterBand(1)->RasterIO(
                        GF_Read, x, y, 4, 3, pixels, 4, 3, GDT_Byte, 1, 4,
                        nullptr) != CE_None) {
                    succeeded = false;
                    return;
                }
                for (int wy = 0; wy < 3; ++wy)
                    for (int wx = 0; wx < 4; ++wx)
                        if (pixels[wy * 4 + wx] != Expected(x + wx, y + wy))
                            succeeded = false;
            }
        });
    }
    for (auto &thread : threads)
        thread.join();
    Check(succeeded.load(), "repeated concurrent reads failed");
    dataset.reset();

    for (int iteration = 0; iteration < 25; ++iteration) {
        auto reopened = Open("/vsimem/jp2emuella-concurrent.j2k");
        Check(reopened != nullptr, "dataset lifecycle reopen failed");
    }
    VSIUnlink("/vsimem/jp2emuella-concurrent.j2k");
}

} // namespace

int main() {
    try {
        GDALAllRegister();
        auto *driver = GetGDALDriverManager()->GetDriverByName("JP2Emuella");
        Check(driver != nullptr, "JP2Emuella plugin did not autoload");
        Check(std::string(driver->GetMetadataItem(GDAL_DCAP_RASTER)) == "YES",
              "raster capability metadata missing");
        Check(std::string(driver->GetMetadataItem(GDAL_DCAP_VIRTUALIO)) ==
                  "YES",
              "virtual I/O capability metadata missing");
        Check(std::string(driver->GetMetadataItem(GDAL_DMD_EXTENSIONS)) ==
                  "j2k j2c jpc",
              "extension metadata mismatch");

        const auto fixture = ReadFixture();
        TestMetadataAndPixels(fixture);
        TestExtraArgs(fixture);
        TestSubfile(fixture);
        TestRejections(fixture);
        TestRepeatedConcurrentAndLifecycle(fixture);
        GDALDestroyDriverManager();
        std::cout << "JP2Emuella native tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "JP2Emuella native test failure: " << error.what() << '\n';
        GDALDestroyDriverManager();
        return 1;
    }
}

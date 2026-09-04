#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal_alg.h"
#include "gdal_priv.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t FILE_LENGTH_OFFSET = 342;
constexpr std::size_t FILE_LENGTH_WIDTH = 12;
constexpr std::size_t HEADER_LENGTH_OFFSET = 354;
constexpr std::size_t HEADER_LENGTH_WIDTH = 6;
constexpr std::size_t IMAGE_COUNT_OFFSET = 360;
constexpr std::size_t IMAGE_COUNT_WIDTH = 3;
constexpr std::size_t IMAGE_SUBHEADER_LENGTH_OFFSET = 363;
constexpr std::size_t IMAGE_SUBHEADER_LENGTH_WIDTH = 6;
constexpr std::size_t IMAGE_DATA_LENGTH_OFFSET = 369;
constexpr std::size_t IMAGE_DATA_LENGTH_WIDTH = 10;
constexpr std::size_t IMAGE_COMPRESSION_OFFSET = 373;

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

std::uint64_t ReadDecimal(const std::vector<std::uint8_t> &bytes,
                          std::size_t offset, std::size_t width) {
    Check(offset <= bytes.size() && width <= bytes.size() - offset,
          "NITF decimal field is outside the skeleton");
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        const auto byte = bytes[offset + index];
        Check(byte >= '0' && byte <= '9', "NITF decimal field is malformed");
        Check(value <= (std::numeric_limits<std::uint64_t>::max() - 9) / 10,
              "NITF decimal field overflows");
        value = value * 10 + static_cast<std::uint64_t>(byte - '0');
    }
    return value;
}

void WriteDecimal(std::vector<std::uint8_t> &bytes, std::size_t offset,
                  std::size_t width, std::uint64_t value) {
    Check(offset <= bytes.size() && width <= bytes.size() - offset,
          "NITF decimal field is outside the output");
    for (std::size_t index = 0; index < width; ++index) {
        bytes[offset + width - index - 1] =
            static_cast<std::uint8_t>('0' + value % 10);
        value /= 10;
    }
    Check(value == 0, "NITF decimal value does not fit its field");
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

std::vector<std::uint8_t>
BuildNITFC8Fixture(const std::vector<std::uint8_t> &codestream) {
    constexpr const char *skeletonPath = "/vsimem/jp2emuella-nitf-nc.ntf";
    auto *driver = GetGDALDriverManager()->GetDriverByName("NITF");
    Check(driver != nullptr, "NITF driver is unavailable");

    CPLStringList options;
    options.SetNameValue("IC", "NC");
    DatasetPtr skeleton(
        driver->Create(skeletonPath, 17, 19, 1, GDT_Byte, options.List()));
    Check(skeleton != nullptr, "could not create NITF skeleton");
    skeleton.reset();

    vsi_l_offset skeletonSize = 0;
    const GByte *skeletonBytes =
        VSIGetMemFileBuffer(skeletonPath, &skeletonSize, FALSE);
    Check(skeletonBytes != nullptr, "could not read NITF skeleton");
    Check(skeletonSize <= static_cast<vsi_l_offset>(
                              std::numeric_limits<std::size_t>::max()),
          "NITF skeleton is too large");
    std::vector<std::uint8_t> skeletonCopy(
        skeletonBytes, skeletonBytes + static_cast<std::size_t>(skeletonSize));
    VSIUnlink(skeletonPath);

    Check(skeletonCopy.size() >=
              IMAGE_DATA_LENGTH_OFFSET + IMAGE_DATA_LENGTH_WIDTH,
          "NITF skeleton header is truncated");
    Check(
        std::equal(skeletonCopy.begin(), skeletonCopy.begin() + 9, "NITF02.10"),
        "unexpected NITF skeleton version");
    Check(ReadDecimal(skeletonCopy, IMAGE_COUNT_OFFSET, IMAGE_COUNT_WIDTH) == 1,
          "NITF skeleton must contain exactly one image");

    const auto headerLengthValue =
        ReadDecimal(skeletonCopy, HEADER_LENGTH_OFFSET, HEADER_LENGTH_WIDTH);
    const auto subheaderLengthValue =
        ReadDecimal(skeletonCopy, IMAGE_SUBHEADER_LENGTH_OFFSET,
                    IMAGE_SUBHEADER_LENGTH_WIDTH);
    const auto dataLengthValue = ReadDecimal(
        skeletonCopy, IMAGE_DATA_LENGTH_OFFSET, IMAGE_DATA_LENGTH_WIDTH);
    Check(headerLengthValue <= skeletonCopy.size() &&
              subheaderLengthValue <=
                  skeletonCopy.size() -
                      static_cast<std::size_t>(headerLengthValue),
          "NITF skeleton segment lengths are invalid");
    const auto headerLength = static_cast<std::size_t>(headerLengthValue);
    const auto subheaderLength = static_cast<std::size_t>(subheaderLengthValue);
    Check(dataLengthValue ==
              skeletonCopy.size() - headerLength - subheaderLength,
          "NITF skeleton data length is inconsistent");
    Check(IMAGE_COMPRESSION_OFFSET + 2 <= subheaderLength,
          "NITF image subheader is too short");

    std::vector<std::uint8_t> header(skeletonCopy.begin(),
                                     skeletonCopy.begin() + headerLength);
    std::vector<std::uint8_t> imageHeader(skeletonCopy.begin() + headerLength,
                                          skeletonCopy.begin() + headerLength +
                                              subheaderLength);
    Check(imageHeader[IMAGE_COMPRESSION_OFFSET] == 'N' &&
              imageHeader[IMAGE_COMPRESSION_OFFSET + 1] == 'C',
          "NITF skeleton image is not uncompressed");
    imageHeader[IMAGE_COMPRESSION_OFFSET] = 'C';
    imageHeader[IMAGE_COMPRESSION_OFFSET + 1] = '8';
    imageHeader.insert(imageHeader.begin() + IMAGE_COMPRESSION_OFFSET + 2,
                       {'N', '0', '0', '0'});

    std::uint64_t fileLength = header.size();
    Check(imageHeader.size() <=
              std::numeric_limits<std::uint64_t>::max() - fileLength,
          "NITF fixture length overflows");
    fileLength += imageHeader.size();
    Check(codestream.size() <=
              std::numeric_limits<std::uint64_t>::max() - fileLength,
          "NITF fixture length overflows");
    fileLength += codestream.size();
    Check(fileLength <= std::numeric_limits<std::size_t>::max(),
          "NITF fixture length is not representable");
    WriteDecimal(header, FILE_LENGTH_OFFSET, FILE_LENGTH_WIDTH, fileLength);
    WriteDecimal(header, IMAGE_SUBHEADER_LENGTH_OFFSET,
                 IMAGE_SUBHEADER_LENGTH_WIDTH, imageHeader.size());
    WriteDecimal(header, IMAGE_DATA_LENGTH_OFFSET, IMAGE_DATA_LENGTH_WIDTH,
                 codestream.size());

    std::vector<std::uint8_t> output;
    output.reserve(static_cast<std::size_t>(fileLength));
    output.insert(output.end(), header.begin(), header.end());
    output.insert(output.end(), imageHeader.begin(), imageHeader.end());
    output.insert(output.end(), codestream.begin(), codestream.end());
    Check(output.size() == fileLength, "NITF fixture length mismatch");
    return output;
}

DatasetPtr OpenNITF(const char *path) {
    const char *allowed[] = {"NITF", nullptr};
    return DatasetPtr(static_cast<GDALDataset *>(GDALOpenEx(
        path, GDAL_OF_RASTER | GDAL_OF_READONLY, allowed, nullptr, nullptr)));
}

DatasetPtr OpenJP2Emuella(const char *path) {
    const char *allowed[] = {"JP2Emuella", nullptr};
    return DatasetPtr(static_cast<GDALDataset *>(GDALOpenEx(
        path, GDAL_OF_RASTER | GDAL_OF_READONLY, allowed, nullptr, nullptr)));
}

void CheckNestedJP2Emuella(GDALDataset *dataset) {
    const char *nestedName =
        dataset->GetMetadataItem("JPEG2000_DATASET_NAME", "DEBUG");
    Check(nestedName != nullptr, "nested JPEG 2000 dataset name is missing");
    auto nested = OpenJP2Emuella(nestedName);
    Check(nested != nullptr &&
              std::string(nested->GetDriverName()) == "JP2Emuella",
          "NITF image segment did not select JP2Emuella");
}

void TestNITFDecode(const std::vector<std::uint8_t> &fixture) {
    constexpr const char *path = "/vsimem/jp2emuella-c8.ntf";
    PutVsiMem(path, fixture);
    auto dataset = OpenNITF(path);
    Check(dataset != nullptr, "IC=C8 NITF did not open");
    Check(std::string(dataset->GetDriverName()) == "NITF",
          "unexpected outer driver");
    Check(dataset->GetRasterXSize() == 17 && dataset->GetRasterYSize() == 19,
          "unexpected NITF raster dimensions");
    Check(dataset->GetRasterCount() == 1, "unexpected NITF band count");
    const char *compression = dataset->GetMetadataItem("NITF_IC");
    Check(compression != nullptr && std::string(compression) == "C8",
          "NITF compression metadata mismatch");

    CheckNestedJP2Emuella(dataset.get());

    std::vector<std::uint8_t> pixels(17 * 19);
    Check(dataset->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 17, 19,
                                              pixels.data(), 17, 19, GDT_Byte,
                                              1, 17, nullptr) == CE_None,
          "full NITF raster read failed");
    for (int y = 0; y < 19; ++y)
        for (int x = 0; x < 17; ++x)
            Check(pixels[static_cast<std::size_t>(y * 17 + x)] ==
                      Expected(x, y),
                  "full NITF raster pixel mismatch");

    std::vector<std::uint8_t> window(5 * 4);
    Check(dataset->GetRasterBand(1)->RasterIO(GF_Read, 3, 7, 5, 4,
                                              window.data(), 5, 4, GDT_Byte, 1,
                                              5, nullptr) == CE_None,
          "NITF window read failed");
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 5; ++x)
            Check(window[static_cast<std::size_t>(y * 5 + x)] ==
                      Expected(x + 3, y + 7),
                  "NITF window pixel mismatch");

    std::uint8_t edge = 0;
    Check(dataset->GetRasterBand(1)->RasterIO(GF_Read, 16, 18, 1, 1, &edge, 1,
                                              1, GDT_Byte, 1, 1,
                                              nullptr) == CE_None &&
              edge == Expected(16, 18),
          "NITF edge read failed");
    dataset.reset();
    VSIUnlink(path);
}

void TestExternalGDALNITF(const char *path) {
    auto dataset = OpenNITF(path);
    Check(dataset != nullptr, "external GDAL NITF fixture did not open");
    Check(std::string(dataset->GetDriverName()) == "NITF",
          "unexpected external fixture outer driver");
    Check(dataset->GetRasterXSize() == 200 && dataset->GetRasterYSize() == 100,
          "unexpected external fixture raster dimensions");
    Check(dataset->GetRasterCount() == 3,
          "unexpected external fixture band count");
    const char *compression = dataset->GetMetadataItem("NITF_IC");
    Check(compression != nullptr && std::string(compression) == "C8",
          "external fixture compression metadata mismatch");
    CheckNestedJP2Emuella(dataset.get());

    constexpr std::array<int, 3> expectedChecksums = {32398, 42502, 38882};
    for (int bandIndex = 0; bandIndex < 3; ++bandIndex) {
        auto *band = dataset->GetRasterBand(bandIndex + 1);
        Check(GDALChecksumImage(band, 0, 0, 200, 100) ==
                  expectedChecksums[static_cast<std::size_t>(bandIndex)],
              "external fixture full-band checksum mismatch");
    }

    constexpr int windowX = 37;
    constexpr int windowY = 23;
    constexpr int windowWidth = 61;
    constexpr int windowHeight = 29;
    std::vector<std::uint8_t> window(
        static_cast<std::size_t>(windowWidth * windowHeight * 3));
    Check(dataset->RasterIO(GF_Read, windowX, windowY, windowWidth,
                            windowHeight, window.data(), windowWidth,
                            windowHeight, GDT_Byte, 3, nullptr, 3,
                            3 * windowWidth, 1, nullptr) == CE_None,
          "external fixture window read failed");
    // GDAL created this fixture from horizontal ramps x, x + 20 and x + 30.
    // This interior window is unchanged by the fixture's lossy encoding.
    constexpr std::array<int, 3> bandOffsets = {0, 20, 30};
    for (int y = 0; y < windowHeight; ++y) {
        for (int x = 0; x < windowWidth; ++x) {
            const auto pixelIndex =
                static_cast<std::size_t>((y * windowWidth + x) * 3);
            for (int bandIndex = 0; bandIndex < 3; ++bandIndex) {
                const auto expected = static_cast<std::uint8_t>(
                    windowX + x +
                    bandOffsets[static_cast<std::size_t>(bandIndex)]);
                Check(
                    window[pixelIndex + static_cast<std::size_t>(bandIndex)] ==
                        expected,
                    "external fixture window pixel mismatch");
            }
        }
    }

    std::array<std::uint8_t, 3> edge = {0, 0, 0};
    Check(dataset->RasterIO(GF_Read, 199, 99, 1, 1, edge.data(), 1, 1, GDT_Byte,
                            3, nullptr, 3, 3, 1, nullptr) == CE_None,
          "external fixture edge read failed");
    constexpr std::array<std::uint8_t, 3> expectedEdge = {198, 218, 228};
    Check(edge == expectedEdge,
          "external fixture edge pixel mismatch: " + std::to_string(edge[0]) +
              ", " + std::to_string(edge[1]) + ", " + std::to_string(edge[2]));
}

std::atomic<int> trapIdentifications{0};

int TrapIdentify(GDALOpenInfo *openInfo) {
    if (openInfo->nHeaderBytes >= 4 && openInfo->pabyHeader[0] == 0xff &&
        openInfo->pabyHeader[1] == 0x4f && openInfo->pabyHeader[2] == 0xff &&
        openInfo->pabyHeader[3] == 0x51) {
        ++trapIdentifications;
        return TRUE;
    }
    return FALSE;
}

GDALDataset *TrapOpen(GDALOpenInfo *) { return nullptr; }

void TestNarrowAllowList(const std::vector<std::uint8_t> &fixture) {
    constexpr const char *path = "/vsimem/jp2emuella-c8-negative.ntf";
    PutVsiMem(path, fixture);

    auto *manager = GetGDALDriverManager();
    auto *emuella = manager->GetDriverByName("JP2Emuella");
    Check(emuella != nullptr, "JP2Emuella driver is unavailable");
    auto *trap = new GDALDriver();
    trap->SetDescription("NotAllowedJ2KProbe");
    trap->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    trap->pfnIdentify = TrapIdentify;
    trap->pfnOpen = TrapOpen;
    manager->RegisterDriver(trap);
    manager->DeregisterDriver(emuella);

    trapIdentifications = 0;
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    DatasetPtr dataset(static_cast<GDALDataset *>(GDALOpenEx(
        path, GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr)));
    CPLPopErrorHandler();

    manager->DeregisterDriver(trap);
    delete trap;
    manager->RegisterDriver(emuella);
    VSIUnlink(path);

    Check(dataset == nullptr,
          "C8 NITF opened without an allowed JPEG 2000 driver");
    Check(trapIdentifications.load() == 0,
          "NITF asked an unlisted driver to inspect embedded content");
}

} // namespace

int main() {
    try {
        GDALAllRegister();
        Check(GetGDALDriverManager()->GetDriverByName("JP2Emuella") != nullptr,
              "JP2Emuella plugin did not autoload");
        const auto fixture = BuildNITFC8Fixture(ReadFixture());
        TestNITFDecode(fixture);
        TestNarrowAllowList(fixture);
        if (const char *externalFixture =
                std::getenv("JP2EMUELLA_GDAL_NITF_FIXTURE");
            externalFixture != nullptr && externalFixture[0] != '\0')
            TestExternalGDALNITF(externalFixture);
        GDALDestroyDriverManager();
        std::cout << "JP2Emuella NITF tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "JP2Emuella NITF test failure: " << error.what() << '\n';
        GDALDestroyDriverManager();
        return 1;
    }
}

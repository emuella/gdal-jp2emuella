#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_vsi.h"
#include "emuella_j2k.h"
#include "gdal_pam.h"
#include "gdal_priv.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char *DRIVER_NAME = "JP2Emuella";

template <typename T, void (*Destroy)(T *)> struct HandleDeleter {
    void operator()(T *handle) const noexcept { Destroy(handle); }
};

using DecoderPtr = std::unique_ptr<
    EmuellaJ2kDecoder,
    HandleDeleter<EmuellaJ2kDecoder, emuella_j2k_decoder_destroy>>;
using InspectionPtr = std::unique_ptr<
    EmuellaJ2kInspection,
    HandleDeleter<EmuellaJ2kInspection, emuella_j2k_inspection_destroy>>;
using WorkspacePtr = std::unique_ptr<
    EmuellaJ2kWorkspace,
    HandleDeleter<EmuellaJ2kWorkspace, emuella_j2k_workspace_destroy>>;
using ImagePtr =
    std::unique_ptr<EmuellaJ2kImage,
                    HandleDeleter<EmuellaJ2kImage, emuella_j2k_image_destroy>>;
using ErrorPtr =
    std::unique_ptr<EmuellaJ2kError,
                    HandleDeleter<EmuellaJ2kError, emuella_j2k_error_destroy>>;

struct VSIFileCloser {
    void operator()(VSILFILE *file) const noexcept {
        if (file != nullptr)
            VSIFCloseL(file);
    }
};
using VSIFilePtr = std::unique_ptr<VSILFILE, VSIFileCloser>;

std::string Diagnostic(EmuellaJ2kStatus status, EmuellaJ2kError *rawError) {
    ErrorPtr error(rawError);
    if (!error)
        return "codec status " + std::to_string(status);

    size_t required = 0;
    if (emuella_j2k_error_message_size(error.get(), &required) !=
            EMUELLA_J2K_STATUS_OK ||
        required == 0)
        return "codec status " + std::to_string(status);

    std::vector<std::uint8_t> bytes(required);
    if (emuella_j2k_error_message_copy(error.get(), bytes.data(),
                                       bytes.size()) != EMUELLA_J2K_STATUS_OK)
        return "codec status " + std::to_string(status);
    return reinterpret_cast<const char *>(bytes.data());
}

bool CodecCallSucceeded(EmuellaJ2kStatus status, EmuellaJ2kError *error,
                        const char *operation) {
    if (status == EMUELLA_J2K_STATUS_OK) {
        ErrorPtr unexpected(error);
        return true;
    }
    CPLError(CE_Failure, CPLE_AppDefined, "JP2Emuella %s failed: %s", operation,
             Diagnostic(status, error).c_str());
    return false;
}

class JP2EmuellaDataset final : public GDALPamDataset {
    friend class JP2EmuellaRasterBand;

    VSIFilePtr file_;
    std::uint64_t length_ = 0;
    std::mutex fileMutex_;
    EmuellaJ2kSourceV0 source_{};
    DecoderPtr decoder_;
    std::uint32_t imageOriginX_ = 0;
    std::uint32_t imageOriginY_ = 0;

    static EmuellaJ2kStatus ReadAt(void *context, std::uint64_t offset,
                                   std::uint8_t *destination,
                                   size_t length) noexcept {
        try {
            if (context == nullptr || (length != 0 && destination == nullptr))
                return EMUELLA_J2K_STATUS_SOURCE_IO;
            auto *dataset = static_cast<JP2EmuellaDataset *>(context);
            if (!dataset->file_ || offset > dataset->length_ ||
                static_cast<std::uint64_t>(length) >
                    dataset->length_ - offset ||
                offset > std::numeric_limits<vsi_l_offset>::max())
                return EMUELLA_J2K_STATUS_SOURCE_IO;
            if (length == 0)
                return EMUELLA_J2K_STATUS_OK;

            std::lock_guard<std::mutex> lock(dataset->fileMutex_);
            if (VSIFSeekL(dataset->file_.get(),
                          static_cast<vsi_l_offset>(offset), SEEK_SET) != 0)
                return EMUELLA_J2K_STATUS_SOURCE_IO;
            if (VSIFReadL(destination, 1, length, dataset->file_.get()) !=
                length)
                return EMUELLA_J2K_STATUS_SOURCE_IO;
            return EMUELLA_J2K_STATUS_OK;
        } catch (...) {
            return EMUELLA_J2K_STATUS_SOURCE_IO;
        }
    }

  public:
    JP2EmuellaDataset(VSIFilePtr file, std::uint64_t length)
        : file_(std::move(file)), length_(length) {
        source_.struct_size = sizeof(source_);
        source_.abi_version = EMUELLA_J2K_ABI_VERSION;
        source_.length = length_;
        source_.context = this;
        source_.read_at = ReadAt;
    }

    ~JP2EmuellaDataset() override {
        FlushCache(true);
        decoder_.reset();
        file_.reset();
    }

    CPLErr DecodeRegion(int component, int x, int y, int width, int height,
                        std::uint8_t *destination, size_t stride) {
        if (component < 0 || component >= GetRasterCount() || x < 0 || y < 0 ||
            width <= 0 || height <= 0 || x > nRasterXSize - width ||
            y > nRasterYSize - height || destination == nullptr ||
            stride < static_cast<size_t>(width)) {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "JP2Emuella received an invalid decode region");
            return CE_Failure;
        }
        const auto rowBytes = static_cast<size_t>(width);
        const auto precedingRows = static_cast<size_t>(height - 1);
        if (precedingRows != 0 &&
            stride > (std::numeric_limits<size_t>::max() - rowBytes) /
                         precedingRows) {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "JP2Emuella decode destination extent overflows");
            return CE_Failure;
        }
        const size_t capacity = precedingRows * stride + rowBytes;

        EmuellaJ2kWorkspace *rawWorkspace = nullptr;
        EmuellaJ2kError *rawError = nullptr;
        auto status = emuella_j2k_workspace_create(&rawWorkspace, &rawError);
        WorkspacePtr workspace(rawWorkspace);
        if (!CodecCallSucceeded(status, rawError, "workspace creation"))
            return CE_Failure;

        EmuellaJ2kDecodeRequestV0 request{};
        request.struct_size = sizeof(request);
        request.abi_version = EMUELLA_J2K_ABI_VERSION;
        request.component = static_cast<std::uint16_t>(component);
        request.x = static_cast<std::uint32_t>(x);
        request.y = static_cast<std::uint32_t>(y);
        request.width = static_cast<std::uint32_t>(width);
        request.height = static_cast<std::uint32_t>(height);

        EmuellaJ2kImage *rawImage = nullptr;
        rawError = nullptr;
        status = emuella_j2k_decode_component_region(
            decoder_.get(), workspace.get(), &request, &rawImage, &rawError);
        ImagePtr image(rawImage);
        if (!CodecCallSucceeded(status, rawError, "region decode"))
            return CE_Failure;

        EmuellaJ2kImageInfoV0 info{};
        info.struct_size = sizeof(info);
        info.abi_version = EMUELLA_J2K_ABI_VERSION;
        rawError = nullptr;
        status = emuella_j2k_image_info(image.get(), &info, &rawError);
        if (!CodecCallSucceeded(status, rawError, "decoded image inspection"))
            return CE_Failure;
        if (info.width != static_cast<std::uint32_t>(width) ||
            info.height != static_cast<std::uint32_t>(height) ||
            info.component_count != 1 || info.bits_per_sample != 8 ||
            info.is_signed != 0 || info.byte_order != EMUELLA_J2K_ENDIAN_NONE) {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "JP2Emuella codec returned incompatible decoded geometry");
            return CE_Failure;
        }

        EmuellaJ2kComponentInfoV0 componentInfo{};
        componentInfo.struct_size = sizeof(componentInfo);
        componentInfo.abi_version = EMUELLA_J2K_ABI_VERSION;
        rawError = nullptr;
        status = emuella_j2k_image_component_info(image.get(), &componentInfo,
                                                  &rawError);
        if (!CodecCallSucceeded(status, rawError,
                                "decoded component inspection"))
            return CE_Failure;
        const auto expectedOriginX = static_cast<std::uint64_t>(imageOriginX_) +
                                     static_cast<std::uint32_t>(x);
        const auto expectedOriginY = static_cast<std::uint64_t>(imageOriginY_) +
                                     static_cast<std::uint32_t>(y);
        if (componentInfo.source_component !=
                static_cast<std::uint16_t>(component) ||
            componentInfo.bits_per_sample != 8 ||
            componentInfo.is_signed != 0 ||
            componentInfo.byte_order != EMUELLA_J2K_ENDIAN_NONE ||
            componentInfo.horizontal_separation != 1 ||
            componentInfo.vertical_separation != 1 ||
            componentInfo.width != static_cast<std::uint32_t>(width) ||
            componentInfo.height != static_cast<std::uint32_t>(height) ||
            componentInfo.x_origin != expectedOriginX ||
            componentInfo.y_origin != expectedOriginY) {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "JP2Emuella codec returned an incompatible component layout");
            return CE_Failure;
        }

        rawError = nullptr;
        status = emuella_j2k_image_copy(image.get(), destination, capacity,
                                        stride, &rawError);
        if (!CodecCallSucceeded(status, rawError, "decoded image copy"))
            return CE_Failure;
        return CE_None;
    }

    static int Identify(GDALOpenInfo *openInfo) {
        if (openInfo == nullptr || openInfo->nHeaderBytes < 4)
            return FALSE;
        const GByte signature[] = {0xff, 0x4f, 0xff, 0x51};
        return std::memcmp(openInfo->pabyHeader, signature,
                           sizeof(signature)) == 0;
    }

    static GDALDataset *Open(GDALOpenInfo *openInfo);
};

class JP2EmuellaRasterBand final : public GDALPamRasterBand {
  public:
    JP2EmuellaRasterBand(JP2EmuellaDataset *dataset, int band) {
        poDS = dataset;
        nBand = band;
        nRasterXSize = dataset->GetRasterXSize();
        nRasterYSize = dataset->GetRasterYSize();
        eDataType = GDT_Byte;
        nBlockXSize = std::min(256, dataset->GetRasterXSize());
        nBlockYSize = std::min(256, dataset->GetRasterYSize());
    }

    CPLErr IReadBlock(int blockX, int blockY, void *buffer) override {
        const int x = blockX * nBlockXSize;
        const int y = blockY * nBlockYSize;
        const int width = std::min(nBlockXSize, nRasterXSize - x);
        const int height = std::min(nBlockYSize, nRasterYSize - y);
        std::memset(buffer, 0,
                    static_cast<size_t>(nBlockXSize) *
                        static_cast<size_t>(nBlockYSize));
        auto *dataset = static_cast<JP2EmuellaDataset *>(poDS);
        return dataset->DecodeRegion(nBand - 1, x, y, width, height,
                                     static_cast<std::uint8_t *>(buffer),
                                     static_cast<size_t>(nBlockXSize));
    }

    CPLErr IRasterIO(GDALRWFlag readWriteFlag, int x, int y, int width,
                     int height, void *buffer, int bufferWidth,
                     int bufferHeight, GDALDataType bufferType,
                     GSpacing pixelSpace, GSpacing lineSpace,
                     GDALRasterIOExtraArg *extraArg) override {
        if (readWriteFlag != GF_Read) {
            CPLError(CE_Failure, CPLE_NotSupported, "JP2Emuella is read-only");
            return CE_Failure;
        }
        if (width == bufferWidth && height == bufferHeight &&
            bufferType == GDT_Byte && pixelSpace == 1 &&
            lineSpace >= static_cast<GSpacing>(width)) {
            return static_cast<JP2EmuellaDataset *>(poDS)->DecodeRegion(
                nBand - 1, x, y, width, height,
                static_cast<std::uint8_t *>(buffer),
                static_cast<size_t>(lineSpace));
        }
        return GDALPamRasterBand::IRasterIO(
            readWriteFlag, x, y, width, height, buffer, bufferWidth,
            bufferHeight, bufferType, pixelSpace, lineSpace, extraArg);
    }
};

GDALDataset *JP2EmuellaDataset::Open(GDALOpenInfo *openInfo) {
    if (!Identify(openInfo))
        return nullptr;
    if (openInfo->eAccess != GA_ReadOnly) {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "JP2Emuella supports read-only access");
        return nullptr;
    }
    if (emuella_j2k_abi_version() != EMUELLA_J2K_ABI_VERSION) {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "JP2Emuella requires Emuella JPEG 2000 C ABI version %u",
                 EMUELLA_J2K_ABI_VERSION);
        return nullptr;
    }

    VSIFilePtr file(VSIFOpenL(openInfo->pszFilename, "rb"));
    if (!file) {
        CPLError(CE_Failure, CPLE_OpenFailed, "JP2Emuella could not open %s",
                 openInfo->pszFilename);
        return nullptr;
    }
    if (VSIFSeekL(file.get(), 0, SEEK_END) != 0) {
        CPLError(CE_Failure, CPLE_FileIO,
                 "JP2Emuella could not determine source length");
        return nullptr;
    }
    const vsi_l_offset fileLength = VSIFTellL(file.get());
    if (fileLength == static_cast<vsi_l_offset>(-1) ||
        VSIFSeekL(file.get(), 0, SEEK_SET) != 0) {
        CPLError(CE_Failure, CPLE_FileIO,
                 "JP2Emuella source length is unavailable");
        return nullptr;
    }

    auto dataset = std::make_unique<JP2EmuellaDataset>(
        std::move(file), static_cast<std::uint64_t>(fileLength));
    EmuellaJ2kDecoder *rawDecoder = nullptr;
    EmuellaJ2kError *rawError = nullptr;
    auto status =
        emuella_j2k_decoder_create(&dataset->source_, &rawDecoder, &rawError);
    DecoderPtr decoder(rawDecoder);
    if (!CodecCallSucceeded(status, rawError, "decoder creation"))
        return nullptr;
    dataset->decoder_ = std::move(decoder);

    EmuellaJ2kInspection *rawInspection = nullptr;
    rawError = nullptr;
    status = emuella_j2k_decoder_inspect(dataset->decoder_.get(),
                                         &rawInspection, &rawError);
    InspectionPtr inspection(rawInspection);
    if (!CodecCallSucceeded(status, rawError, "codestream inspection"))
        return nullptr;

    EmuellaJ2kImageInfoV0 imageInfo{};
    imageInfo.struct_size = sizeof(imageInfo);
    imageInfo.abi_version = EMUELLA_J2K_ABI_VERSION;
    rawError = nullptr;
    status = emuella_j2k_inspection_image_info(inspection.get(), &imageInfo,
                                               &rawError);
    if (!CodecCallSucceeded(status, rawError, "image inspection"))
        return nullptr;
    if (imageInfo.width == 0 || imageInfo.height == 0 ||
        imageInfo.width >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        imageInfo.height >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        imageInfo.component_count == 0 || imageInfo.bits_per_sample != 8 ||
        imageInfo.is_signed != 0 ||
        imageInfo.byte_order != EMUELLA_J2K_ENDIAN_NONE) {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "JP2Emuella does not support this image geometry");
        return nullptr;
    }

    std::vector<EmuellaJ2kComponentInfoV0> components;
    components.reserve(imageInfo.component_count);
    for (std::uint16_t component = 0; component < imageInfo.component_count;
         ++component) {
        EmuellaJ2kComponentInfoV0 info{};
        info.struct_size = sizeof(info);
        info.abi_version = EMUELLA_J2K_ABI_VERSION;
        rawError = nullptr;
        status = emuella_j2k_inspection_component_info(
            inspection.get(), component, &info, &rawError);
        if (!CodecCallSucceeded(status, rawError, "component inspection"))
            return nullptr;
        if (info.source_component != component || info.bits_per_sample != 8 ||
            info.is_signed != 0 || info.byte_order != EMUELLA_J2K_ENDIAN_NONE ||
            info.horizontal_separation != 1 || info.vertical_separation != 1 ||
            info.width != imageInfo.width || info.height != imageInfo.height ||
            (!components.empty() &&
             (info.x_origin != components.front().x_origin ||
              info.y_origin != components.front().y_origin))) {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "JP2Emuella initially supports only co-sited, unsigned "
                     "8-bit components with unit separation and full image "
                     "dimensions");
            return nullptr;
        }
        components.push_back(info);
    }

    dataset->nRasterXSize = static_cast<int>(imageInfo.width);
    dataset->nRasterYSize = static_cast<int>(imageInfo.height);
    dataset->imageOriginX_ = components.front().x_origin;
    dataset->imageOriginY_ = components.front().y_origin;
    dataset->SetDescription(openInfo->pszFilename);
    dataset->SetMetadataItem("CODEC", "Emuella JPEG 2000");
    dataset->SetMetadataItem("CODEC_VERSION", emuella_j2k_package_version());
    dataset->SetMetadataItem("CODESTREAM_KIND",
                             "JPEG 2000 Part 1 raw codestream");
    dataset->SetMetadataItem("IMAGE_ORIGIN_X",
                             std::to_string(dataset->imageOriginX_).c_str());
    dataset->SetMetadataItem("IMAGE_ORIGIN_Y",
                             std::to_string(dataset->imageOriginY_).c_str());
    for (int band = 1; band <= static_cast<int>(imageInfo.component_count);
         ++band)
        dataset->SetBand(band, new JP2EmuellaRasterBand(dataset.get(), band));
    dataset->TryLoadXML();
    dataset->oOvManager.Initialize(dataset.get(), openInfo->pszFilename);
    return dataset.release();
}

} // namespace

extern "C" CPL_DLL void GDALRegister_JP2Emuella() {
    if (GDALGetDriverByName(DRIVER_NAME) != nullptr)
        return;
    auto *driver = new GDALDriver();
    driver->SetDescription(DRIVER_NAME);
    driver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    driver->SetMetadataItem(GDAL_DCAP_OPEN, "YES");
    driver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    driver->SetMetadataItem(GDAL_DMD_LONGNAME,
                            "JPEG 2000 Part 1 (Emuella experimental decoder)");
    driver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "j2k j2c jpc");
    driver->SetMetadataItem(GDAL_DMD_MIMETYPE, "image/j2k");
    driver->SetMetadataItem(GDAL_DMD_HELPTOPIC,
                            "drivers/raster/jp2emuella.html");
    driver->pfnIdentify = JP2EmuellaDataset::Identify;
    driver->pfnOpen = JP2EmuellaDataset::Open;
    GetGDALDriverManager()->RegisterDriver(driver);
}

extern "C" CPL_DLL void GDALRegisterMe() { GDALRegister_JP2Emuella(); }

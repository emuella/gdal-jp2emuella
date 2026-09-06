#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "emuella_j2k.h"
#include "gdal_pam.h"
#include "gdal_priv.h"

#include <algorithm>
#include <array>
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
    // Lock order: decodeMutex_ then fileMutex_. The source callback only takes
    // fileMutex_, and never re-enters GDAL or the workspace. Handles are destroyed
    // before their source context and VSI file.
    std::mutex decodeMutex_;
    WorkspacePtr workspace_;
    std::vector<std::uint8_t> scatterPlane_;
    std::uint64_t scatterGrowths_ = 0;
    bool diagnostics_ = false;
    std::uint64_t sourceBytes_ = 0;
    std::uint64_t decodeCount_ = 0;
    std::uint64_t workspaceCreations_ = 0;
    EmuellaJ2kDecodeWorkV0 work_{};
    char **diagnosticMetadata_ = nullptr;

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
            if (dataset->diagnostics_)
                dataset->sourceBytes_ += length;
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
    JP2EmuellaDataset(VSIFilePtr file, std::uint64_t length, bool diagnostics)
        : file_(std::move(file)), length_(length), diagnostics_(diagnostics) {
        source_.struct_size = sizeof(source_);
        source_.abi_version = EMUELLA_J2K_ABI_VERSION;
        source_.length = length_;
        source_.context = this;
        source_.read_at = ReadAt;
    }

    ~JP2EmuellaDataset() override {
        FlushCache(true);
        workspace_.reset();
        decoder_.reset();
        CSLDestroy(diagnosticMetadata_);
        file_.reset();
    }

    CSLConstList GetMetadata(const char *domain = "") override {
        if (domain == nullptr || !EQUAL(domain, "EMUELLA_DIAGNOSTICS"))
            return GDALPamDataset::GetMetadata(domain);
        if (!diagnostics_)
            return nullptr;
        std::lock_guard<std::mutex> decodeLock(decodeMutex_);
        std::lock_guard<std::mutex> fileLock(fileMutex_);
        auto set = [&](const char *key, std::uint64_t value) {
            diagnosticMetadata_ = CSLSetNameValue(
                diagnosticMetadata_, key, std::to_string(value).c_str());
        };
        set("SCHEMA_VERSION", 1);
        set("SOURCE_BYTES_REQUESTED", sourceBytes_);
        set("DECODE_COUNT", decodeCount_);
        set("WORKSPACE_CREATIONS", workspaceCreations_);
        set("SCATTER_GROWTH_REQUESTS", scatterGrowths_);
        set("PEAK_SCATTER_CAPACITY_BYTES", scatterPlane_.capacity());
#define WORK_SUM(name) set(#name, work_.name)
        WORK_SUM(preparation_count);
        WORK_SUM(code_blocks_decoded);
        WORK_SUM(tier1_coefficients);
        WORK_SUM(dwt_samples);
        WORK_SUM(synthesis_coefficients_loaded);
        WORK_SUM(synthesis_horizontal_values);
        WORK_SUM(synthesis_vertical_values);
        WORK_SUM(synthesis_lifting_updates);
        WORK_SUM(synthesis_output_samples);
        WORK_SUM(windowed_synthesis_component_tiles);
        WORK_SUM(full_synthesis_component_tiles);
        WORK_SUM(output_allocation_bytes);
        WORK_SUM(output_allocation_count);
#undef WORK_SUM
#define WORK_PEAK(name) set("PEAK_" #name, work_.name)
        WORK_PEAK(output_capacity_bytes);
        WORK_PEAK(workspace_retained_heap_bytes);
        WORK_PEAK(coefficient_capacity);
        WORK_PEAK(segment_capacity);
        WORK_PEAK(transform_capacity);
        WORK_PEAK(full_coefficient_plane_capacity);
        WORK_PEAK(full_transform_scratch_capacity);
#undef WORK_PEAK
        return diagnosticMetadata_;
    }

    const char *GetMetadataItem(const char *name,
                                const char *domain = "") override {
        if (domain != nullptr && EQUAL(domain, "EMUELLA_DIAGNOSTICS"))
            return CSLFetchNameValue(GetMetadata(domain), name);
        return GDALPamDataset::GetMetadataItem(name, domain);
    }

    // Only monotone, non-overlapping planar or pixel-interleaved layouts are
    // admitted. Bound the complete offset by ptrdiff_t before pointer arithmetic.
    static bool DirectLayout(int width, int height, int bands,
                             GSpacing pixel, GSpacing line, GSpacing band) {
        if (width <= 0 || height <= 0 || bands <= 0 || pixel <= 0 ||
            line <= 0 || band <= 0)
            return false;
        const auto limit = static_cast<std::uint64_t>(
            std::numeric_limits<std::ptrdiff_t>::max());
        std::uint64_t extent = 1;
        auto add = [&](int count, GSpacing stride) {
            const auto step = static_cast<std::uint64_t>(stride);
            if (step > limit || (count > 1 &&
                step > (limit - extent) / static_cast<unsigned>(count - 1)))
                return false;
            extent += static_cast<unsigned>(count - 1) * step;
            return true;
        };
        if (!add(width, pixel) || !add(height, line) || !add(bands, band))
            return false;
        const auto rowExtent = static_cast<std::uint64_t>(width - 1) *
                               static_cast<std::uint64_t>(pixel) + 1;
        const auto planeExtent = static_cast<std::uint64_t>(height - 1) *
                                 static_cast<std::uint64_t>(line) + rowExtent;
        const auto sampleExtent = static_cast<std::uint64_t>(bands - 1) *
                                  static_cast<std::uint64_t>(band) + 1;
        return (static_cast<std::uint64_t>(line) >= rowExtent &&
                (bands == 1 || static_cast<std::uint64_t>(band) >= planeExtent)) ||
               (static_cast<std::uint64_t>(pixel) >= sampleExtent &&
                static_cast<std::uint64_t>(line) >= rowExtent + sampleExtent - 1);
    }

    CPLErr DecodeBands(int x, int y, int width, int height, void *destination,
                       int bandCount, const int *bandMap, GSpacing pixelSpace,
                       GSpacing lineSpace, GSpacing bandSpace) try {
        if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
            x > nRasterXSize - width || y > nRasterYSize - height ||
            destination == nullptr || bandMap == nullptr ||
            !DirectLayout(width, height, bandCount, pixelSpace, lineSpace, bandSpace)) {
            CPLError(CE_Failure, CPLE_IllegalArg,
                     "JP2Emuella received an invalid decode region or layout");
            return CE_Failure;
        }
        std::lock_guard<std::mutex> lock(decodeMutex_);
        EmuellaJ2kDecodeComponentsRequestV0 request{};
        request.struct_size = sizeof(request);
        request.abi_version = EMUELLA_J2K_ABI_VERSION;
        request.x = static_cast<std::uint32_t>(x);
        request.y = static_cast<std::uint32_t>(y);
        request.width = static_cast<std::uint32_t>(width);
        request.height = static_cast<std::uint32_t>(height);
        request.collect_work = diagnostics_ ? 1 : 0;
        std::vector<std::uint16_t> outputIndices;
        outputIndices.reserve(static_cast<size_t>(bandCount));
        for (int band = 0; band < bandCount; ++band) {
            if (bandMap[band] < 1 || bandMap[band] > GetRasterCount()) {
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "JP2Emuella received an invalid band index");
                return CE_Failure;
            }
            const auto component = static_cast<std::uint16_t>(bandMap[band] - 1);
            std::uint16_t index = 0;
            while (index < request.component_count &&
                   request.components[index] != component)
                ++index;
            if (index == request.component_count) {
                if (request.component_count == 4)
                    return CE_Failure; // Caller checks the distinct-band limit.
                request.components[request.component_count++] = component;
            }
            outputIndices.push_back(index);
        }
        EmuellaJ2kError *rawError = nullptr;
        if (!workspace_) {
            EmuellaJ2kWorkspace *rawWorkspace = nullptr;
            const auto status = emuella_j2k_workspace_create(&rawWorkspace, &rawError);
            workspace_.reset(rawWorkspace);
            if (!CodecCallSucceeded(status, rawError, "workspace creation"))
                return CE_Failure;
            if (diagnostics_)
                ++workspaceCreations_;
        }
        EmuellaJ2kImage *rawImage = nullptr;
        rawError = nullptr;
        auto status = emuella_j2k_decode_components_region(
            decoder_.get(), workspace_.get(), &request, &rawImage, &rawError);
        ImagePtr image(rawImage);
        if (!CodecCallSucceeded(status, rawError, "region decode"))
            return CE_Failure;
        // Validate every output before touching the GDAL destination.
        for (std::uint16_t index = 0; index < request.component_count; ++index) {
            EmuellaJ2kComponentInfoV0 info{};
            info.struct_size = sizeof(info);
            info.abi_version = EMUELLA_J2K_ABI_VERSION;
            rawError = nullptr;
            status = emuella_j2k_image_component_info_at(image.get(), index,
                                                        &info, &rawError);
            if (!CodecCallSucceeded(status, rawError, "decoded component inspection"))
                return CE_Failure;
            if (info.source_component != request.components[index] ||
                info.bits_per_sample != 8 || info.is_signed != 0 ||
                info.byte_order != EMUELLA_J2K_ENDIAN_NONE ||
                info.horizontal_separation != 1 || info.vertical_separation != 1 ||
                info.width != request.width || info.height != request.height ||
                info.x_origin != static_cast<std::uint64_t>(imageOriginX_) + request.x ||
                info.y_origin != static_cast<std::uint64_t>(imageOriginY_) + request.y) {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "JP2Emuella codec returned an incompatible component layout");
                return CE_Failure;
            }
        }
        if (diagnostics_) {
            EmuellaJ2kDecodeWorkV0 work{};
            work.struct_size = sizeof(work);
            work.abi_version = EMUELLA_J2K_ABI_VERSION;
            rawError = nullptr;
            status = emuella_j2k_image_decode_work(image.get(), &work, &rawError);
            if (!CodecCallSucceeded(status, rawError, "decode work inspection"))
                return CE_Failure;
            ++decodeCount_;
#define WORK_SUM(name) work_.name += work.name
            WORK_SUM(preparation_count);
            WORK_SUM(code_blocks_decoded);
            WORK_SUM(tier1_coefficients);
            WORK_SUM(dwt_samples);
            WORK_SUM(synthesis_coefficients_loaded);
            WORK_SUM(synthesis_horizontal_values);
            WORK_SUM(synthesis_vertical_values);
            WORK_SUM(synthesis_lifting_updates);
            WORK_SUM(synthesis_output_samples);
            WORK_SUM(windowed_synthesis_component_tiles);
            WORK_SUM(full_synthesis_component_tiles);
            WORK_SUM(output_allocation_bytes);
            WORK_SUM(output_allocation_count);
#undef WORK_SUM
#define WORK_PEAK(name) work_.name = std::max(work_.name, work.name)
            WORK_PEAK(output_capacity_bytes);
            WORK_PEAK(workspace_retained_heap_bytes);
            WORK_PEAK(coefficient_capacity);
            WORK_PEAK(segment_capacity);
            WORK_PEAK(transform_capacity);
            WORK_PEAK(full_coefficient_plane_capacity);
            WORK_PEAK(full_transform_scratch_capacity);
#undef WORK_PEAK
        }
        // Retain one scratch plane only when scatter is needed; contiguous
        // planar reads copy directly. Its allocation is outside codec metrics.
        if (pixelSpace != 1) {
            const auto size = static_cast<size_t>(width) * static_cast<size_t>(height);
            if (diagnostics_ && scatterPlane_.capacity() < size)
                ++scatterGrowths_;
            scatterPlane_.resize(size);
        }
        for (int band = 0; band < bandCount; ++band) {
            auto *target = static_cast<std::uint8_t *>(destination) + band * bandSpace;
            auto *copyTarget = pixelSpace == 1 ? target : scatterPlane_.data();
            const size_t stride = pixelSpace == 1 ? static_cast<size_t>(lineSpace) :
                                                   static_cast<size_t>(width);
            const size_t capacity = static_cast<size_t>(height - 1) * stride +
                                    static_cast<size_t>(width);
            rawError = nullptr;
            status = emuella_j2k_image_copy_component(image.get(),
                outputIndices[static_cast<size_t>(band)], copyTarget, capacity,
                stride, &rawError);
            if (!CodecCallSucceeded(status, rawError, "decoded component copy"))
                return CE_Failure;
            if (pixelSpace != 1)
                for (int row = 0; row < height; ++row)
                    for (int column = 0; column < width; ++column)
                        target[row * lineSpace + column * pixelSpace] =
                            scatterPlane_[static_cast<size_t>(row) * static_cast<size_t>(width) +
                                  static_cast<size_t>(column)];
        }
        return CE_None;
    } catch (const std::exception &error) {
        CPLError(CE_Failure, CPLE_AppDefined, "JP2Emuella region read failed: %s",
                 error.what());
        return CE_Failure;
    }

    CPLErr DecodeRegion(int component, int x, int y, int width, int height,
                        std::uint8_t *destination, size_t stride) {
        const int band = component + 1;
        return DecodeBands(x, y, width, height, destination, 1, &band, 1,
                           static_cast<GSpacing>(stride), 1);
    }

    CPLErr IRasterIO(GDALRWFlag flag, int x, int y, int width, int height,
                     void *buffer, int bufferWidth, int bufferHeight,
                     GDALDataType type, int bandCount, int *bandMap,
                     GSpacing pixelSpace, GSpacing lineSpace, GSpacing bandSpace,
                     GDALRasterIOExtraArg *extra) override {
        const bool generic = extra != nullptr &&
            (extra->bFloatingPointWindowValidity || extra->pfnProgress != nullptr ||
             extra->eResampleAlg != GRIORA_NearestNeighbour);
        std::array<int, 4> unique{};
        int uniqueCount = 0;
        bool supported = bandCount > 0 && bandMap != nullptr;
        for (int index = 0; supported && index < bandCount; ++index) {
            if (bandMap[index] < 1 || bandMap[index] > GetRasterCount()) {
                supported = false;
                break;
            }
            if (std::find(unique.begin(), unique.begin() + uniqueCount,
                          bandMap[index]) == unique.begin() + uniqueCount) {
                if (uniqueCount == 4)
                    supported = false;
                else
                    unique[static_cast<size_t>(uniqueCount++)] = bandMap[index];
            }
        }
        if (flag == GF_Read && !generic && supported && type == GDT_Byte &&
            width == bufferWidth && height == bufferHeight &&
            DirectLayout(width, height, bandCount, pixelSpace, lineSpace, bandSpace))
            return DecodeBands(x, y, width, height, buffer, bandCount, bandMap,
                               pixelSpace, lineSpace, bandSpace);
        return GDALPamDataset::IRasterIO(flag, x, y, width, height, buffer,
            bufferWidth, bufferHeight, type, bandCount, bandMap, pixelSpace,
            lineSpace, bandSpace, extra);
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
        // GDAL owns fractional-window resampling and progress/cancellation.
        const bool needsGenericIO =
            extraArg != nullptr &&
            (extraArg->bFloatingPointWindowValidity ||
             extraArg->pfnProgress != nullptr ||
             extraArg->eResampleAlg != GRIORA_NearestNeighbour);
        if (!needsGenericIO && width == bufferWidth && height == bufferHeight &&
            bufferType == GDT_Byte && pixelSpace == 1 &&
            JP2EmuellaDataset::DirectLayout(width, height, 1, pixelSpace, lineSpace, 1)) {
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
        std::move(file), static_cast<std::uint64_t>(fileLength),
        CPLTestBool(CSLFetchNameValueDef(openInfo->papszOpenOptions,
                                       "DIAGNOSTICS", "NO")));
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
    driver->SetMetadataItem(GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList><Option name='DIAGNOSTICS' type='boolean' default='NO' "
        "description='Collect dataset decode work in EMUELLA_DIAGNOSTICS'/></OpenOptionList>");
    driver->pfnIdentify = JP2EmuellaDataset::Identify;
    driver->pfnOpen = JP2EmuellaDataset::Open;
    GetGDALDriverManager()->RegisterDriver(driver);
}

extern "C" CPL_DLL void GDALRegisterMe() { GDALRegister_JP2Emuella(); }

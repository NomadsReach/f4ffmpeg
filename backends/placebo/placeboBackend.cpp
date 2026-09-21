#include "placeboBackendApi.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>

extern "C"
{
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <libplacebo/colorspace.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

namespace
{
    const pl_render_params* renderParams(
        std::uint32_t quality)
    {
        switch (quality)
        {
            case 0:
                return &pl_render_fast_params;
            case 2:
                return &pl_render_high_quality_params;
            case 1:
            default:
                return &pl_render_default_params;
        }
    }

    void releaseOutput(
        f4ffmpeg_placebo_output& output)
    {
        if (output.resource_view != nullptr)
        {
            static_cast<ID3D11ShaderResourceView*>(
                output.resource_view
            )->Release();
        }

        if (output.texture != nullptr)
        {
            static_cast<ID3D11Texture2D*>(
                output.texture
            )->Release();
        }

        output = {};
    }

    bool createD3D11Output(
        ID3D11Device* device,
        int width,
        int height,
        const std::uint8_t* pixels,
        std::uint32_t pitch,
        f4ffmpeg_placebo_output& output)
    {
        if (
            device == nullptr ||
            width <= 0 ||
            height <= 0 ||
            pixels == nullptr ||
            pitch == 0)
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initialData{};
        initialData.pSysMem = pixels;
        initialData.SysMemPitch = pitch;

        ID3D11Texture2D* texture = nullptr;
        if (FAILED(device->CreateTexture2D(
                &desc,
                &initialData,
                &texture)))
        {
            return false;
        }

        ID3D11ShaderResourceView* view = nullptr;
        if (FAILED(device->CreateShaderResourceView(
                texture,
                nullptr,
                &view)))
        {
            texture->Release();
            return false;
        }

        output.texture = texture;
        output.resource_view = view;
        output.width = static_cast<std::uint32_t>(width);
        output.height = static_cast<std::uint32_t>(height);
        return true;
    }

    void lockQueue(
        void* opaque,
        std::uint32_t queueFamily,
        std::uint32_t queueIndex)
    {
        auto* deviceContext =
            static_cast<AVHWDeviceContext*>(opaque);

        if (
            deviceContext == nullptr ||
            deviceContext->hwctx == nullptr)
        {
            return;
        }

        auto* vulkanContext =
            static_cast<AVVulkanDeviceContext*>(
                deviceContext->hwctx
            );

        if (vulkanContext->lock_queue != nullptr)
        {
            vulkanContext->lock_queue(
                deviceContext,
                queueFamily,
                queueIndex
            );
        }
    }

    void unlockQueue(
        void* opaque,
        std::uint32_t queueFamily,
        std::uint32_t queueIndex)
    {
        auto* deviceContext =
            static_cast<AVHWDeviceContext*>(opaque);

        if (
            deviceContext == nullptr ||
            deviceContext->hwctx == nullptr)
        {
            return;
        }

        auto* vulkanContext =
            static_cast<AVVulkanDeviceContext*>(
                deviceContext->hwctx
            );

        if (vulkanContext->unlock_queue != nullptr)
        {
            vulkanContext->unlock_queue(
                deviceContext,
                queueFamily,
                queueIndex
            );
        }
    }

    struct frameVulkanDevice
    {
        AVBufferRef* deviceRef = nullptr;
        AVHWDeviceContext* avDevice = nullptr;
        AVVulkanDeviceContext* vulkan = nullptr;
    };

    bool extractVulkanDevice(
        const AVFrame& frame,
        frameVulkanDevice& output)
    {
        output = {};

        if (
            frame.format != AV_PIX_FMT_VULKAN ||
            frame.hw_frames_ctx == nullptr ||
            frame.hw_frames_ctx->data == nullptr)
        {
            return false;
        }

        auto* framesContext =
            reinterpret_cast<AVHWFramesContext*>(
                frame.hw_frames_ctx->data
            );

        if (
            framesContext == nullptr ||
            framesContext->device_ctx == nullptr ||
            framesContext->device_ctx->type !=
                AV_HWDEVICE_TYPE_VULKAN ||
            framesContext->device_ctx->hwctx == nullptr)
        {
            return false;
        }

        if (framesContext->device_ref == nullptr)
            return false;

        output.deviceRef = framesContext->device_ref;
        output.avDevice = framesContext->device_ctx;
        output.vulkan =
            static_cast<AVVulkanDeviceContext*>(
                framesContext->device_ctx->hwctx
            );

        return
            output.vulkan->inst != VK_NULL_HANDLE &&
            output.vulkan->phys_dev != VK_NULL_HANDLE &&
            output.vulkan->act_dev != VK_NULL_HANDLE &&
            output.vulkan->get_proc_addr != nullptr;
    }

    struct importedVulkanContext
    {
        AVBufferRef* deviceRef = nullptr;
        AVHWDeviceContext* avDevice = nullptr;
        VkDevice vkDevice = VK_NULL_HANDLE;
        pl_vulkan vulkan = nullptr;
        pl_renderer renderer = nullptr;
        pl_tex targetTexture = nullptr;
        pl_fmt rgba8Format = nullptr;
        bool failed = false;

        ~importedVulkanContext()
        {
            // The imported VkDevice is owned by FFmpeg. Destroy all libplacebo
            // children before dropping our AVHWDeviceContext reference so the
            // underlying VkDevice cannot disappear out from under them.
            if (renderer != nullptr)
                pl_renderer_destroy(&renderer);

            if (targetTexture != nullptr && vulkan != nullptr)
            {
                pl_tex_destroy(
                    vulkan->gpu,
                    &targetTexture
                );
            }

            if (vulkan != nullptr)
                pl_vulkan_destroy(&vulkan);

            av_buffer_unref(&deviceRef);
        }
    };

    class backendContext
    {
    public:
        std::int32_t convert(
            ID3D11Device* d3d11Device,
            const AVFrame& source,
            std::uint32_t quality,
            f4ffmpeg_placebo_output& output)
        {
            std::scoped_lock lock(mutex);
            output = {};

            if (permanentlyUnavailable)
                return F4FFMPEG_PLACEBO_UNAVAILABLE;

            frameVulkanDevice frameDevice{};
            if (!extractVulkanDevice(source, frameDevice))
                return F4FFMPEG_PLACEBO_FALLBACK;

            sweepUnusedDevices();

            auto* imported = findOrImport(frameDevice);
            if (imported == nullptr || imported->failed)
                return F4FFMPEG_PLACEBO_DEVICE_UNAVAILABLE;

            if (
                d3d11Device == nullptr ||
                source.width <= 0 ||
                source.height <= 0)
            {
                return F4FFMPEG_PLACEBO_FALLBACK;
            }

            if (!ensureTarget(
                    *imported,
                    source.width,
                    source.height))
            {
                return F4FFMPEG_PLACEBO_FALLBACK;
            }

            pl_frame image{};
            pl_avframe_params mapParams{};
            mapParams.frame = &source;
            // Vulkan hardware frames are wrapped directly. Backing upload
            // textures are only required for software AVFrames.
            mapParams.tex = nullptr;
            mapParams.map_dovi = true;

            if (!pl_map_avframe_ex(
                    imported->vulkan->gpu,
                    &image,
                    &mapParams))
            {
                return F4FFMPEG_PLACEBO_FALLBACK;
            }

            pl_frame target{};
            target.num_planes = 1;
            target.planes[0].texture = imported->targetTexture;
            target.planes[0].components = 4;
            target.planes[0].component_mapping[0] = 0;
            target.planes[0].component_mapping[1] = 1;
            target.planes[0].component_mapping[2] = 2;
            target.planes[0].component_mapping[3] = 3;
            target.repr = pl_color_repr_rgb;
            target.repr.levels = PL_COLOR_LEVELS_FULL;
            target.repr.alpha = PL_ALPHA_INDEPENDENT;
            target.color = pl_color_space_bt709;
            target.crop = {
                0.0f,
                0.0f,
                static_cast<float>(source.width),
                static_cast<float>(source.height)
            };

            const bool rendered =
                pl_render_image(
                    imported->renderer,
                    &image,
                    &target,
                    renderParams(quality)
                );

            pl_unmap_avframe(
                imported->vulkan->gpu,
                &image
            );

            if (!rendered)
                return F4FFMPEG_PLACEBO_FALLBACK;

            if (
                source.width >
                    std::numeric_limits<int>::max() / 4)
            {
                return F4FFMPEG_PLACEBO_FALLBACK;
            }

            const std::uint32_t pitch =
                static_cast<std::uint32_t>(
                    source.width * 4
                );

            const auto bufferSize =
                static_cast<std::size_t>(pitch) *
                static_cast<std::size_t>(source.height);

            if (
                source.height <= 0 ||
                bufferSize /
                    static_cast<std::size_t>(source.height) !=
                    static_cast<std::size_t>(pitch))
            {
                return F4FFMPEG_PLACEBO_FALLBACK;
            }

            std::vector<std::uint8_t> pixels(bufferSize);

            pl_tex_transfer_params download{};
            download.tex = imported->targetTexture;
            download.ptr = pixels.data();
            download.row_pitch = pitch;
            download.depth_pitch = bufferSize;

            // With no callback and host memory as the destination,
            // pl_tex_download is blocking. This gives CreateTexture2D stable
            // RGBA bytes and keeps the first implementation deliberately
            // simple. The eventual Win32 external-memory path can replace
            // only this readback/handoff section.
            if (!pl_tex_download(
                    imported->vulkan->gpu,
                    &download))
            {
                return F4FFMPEG_PLACEBO_FALLBACK;
            }

            if (!createD3D11Output(
                    d3d11Device,
                    source.width,
                    source.height,
                    pixels.data(),
                    pitch,
                    output))
            {
                releaseOutput(output);
                return F4FFMPEG_PLACEBO_FALLBACK;
            }

            return F4FFMPEG_PLACEBO_SUCCESS;
        }

    private:
        importedVulkanContext* findOrImport(
            const frameVulkanDevice& frameDevice)
        {
            for (auto& candidate : devices)
            {
                if (
                    candidate->avDevice == frameDevice.avDevice &&
                    candidate->vkDevice == frameDevice.vulkan->act_dev)
                {
                    return candidate.get();
                }
            }

            auto candidate =
                std::make_unique<importedVulkanContext>();
            candidate->deviceRef =
                av_buffer_ref(frameDevice.deviceRef);
            candidate->avDevice = frameDevice.avDevice;
            candidate->vkDevice = frameDevice.vulkan->act_dev;

            if (candidate->deviceRef == nullptr)
            {
                candidate->failed = true;
            }
            else if (!initializeImportedDevice(
                         *candidate,
                         frameDevice))
            {
                candidate->failed = true;
            }

            auto* result = candidate.get();
            devices.push_back(std::move(candidate));
            return result;
        }

        void sweepUnusedDevices()
        {
            // Hold only AVHWDeviceContext, not AVHWFramesContext, references.
            // This keeps imported VkDevices alive without pinning large decoder
            // frame pools. Once FFmpeg drops every other reference, our own
            // reference is the sole survivor and the libplacebo import can be
            // torn down safely.
            devices.erase(
                std::remove_if(
                    devices.begin(),
                    devices.end(),
                    [](const auto& candidate)
                    {
                        return
                            candidate->deviceRef != nullptr &&
                            av_buffer_get_ref_count(
                                candidate->deviceRef
                            ) == 1;
                    }
                ),
                devices.end()
            );
        }

        bool ensureLog()
        {
            if (log != nullptr)
                return true;

            pl_log_params logParams{};
            logParams.log_cb = pl_log_simple;
            logParams.log_level = PL_LOG_WARN;

            log = pl_log_create(
                PL_API_VER,
                &logParams
            );

            if (log == nullptr)
            {
                permanentlyUnavailable = true;
                return false;
            }

            return true;
        }

        bool initializeImportedDevice(
            importedVulkanContext& destination,
            const frameVulkanDevice& frameDevice)
        {
            if (!ensureLog())
                return false;

            const auto* hwctx = frameDevice.vulkan;

#if LIBAVUTIL_VERSION_MAJOR >= 61 && PL_API_VER < 365
            if (hwctx->queue_flags != 0)
                return false;
#endif

            pl_vulkan_import_params params{};
            params.instance = hwctx->inst;
            params.get_proc_addr = hwctx->get_proc_addr;
            params.phys_device = hwctx->phys_dev;
            params.device = hwctx->act_dev;
            params.extensions = hwctx->enabled_dev_extensions;
            params.num_extensions = hwctx->nb_enabled_dev_extensions;
            params.features = &hwctx->device_features;
            params.lock_queue = lockQueue;
            params.unlock_queue = unlockQueue;
            params.queue_ctx = frameDevice.avDevice;
            params.max_api_version = VK_API_VERSION_1_3;

            // FFmpeg 7.1 exposes the enabled queue families in preferential
            // order through qf[]. Avoid the deprecated fixed-queue fields and
            // select the first enabled family advertising each capability.
            for (int i = 0; i < hwctx->nb_qf; ++i)
            {
                const auto& queue = hwctx->qf[i];
                if (queue.idx < 0 || queue.num <= 0)
                    continue;

                const auto index =
                    static_cast<std::uint32_t>(queue.idx);
                const auto count =
                    static_cast<std::uint32_t>(queue.num);
                const auto flags =
                    static_cast<VkQueueFlags>(queue.flags);

                if (
                    params.queue_graphics.count == 0 &&
                    (flags & VK_QUEUE_GRAPHICS_BIT) != 0)
                {
                    params.queue_graphics.index = index;
                    params.queue_graphics.count = count;
                }

                if (
                    params.queue_compute.count == 0 &&
                    (flags & VK_QUEUE_COMPUTE_BIT) != 0)
                {
                    params.queue_compute.index = index;
                    params.queue_compute.count = count;
                }

                if (
                    params.queue_transfer.count == 0 &&
                    (flags & VK_QUEUE_TRANSFER_BIT) != 0)
                {
                    params.queue_transfer.index = index;
                    params.queue_transfer.count = count;
                }
            }

            if (params.queue_graphics.count == 0)
                return false;

            destination.vulkan =
                pl_vulkan_import(
                    log,
                    &params
                );

            if (destination.vulkan == nullptr)
                return false;

            destination.renderer =
                pl_renderer_create(
                    log,
                    destination.vulkan->gpu
                );

            if (destination.renderer == nullptr)
                return false;

            destination.rgba8Format =
                pl_find_fmt(
                    destination.vulkan->gpu,
                    PL_FMT_UNORM,
                    4,
                    8,
                    8,
                    static_cast<enum pl_fmt_caps>(
                        PL_FMT_CAP_RENDERABLE |
                        PL_FMT_CAP_HOST_READABLE
                    )
                );

            return destination.rgba8Format != nullptr;
        }

        bool ensureTarget(
            importedVulkanContext& context,
            int width,
            int height)
        {
            pl_tex_params params{};
            params.w = width;
            params.h = height;
            params.format = context.rgba8Format;
            params.renderable = true;
            params.host_readable = true;

            return pl_tex_recreate(
                context.vulkan->gpu,
                &context.targetTexture,
                &params
            );
        }

        std::mutex mutex;
        pl_log log = nullptr;
        std::vector<std::unique_ptr<importedVulkanContext>> devices;
        bool permanentlyUnavailable = false;
    };

    backendContext& context()
    {
        // Process-lifetime by design. Imported pl_vulkan objects do not own
        // FFmpeg's VkDevice, and avoiding late DLL/device teardown ordering is
        // safer inside Fallout. Each conversion is fully synchronized before
        // returning because the current handoff uses blocking readback.
        static auto* instance = new backendContext();
        return *instance;
    }
}

extern "C" __declspec(dllexport)
uint32_t __cdecl f4ffmpeg_placebo_backend_abi(void)
{
    return F4FFMPEG_PLACEBO_BACKEND_ABI;
}

extern "C" __declspec(dllexport)
int32_t __cdecl f4ffmpeg_placebo_convert(
    void* d3d11_device,
    const AVFrame* frame,
    uint32_t quality,
    f4ffmpeg_placebo_output* output)
{
    if (
        frame == nullptr ||
        output == nullptr)
    {
        return F4FFMPEG_PLACEBO_FALLBACK;
    }

    return context().convert(
        static_cast<ID3D11Device*>(d3d11_device),
        *frame,
        quality,
        *output
    );
}

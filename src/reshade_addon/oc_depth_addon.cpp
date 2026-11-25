// by MK-JH - https://t.me/MKJH_APPS
// OptiCaptureDepth: a ReShade add-on (put OptiCaptureDepth.addon64 next to the game's exe, with ReShade 6+ "with add-on support").
//
// It does not look for the depth buffer itself: it reuses ReShade's own depth detection through the small effect
// OptiCaptureDepth.fx (which writes the linearised depth, 0 = near, 1 = far, into the texture OC_LinearDepth). Every frame this
// add-on copies that texture into a shared texture (D3D11 / D3D12) or a CPU shared memory buffer (other APIs) and tells OptiCaptureHost.exe.
//
// ReShade's headers (BSD-3-Clause, Copyright (C) Patrick Mours) are used to build this file; see NOTICE.md.

#include <reshade.hpp>
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#include "../depth_share.h"

using namespace reshade::api;

namespace
{
std::mutex g_mutex;
device* g_device = nullptr;
resource g_shared = { 0 };
resource g_staging = { 0 };
void* g_handle = nullptr; // NT handle (kept open for as long as the texture lives)
uint32_t g_w = 0, g_h = 0;
HANDLE g_map = nullptr;
ocdepth::Share* g_info = nullptr;

HANDLE g_cpuMap = nullptr;
ocdepth::CpuHeader* g_cpuHdr = nullptr;
float* g_cpuPixels = nullptr;
size_t g_cpuCapBytes = 0;

bool g_useCpu = false;
bool g_failed = false; // sharing is not possible here: say so once and stop trying
bool g_loggedMissing = false;

uint32_t ApiCode(device_api api)
{
    switch (api)
    {
    case device_api::d3d12: return 12u;
    case device_api::d3d11: return 11u;
    case device_api::d3d10: return 10u;
    case device_api::d3d9:  return 9u;
    case device_api::opengl: return 100u;
    case device_api::vulkan: return 200u;
    default: return 0u;
    }
}

void Say(reshade::log::level level, const char* text)
{
    reshade::log::message(level, text);
}

bool OpenInfo()
{
    if (g_info)
        return true;
    char name[96];
    std::snprintf(name, sizeof(name), "Local\\OptiCaptureDepth_%lu", GetCurrentProcessId());
    g_map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ocdepth::Share), name);
    if (!g_map)
        return false;
    g_info = static_cast<ocdepth::Share*>(MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ocdepth::Share)));
    if (!g_info)
    {
        CloseHandle(g_map);
        g_map = nullptr;
        return false;
    }
    std::memset(g_info, 0, sizeof(ocdepth::Share));
    g_info->pid = GetCurrentProcessId();
    return true;
}

void DropCpuBuffer()
{
    if (g_staging.handle != 0 && g_device)
    {
        g_device->destroy_resource(g_staging);
        g_staging = { 0 };
    }
    if (g_cpuHdr)
    {
        UnmapViewOfFile(g_cpuHdr);
        g_cpuHdr = nullptr;
        g_cpuPixels = nullptr;
    }
    if (g_cpuMap)
    {
        CloseHandle(g_cpuMap);
        g_cpuMap = nullptr;
    }
    g_cpuCapBytes = 0;
}

bool EnsureCpuBuffer(device* dev, uint32_t w, uint32_t h)
{
    const size_t dataBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(float);
    const size_t totalBytes = sizeof(ocdepth::CpuHeader) + dataBytes;
    if (g_cpuHdr && g_staging.handle != 0 && g_cpuCapBytes >= totalBytes && g_w == w && g_h == h)
        return true;

    DropCpuBuffer();

    const resource_desc stDesc(w, h, 1, 1, format::r32_float, 1, memory_heap::gpu_to_cpu, resource_usage::copy_dest);
    if (!dev->create_resource(stDesc, nullptr, resource_usage::copy_dest, &g_staging) || g_staging.handle == 0)
    {
        g_staging = { 0 };
        return false;
    }

    char name[96];
    std::snprintf(name, sizeof(name), "Local\\OptiCaptureDepthCpu_%lu", GetCurrentProcessId());
    g_cpuMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(totalBytes >> 32), name);
    if (!g_cpuMap)
    {
        g_cpuMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(totalBytes & 0xFFFFFFFFULL), name);
    }
    if (!g_cpuMap)
    {
        dev->destroy_resource(g_staging);
        g_staging = { 0 };
        return false;
    }

    void* view = MapViewOfFile(g_cpuMap, FILE_MAP_ALL_ACCESS, 0, 0, totalBytes);
    if (!view)
    {
        CloseHandle(g_cpuMap);
        g_cpuMap = nullptr;
        dev->destroy_resource(g_staging);
        g_staging = { 0 };
        return false;
    }

    g_cpuHdr = static_cast<ocdepth::CpuHeader*>(view);
    g_cpuPixels = reinterpret_cast<float*>(static_cast<uint8_t*>(view) + sizeof(ocdepth::CpuHeader));
    g_cpuCapBytes = totalBytes;
    g_w = w;
    g_h = h;

    g_cpuHdr->magic = ocdepth::kMagic;
    g_cpuHdr->version = ocdepth::kVersion;
    g_cpuHdr->width = w;
    g_cpuHdr->height = h;
    g_cpuHdr->rowPitchBytes = w * static_cast<uint32_t>(sizeof(float));
    g_cpuHdr->reserved = 0;
    g_cpuHdr->frame = 0;
    return true;
}

void DropShared(effect_runtime* runtime)
{
    if (g_info)
    {
        g_info->handle = 0; // the host stops using it before it goes away
        g_info->transport = static_cast<uint32_t>(ocdepth::Transport::None);
        g_info->magic = 0;
    }
    if (g_shared.handle != 0 && g_device)
    {
        if (runtime && runtime->get_command_queue())
            runtime->get_command_queue()->wait_idle();
        g_device->destroy_resource(g_shared);
    }
    g_shared = { 0 };
    if (g_handle)
    {
        CloseHandle(static_cast<HANDLE>(g_handle));
        g_handle = nullptr;
    }
    DropCpuBuffer();
    g_w = g_h = 0;
    g_useCpu = false;
}

bool MakeShared(effect_runtime* runtime, device* dev, uint32_t w, uint32_t h)
{
    DropShared(runtime);

    if (!OpenInfo())
    {
        Say(reshade::log::level::error, "OptiCaptureDepth: could not create the memory block for the host.");
        g_failed = true;
        return false;
    }

    if (dev->check_capability(device_caps::shared_resource_nt_handle))
    {
        const resource_desc desc(w, h, 1, 1, format::r32_float, 1, memory_heap::default_,
                                 resource_usage::copy_dest | resource_usage::shader_resource,
                                 resource_flags::shared | resource_flags::shared_nt_handle);
        void* handle = nullptr;
        if (dev->create_resource(desc, nullptr, resource_usage::general, &g_shared, &handle) && handle != nullptr)
        {
            g_handle = handle;
            g_w = w;
            g_h = h;
            g_useCpu = false;

            g_info->version = ocdepth::kVersion;
            g_info->handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
            g_info->width = w;
            g_info->height = h;
            g_info->dxgiFormat = 41; // DXGI_FORMAT_R32_FLOAT
            g_info->api = ApiCode(dev->get_api());
            g_info->transport = static_cast<uint32_t>(ocdepth::Transport::GpuShared);
            g_info->magic = ocdepth::kMagic;

            char text[160];
            std::snprintf(text, sizeof(text), "OptiCaptureDepth: sharing GPU depth texture (%ux%u).", w, h);
            Say(reshade::log::level::info, text);
            return true;
        }
    }

    // Fallback: CPU shared memory for APIs without NT shared texture handles (D3D9, D3D10, OpenGL, Vulkan).
    if (!EnsureCpuBuffer(dev, w, h))
    {
        Say(reshade::log::level::error, "OptiCaptureDepth: could not create CPU shared depth buffer.");
        g_failed = true;
        return false;
    }

    g_useCpu = true;
    g_info->version = ocdepth::kVersion;
    g_info->handle = 0;
    g_info->width = w;
    g_info->height = h;
    g_info->dxgiFormat = 41; // DXGI_FORMAT_R32_FLOAT
    g_info->api = ApiCode(dev->get_api());
    g_info->transport = static_cast<uint32_t>(ocdepth::Transport::CpuBuffer);
    g_info->magic = ocdepth::kMagic;

    char text[180];
    std::snprintf(text, sizeof(text), "OptiCaptureDepth: sharing depth via CPU buffer (%ux%u, api=%u).", w, h, g_info->api);
    Say(reshade::log::level::info, text);
    return true;
}

// After ReShade has run the effects of a frame: OC_LinearDepth now holds this frame's depth.
void OnFinishEffects(effect_runtime* runtime, command_list* cmd_list, resource_view, resource_view)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_failed)
        return;

    const effect_texture_variable var = runtime->find_texture_variable("OptiCaptureDepth.fx", "OC_LinearDepth");
    if (var == 0)
    {
        if (!g_loggedMissing)
        {
            Say(reshade::log::level::info, "OptiCaptureDepth: waiting for the effect OptiCaptureDepth.fx (copy it into ReShade's Shaders folder and tick it on in ReShade's menu).");
            g_loggedMissing = true;
        }
        return;
    }
    g_loggedMissing = false;

    resource_view srv = { 0 };
    resource_view srvSrgb = { 0 };
    runtime->get_texture_binding(var, &srv, &srvSrgb);
    if (srv == 0)
        return;

    device* const dev = runtime->get_device();
    const resource src = dev->get_resource_from_view(srv);
    if (src == 0)
        return;
    const resource_desc sd = dev->get_resource_desc(src);
    if (sd.type != resource_type::texture_2d || sd.texture.width == 0 || sd.texture.height == 0)
        return;

    g_device = dev;
    if ((!g_useCpu && (g_shared == 0 || g_w != sd.texture.width || g_h != sd.texture.height)) ||
        (g_useCpu && (!g_cpuHdr || g_staging.handle == 0 || g_w != sd.texture.width || g_h != sd.texture.height)))
    {
        if (!MakeShared(runtime, dev, sd.texture.width, sd.texture.height))
            return;
    }

    if (!g_useCpu)
    {
        cmd_list->barrier(src, resource_usage::shader_resource, resource_usage::copy_source);
        cmd_list->barrier(g_shared, resource_usage::general, resource_usage::copy_dest);
        cmd_list->copy_resource(src, g_shared);
        cmd_list->barrier(g_shared, resource_usage::copy_dest, resource_usage::general);
        cmd_list->barrier(src, resource_usage::copy_source, resource_usage::shader_resource);

        if (runtime->get_command_queue())
            runtime->get_command_queue()->flush_immediate_command_list();

        if (g_info)
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_info->frame));
    }
    else if (g_cpuHdr && g_cpuPixels && g_staging.handle != 0)
    {
        cmd_list->barrier(src, resource_usage::shader_resource, resource_usage::copy_source);
        cmd_list->barrier(g_staging, resource_usage::copy_dest, resource_usage::copy_dest);
        cmd_list->copy_resource(src, g_staging);
        cmd_list->barrier(src, resource_usage::copy_source, resource_usage::shader_resource);

        if (runtime->get_command_queue())
            runtime->get_command_queue()->flush_immediate_command_list();

        subresource_data mapped{};
        if (dev->map_texture_region(g_staging, 0, nullptr, map_access::read_only, &mapped))
        {
            const uint32_t w = sd.texture.width;
            const uint32_t h = sd.texture.height;
            const size_t dstRowBytes = static_cast<size_t>(w) * sizeof(float);
            const size_t srcPitchBytes = mapped.row_pitch >= dstRowBytes ? static_cast<size_t>(mapped.row_pitch) : dstRowBytes;
            const auto* srcBytes = static_cast<const uint8_t*>(mapped.data);
            auto* dstBytes = reinterpret_cast<uint8_t*>(g_cpuPixels);
            for (uint32_t y = 0; y < h; ++y)
                std::memcpy(dstBytes + static_cast<size_t>(y) * dstRowBytes, srcBytes + static_cast<size_t>(y) * srcPitchBytes, dstRowBytes);
            dev->unmap_texture_region(g_staging, 0);

            g_cpuHdr->width = w;
            g_cpuHdr->height = h;
            g_cpuHdr->rowPitchBytes = static_cast<uint32_t>(dstRowBytes);
            InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_cpuHdr->frame));
            if (g_info)
                InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_info->frame));
        }
    }
}

void OnDestroyDevice(device*)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    DropShared(nullptr);
    g_device = nullptr;
    g_failed = false;
}
} // namespace

extern "C" __declspec(dllexport) const char* NAME = "OptiCaptureDepth";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Shares the game's depth (GPU shared handle on D3D11/12, or CPU shared memory on other APIs) with OptiCaptureHost. by MK-JH";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        ::reshade::register_event< ::reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
        ::reshade::register_event< ::reshade::addon_event::destroy_device>(OnDestroyDevice);
        break;
    case DLL_PROCESS_DETACH:
        ::reshade::unregister_event< ::reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
        ::reshade::unregister_event< ::reshade::addon_event::destroy_device>(OnDestroyDevice);
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}

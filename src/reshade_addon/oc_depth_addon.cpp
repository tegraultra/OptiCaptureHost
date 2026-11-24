// by MK-JH - https://t.me/MKJH_APPS
// OptiCaptureDepth: a ReShade add-on (put OptiCaptureDepth.addon64 next to the game's exe, with ReShade 6+ "with add-on support").
//
// It does not look for the depth buffer itself: it reuses ReShade's own depth detection through the small effect
// OptiCaptureDepth.fx (which writes the linearised depth, 0 = near, 1 = far, into the texture OC_LinearDepth). Every frame this
// add-on copies that texture into a shared texture and tells OptiCaptureHost.exe where it is. Direct3D 11 and 12 games only.
//
// ReShade's headers (BSD-3-Clause, Copyright (C) Patrick Mours) are used to build this file; see NOTICE.md.

#include <reshade.hpp>
#include <Windows.h>

#include <cstdio>
#include <mutex>

#include "../depth_share.h"

using namespace reshade::api;

namespace
{
std::mutex g_mutex;
device* g_device = nullptr;
resource g_shared = { 0 };
void* g_handle = nullptr; // NT handle (kept open for as long as the texture lives)
uint32_t g_w = 0, g_h = 0;
HANDLE g_map = nullptr;
ocdepth::Share* g_info = nullptr;
bool g_failed = false; // sharing is not possible here: say so once and stop trying
bool g_loggedMissing = false;

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
    g_info->pid = GetCurrentProcessId();
    return true;
}

void DropShared(effect_runtime* runtime)
{
    if (g_info)
    {
        g_info->handle = 0; // the host stops using it before it goes away
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
    g_w = g_h = 0;
}

bool MakeShared(effect_runtime* runtime, device* dev, uint32_t w, uint32_t h)
{
    DropShared(runtime);
    if (!dev->check_capability(device_caps::shared_resource_nt_handle))
    {
        Say(reshade::log::level::warning, "OptiCaptureDepth: this graphics API cannot share textures with other processes (Direct3D 11 and 12 only).");
        g_failed = true;
        return false;
    }

    const resource_desc desc(w, h, 1, 1, format::r32_float, 1, memory_heap::default_,
                             resource_usage::copy_dest | resource_usage::shader_resource,
                             resource_flags::shared | resource_flags::shared_nt_handle);
    void* handle = nullptr; // null: ReShade creates the texture and returns its shared handle
    if (!dev->create_resource(desc, nullptr, resource_usage::general, &g_shared, &handle) || handle == nullptr)
    {
        Say(reshade::log::level::error, "OptiCaptureDepth: could not create the shared depth texture.");
        g_failed = true;
        g_shared = { 0 };
        return false;
    }
    g_handle = handle;
    g_w = w;
    g_h = h;

    if (!OpenInfo())
    {
        Say(reshade::log::level::error, "OptiCaptureDepth: could not create the memory block for the host.");
        g_failed = true;
        return false;
    }
    g_info->version = ocdepth::kVersion;
    g_info->handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    g_info->width = w;
    g_info->height = h;
    g_info->dxgiFormat = 41; // DXGI_FORMAT_R32_FLOAT
    g_info->api = dev->get_api() == device_api::d3d12 ? 12u : 11u;
    g_info->magic = ocdepth::kMagic;

    char text[160];
    std::snprintf(text, sizeof(text), "OptiCaptureDepth: sharing the depth texture with the host (%ux%u).", w, h);
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
    if (g_shared == 0 || g_w != sd.texture.width || g_h != sd.texture.height)
    {
        if (!MakeShared(runtime, dev, sd.texture.width, sd.texture.height))
            return;
    }

    cmd_list->barrier(src, resource_usage::shader_resource, resource_usage::copy_source);
    cmd_list->barrier(g_shared, resource_usage::general, resource_usage::copy_dest);
    cmd_list->copy_resource(src, g_shared);
    cmd_list->barrier(g_shared, resource_usage::copy_dest, resource_usage::general);
    cmd_list->barrier(src, resource_usage::copy_source, resource_usage::shader_resource);

    // Direct3D 11: other devices only see the new picture once the commands were really sent.
    if (runtime->get_command_queue())
        runtime->get_command_queue()->flush_immediate_command_list();

    if (g_info)
        InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&g_info->frame));
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
    "Shares the game's depth (through ReShade's depth detection and the effect OptiCaptureDepth.fx) with OptiCaptureHost. by MK-JH";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<addon_event::reshade_finish_effects>(OnFinishEffects);
        reshade::register_event<addon_event::destroy_device>(OnDestroyDevice);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_event<addon_event::reshade_finish_effects>(OnFinishEffects);
        reshade::unregister_event<addon_event::destroy_device>(OnDestroyDevice);
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}

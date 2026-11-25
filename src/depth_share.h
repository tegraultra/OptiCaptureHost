// by MK-JH - https://t.me/MKJH_APPS
// Shared between the ReShade depth add-on (runs inside the game) and OptiCaptureHost.exe: what the add-on tells the host
// through a small named memory block, "Local\OptiCaptureDepth_<process id of the game>"
// or via CPU shared memory "Local\OptiCaptureDepthCpu_<process id of the game>" for APIs without NT shared texture handles.
#pragma once

#include <cstdint>

namespace ocdepth
{
constexpr uint32_t kMagic = 0x3044434Fu; // "OCD0"
constexpr uint32_t kVersion = 2;

enum class Transport : uint32_t
{
    None = 0,
    GpuShared = 1,
    CpuBuffer = 2
};

struct Share
{
    uint32_t magic;
    uint32_t version;
    uint64_t handle;     // NT handle of the shared depth texture (GpuShared), or 0 when using CpuBuffer
    uint32_t width;
    uint32_t height;
    uint32_t dxgiFormat; // DXGI_FORMAT_R32_FLOAT (41): linear depth, 0 = near, 1 = far
    uint32_t api;        // 11, 12, 9, 10, 100 (OpenGL), 200 (Vulkan), etc., for the log only
    uint32_t pid;
    uint32_t transport;  // ocdepth::Transport
    volatile uint64_t frame; // counts up every time the add-on copied a new depth picture
};

struct CpuHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t rowPitchBytes; // width * sizeof(float)
    uint32_t reserved;
    volatile uint64_t frame;
};
} // namespace ocdepth

// by MK-JH - https://t.me/MKJH_APPS
// Shared between the ReShade depth add-on (runs inside the game) and OptiCaptureHost.exe: what the add-on tells the host
// through a small named memory block, "Local\OptiCaptureDepth_<process id of the game>".
#pragma once

#include <cstdint>

namespace ocdepth
{
constexpr uint32_t kMagic = 0x3044434Fu; // "OCD0"
constexpr uint32_t kVersion = 1;

struct Share
{
    uint32_t magic;
    uint32_t version;
    uint64_t handle;     // NT handle of the shared depth texture, valid inside the GAME process (the host duplicates it)
    uint32_t width;
    uint32_t height;
    uint32_t dxgiFormat; // DXGI_FORMAT_R32_FLOAT (41): linear depth, 0 = near, 1 = far
    uint32_t api;        // 11 or 12 (the game's graphics API), for the log only
    uint32_t pid;
    uint32_t reserved;
    volatile uint64_t frame; // counts up every time the add-on copied a new depth picture
};
} // namespace ocdepth

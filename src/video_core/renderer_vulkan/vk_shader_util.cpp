// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {
// Huawei's shader compiler crashes on some SPIR-V; keep each module on disk so the culprit
// can be identified after a crash.
void DumpSpirvForDiagnostics(std::span<const u32> code) {
    static std::atomic<u32> counter{0};
    try {
        const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "spirv";
        std::filesystem::create_directories(dir);
        const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(code.data()),
                                            code.size_bytes());
        const auto name = std::format("{:04}_{:016x}.spv", counter++, hash);
        std::ofstream file(dir / name, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(code.data()),
                   static_cast<std::streamsize>(code.size_bytes()));
        file.close();
        LOG_WARNING(Render_Vulkan, "Compiling SPIR-V module {}", name);
    } catch (...) {
    }
}
} // Anonymous namespace

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code) {
    if (device.IsHuawei()) {
        DumpSpirvForDiagnostics(code);
    }
    return device.GetLogical().CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
}

} // namespace Vulkan

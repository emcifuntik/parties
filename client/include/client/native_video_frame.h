#pragma once

#include <cstdint>
#include <memory>

struct NativeVideoFrame {
    void* resource = nullptr;
    void* chroma_resource = nullptr;
    std::shared_ptr<void> owner;
    void* ready_fence = nullptr;
    uint64_t ready_fence_value = 0;
    uint32_t resource_state = 0;
    bool rgba = false;
    uint32_t visible_width = 0;
    uint32_t visible_height = 0;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
};

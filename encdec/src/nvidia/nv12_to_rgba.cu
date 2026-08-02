#include <cuda_runtime.h>

extern "C" __global__ void parties_nv12_to_rgba(
    cudaSurfaceObject_t destination,
    const unsigned char* source,
    unsigned int source_pitch,
    unsigned int width,
    unsigned int height) {
    const unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;

    const int luma = static_cast<int>(source[static_cast<size_t>(y) * source_pitch + x]);
    const unsigned char* chroma = source + static_cast<size_t>(source_pitch) * height;
    const size_t chroma_offset = static_cast<size_t>(y / 2) * source_pitch + (x & ~1u);
    const int u = static_cast<int>(chroma[chroma_offset]) - 128;
    const int v = static_cast<int>(chroma[chroma_offset + 1]) - 128;
    const int c = max(0, luma - 16);

    uchar4 rgba;
    rgba.x = static_cast<unsigned char>(min(255, max(0, (298 * c + 409 * v + 128) >> 8)));
    rgba.y = static_cast<unsigned char>(min(255, max(0, (298 * c - 100 * u - 208 * v + 128) >> 8)));
    rgba.z = static_cast<unsigned char>(min(255, max(0, (298 * c + 516 * u + 128) >> 8)));
    rgba.w = 255;
    surf2Dwrite(rgba, destination, x * sizeof(uchar4), y);
}

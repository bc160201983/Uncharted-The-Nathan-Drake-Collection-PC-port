// Decoder unit tests use host frame buffers and have no guest memory or GPU cache.
#include <cstdlib>
#include "core/memory.h"

extern "C" {
volatile unsigned long long runtime_video_frame_lo = 0, runtime_video_frame_hi = 0;
void* runtime_map_guest_memory(std::uint64_t) { std::abort(); }
}

void Core::MemoryManager::InvalidateMemory(VAddr, u64) { std::abort(); }
bool Core::MemoryManager::TryWriteBacking(void*, const void*, u64) { std::abort(); }

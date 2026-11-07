#pragma once
// Host stand-in for ESP-IDF's capability allocator: PSRAM and internal RAM are
// both the host heap, with generous free-size figures.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

constexpr uint32_t MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_SPIRAM = 2, MALLOC_CAP_8BIT = 4;
inline void* heap_caps_malloc(size_t size, uint32_t) { return std::malloc(size); }
inline void* heap_caps_calloc(size_t count, size_t size, uint32_t) { return std::calloc(count, size); }
inline void* heap_caps_realloc(void* ptr, size_t size, uint32_t) { return std::realloc(ptr, size); }
inline void heap_caps_free(void* ptr) { std::free(ptr); }
inline size_t heap_caps_get_free_size(uint32_t) { return 4 * 1024 * 1024; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 4 * 1024 * 1024; }

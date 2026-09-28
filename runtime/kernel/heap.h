#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>

class GuestHeap
{
public:
    void Init();

    void* Alloc(size_t size, size_t alignment = 16);
    void* AllocVirtual(size_t size, bool largePages);
    uint32_t ReserveVirtualAt(uint32_t base, size_t size);
    void* AllocPhysical(size_t size, size_t alignment = 0, bool topDown = false);
    void* AllocKernelStack(size_t size, size_t alignment);

    void Free(void* ptr);
    size_t Size(void* ptr);
    bool QueryRegion(uint32_t address, uint32_t& regionBase, uint32_t& regionSize);

private:
    struct Arena
    {
        std::map<uint32_t, uint32_t> free;
        std::unordered_map<uint32_t, uint32_t> used;
    };

    std::mutex mutex_;
    Arena user_;
    Arena physical_;
    Arena smallVirtual_;
    Arena largeVirtual_;
    Arena kernelStack_;
    bool initialized_ = false;
};

extern GuestHeap g_guestHeap;

#pragma once
#include "ThreadCache.h"

namespace MyMemoryPool 
{

class MemoryPool
{
public:
    static void* allocate(size_t size)
    {
        return ThreadCache::getInstance()->allocate(size);
    }

    static void deallocate(void* ptr)
    {
        ThreadCache::getInstance()->deallocate(ptr);
    }
};

} // namespace memoryPool
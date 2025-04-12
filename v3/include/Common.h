#pragma once
#include <cstddef>
#include <atomic>
#include <array>

namespace MyMemoryPool  
{
// 对齐数和大小定义
constexpr size_t ALIGNMENT = 8;  //对齐数，ALIGNMENT等于指针void*的大小
constexpr size_t MAX_BYTES = 256 * 1024; // 256KB
constexpr size_t FREE_LIST_SIZE = MAX_BYTES / ALIGNMENT; //内存链表的长度

// 内存块头部信息
struct BlockHeader
{
    size_t size; // 内存块大小
    bool   inUse; // 使用标志
    BlockHeader* next; // 指向下一个内存块
};

// 大小类管理
class SizeUtil 
{
public:
    static size_t roundUp(size_t bytes)
    {
        return (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    static size_t getIndex(size_t bytes)
    {   
        // 确保bytes至少为ALIGNMENT
        bytes = std::max(bytes, ALIGNMENT);
        // 向上取整
        return (bytes + ALIGNMENT - 1) / ALIGNMENT - 1;
    }
};

} // namespace memoryPool
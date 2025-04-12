#include "../include/ThreadCache.h"
#include "../include/CentralCache.h"
#include <cassert>

namespace MyMemoryPool 
{

void* ThreadCache::allocate(size_t size)
{
    void* ptr = nullptr;
    // 处理0大小的分配请求
    if (size == 0)
    {
        size = ALIGNMENT; // 至少分配一个对齐大小
    }
    
    if (size > MAX_BYTES)
    {
        // 大对象直接从系统分配
        ptr = malloc(size);
        memSize[ptr] = MAX_BYTES+1;
        assert(ptr!=nullptr);
        return ptr;
    }

    size_t index = SizeUtil::getIndex(size); //根据链表中某一元素的index越大，其指向的内存块越大

    // 更新自由链表大小
    freeListSize_[index]--;

    // 检查线程本地自由链表
    // 如果 freeList_[index] 不为空，表示该链表中有可用内存块
    
    if (ptr = freeList_[index])
    {
        freeList_[index] = *reinterpret_cast<void**>(ptr); // 将freeList_[index]指向内存块(ptr)的下一个内存块地址（取决于内存块的实现）
    } else{
        // 如果线程本地自由链表为空，则从中心缓存获取一批内存
        ptr = fetchFromCentralCache(index);
    } 
    assert(ptr != nullptr);
    memSize[ptr] = size;
    return ptr;
}

void ThreadCache::deallocate(void* ptr)
{
    assert(ptr!=nullptr);
    size_t size = memSize[ptr];
    //大内存是malloc分配的
    if (size > MAX_BYTES)
    {
        free(ptr);
        memSize.erase(ptr);
        return;
    }

    size_t index = SizeUtil::getIndex(size);
    
    *reinterpret_cast<void**>(ptr) = freeList_[index]; //!修改ptr本身指向的地址 //将ptr空间放在表头，并将next设置为目前空闲链表表头
    freeList_[index] = ptr;
    memSize.erase(ptr);
     // 更新自由链表大小
    freeListSize_[index]++; // 增加对应大小类的自由链表大小

    // 判断是否需要将部分内存回收给中心缓存
    if (shouldReturnToCentralCache(index))
    {
        returnToCentralCache(freeList_[index], size);
    }
}

// 判断是否需要将内存回收给中心缓存
bool ThreadCache::shouldReturnToCentralCache(size_t index)
{
    // 设定阈值，例如：当自由链表的大小超过一定数量时
    size_t threshold = 64; // 例如，64个内存块
    return (freeListSize_[index] > threshold);
}

void* ThreadCache::fetchFromCentralCache(size_t index)
{
    size_t size = (index + 1) * ALIGNMENT; //待分配的内存大小
    // 根据对象内存大小计算批量获取的数量
    size_t batchNum = getBatchNum(size);
    // 从中心缓存批量获取内存，用于扩充空闲列表的空间
    size_t actualBatchNum = 1;
    void* start = CentralCache::getInstance().fetchRange(index, batchNum, actualBatchNum);
    if (!start) return nullptr;

    // 更新自由链表大小
    freeListSize_[index] += actualBatchNum; // 增加对应大小类的自由链表大小

    // 取一个返回，其余放入线程本地自由链表
    void* result = start;
    if (actualBatchNum > 1)
    {
        freeList_[index] = *reinterpret_cast<void**>(start); //start指向的下一个内存块
    }
    
    return result;
}

void ThreadCache::returnToCentralCache(void* start, size_t size)
{
    // 根据大小计算对应的索引
    size_t index = SizeUtil::getIndex(size);

    // 获取对齐后的实际块大小
    size_t alignedSize = SizeUtil::roundUp(size);

    // 计算要归还内存块数量
    size_t batchNum = freeListSize_[index];
    // if (batchNum <= 1) return; // 如果只有一个块，则不归还
    assert(batchNum>1); //!前面shouldReturnToCentralCache()已保证batchNum大于一个阈值

    // 保留一部分在ThreadCache中（比如保留1/4）
    size_t keepNum = std::max(batchNum / 4, size_t(1));
    size_t returnNum = batchNum - keepNum;

    // 将内存块串成链表
    char* current = static_cast<char*>(start);
    // 使用对齐后的大小计算分割点
    char* splitNode = current;
    for (size_t i = 0; i < keepNum - 1; ++i) 
    {
        assert(splitNode);
        splitNode = reinterpret_cast<char*>(*reinterpret_cast<void**>(splitNode)); //找到下一个内存块
    }

    if (splitNode != nullptr) 
    {
        // 将要返回的部分和要保留的部分断开
        void* nextNode = *reinterpret_cast<void**>(splitNode);
        *reinterpret_cast<void**>(splitNode) = nullptr; //断开连接

        // 更新ThreadCache的空闲链表
        freeList_[index] = start;

        // 更新自由链表大小
        freeListSize_[index] = keepNum;

        // 将剩余部分返回给CentralCache
        if (returnNum > 0 && nextNode != nullptr)
        {
            CentralCache::getInstance().returnRange(nextNode, returnNum * alignedSize, index); 
        }
    }
}

// 计算批量获取内存块的数量
size_t ThreadCache::getBatchNum(size_t size)
{
    // 基准：每次批量获取不超过4KB内存
    constexpr size_t MAX_BATCH_SIZE = 4 * 1024; // 4KB

    // 根据对象大小设置合理的基准批量数
    size_t baseNum;
    if (size <= 32) baseNum = 64;    // 64 * 32 = 2KB
    else if (size <= 64) baseNum = 32;  // 32 * 64 = 2KB
    else if (size <= 128) baseNum = 16; // 16 * 128 = 2KB
    else if (size <= 256) baseNum = 8;  // 8 * 256 = 2KB
    else if (size <= 512) baseNum = 4;  // 4 * 512 = 2KB
    else if (size <= 1024) baseNum = 2; // 2 * 1024 = 2KB
    else baseNum = 1;                   // 大于1024的对象每次只从中心缓存取1个

    // 取最小值，但确保至少返回1
    return std::max((size_t)1, baseNum);
}

} // namespace memoryPool
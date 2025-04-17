#pragma once
#include "Common.h"
#include <map>
#include <mutex>
#include <unistd.h>
#include <vector>

namespace MyMemoryPool 
{

class PageCache
{
public:
    static const size_t PAGE_SIZE = 4096; // 4K页大小（根据实际系统来定的）

    static PageCache& getInstance()
    {
        return instance;
    }

    void returnPageBatchVector(std::vector<void *> returnPages);

    // 分配指定页数的span，一整个批量的blockBatch
    void* allocateSpan(size_t numPages);

    // 释放span，释放ptr指定的blockBatch到freeSpan_
    void deallocateSpan(void *ptr);

private:
    static PageCache instance;

    PageCache() = default;
    
    // 向系统申请内存
    void* systemAlloc(size_t numPages);
private:
    struct Span
    {
        void*  pageAddr; // 页起始地址
        size_t numPages; // 页数
        size_t totalPages; //分配时的总页数
        Span*  next;     // 空闲链表中的下一Span
        Span* pre;       // 空闲链表中的上一Span
        Span* nextSplit; // 在分配的总页批量切割出来的小块中，下一小块的地址
        Span* preSplit; // 在分配的总页批量切割出来的小块中，上一小块的地址
    };

    void detachFreeSpan(Span* span);

    void addFreeSpan(Span* span);

    // 按页数管理空闲span，不同页数对应不同Span链表
    std::map<size_t, Span*> freeSpans_; //! 映射key至多只有256KB/4KB=56个，不会浪费过多的复杂度
    // 页号到span的映射，用于回收
    std::map<void*, Span*> allocatedSpan_; //只记录已经分配给中心缓存的空间，key是内存分块的起始地址，val是该内存分块对应的Span
    // std::map<void*, bool> spanAllocated; //记录内存块是否已分配
    std::mutex mutex_;
};

} // namespace memoryPool
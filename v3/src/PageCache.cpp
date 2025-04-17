#include "PageCache.h"
#include <sys/mman.h>
#include <cstring>
#include <cassert>

namespace MyMemoryPool 
{
//TODO 回收被中心缓存退回的若干页批量
void PageCache::returnPageBatchVector(std::vector<void*> returnPages)
{
    for(auto pageBatch:returnPages)
    {
        deallocateSpan(pageBatch);
    }
    returnPages.clear();
}

//TODO2 添加释放整个内存池的方法
void* PageCache::allocateSpan(size_t numPages)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找合适的空闲span
    // lower_bound函数返回第一个大于等于numPages的元素的迭代器
    auto it = freeSpans_.lower_bound(numPages);
    if (it != freeSpans_.end())
    {
        Span* span = it->second;

        // 将取出的span从原有的空闲链表freeSpans_[it->first]中移除
        if (span->next)
        {
            span->next->pre = nullptr;
            freeSpans_[it->first] = span->next;
        }
        else
        {
            freeSpans_.erase(it);
        }

        // 如果span大于需要的numPages则进行分割，避免浪费
        if (span->numPages > numPages) 
        {
            Span* newSpan = new Span;
            newSpan->pageAddr = static_cast<char*>(span->pageAddr) + 
                                numPages * PAGE_SIZE; //页的起始地址
            newSpan->numPages = span->numPages - numPages;  //剩余页数
            newSpan->totalPages = span->totalPages;
            newSpan->nextSplit = span->nextSplit;
            newSpan->pre = nullptr;
            // 将超出部分放回空闲Span*列表头部
            Span* currentFront = freeSpans_[newSpan->numPages]; 
            newSpan->next = currentFront;
            currentFront->pre = newSpan;
            freeSpans_[newSpan->numPages] = newSpan;

            span->nextSplit = newSpan;
            newSpan->preSplit = span;
            span->numPages = numPages;
            span->next = nullptr;
        }

        // 记录span信息用于回收
        allocatedSpan_[span->pageAddr] = span;
        return span->pageAddr;
    }

    // 没有合适的span，向系统申请
    void* memory = systemAlloc(numPages); //申请内存
    if (!memory) return nullptr;

    // 创建新的span
    Span* span = new Span;
    span->pageAddr = memory;
    span->totalPages = numPages;
    span->numPages = numPages;
    span->next = nullptr;
    span->pre = nullptr;
    span->nextSplit = nullptr;
    span->preSplit = nullptr;
    // 记录span信息用于回收
    allocatedSpan_[memory] = span; //! 把页批量全部分配出去
    return memory;
}

void PageCache::deallocateSpan(void* ptr)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找对应的span，没找到代表不是PageCache分配的内存，则进行报错
    // 防护机制
    auto it = allocatedSpan_.find(ptr);
    assert(it != allocatedSpan_.end());
    size_t numPages = it->second->numPages;
    Span* span = it->second;

    // 尝试合并相邻的span（在下一碎片是空闲的情况下）
    void* nextAddr = span->nextSplit->pageAddr;
    void* preAddr = span->preSplit->pageAddr;

    if(nextAddr != nullptr&&allocatedSpan_.count(nextAddr)==0)
    {
        //一定在空闲列表中
        Span* nextSpan = span->nextSplit;

        detachFreeSpan(nextSpan);
        detachFreeSpan(span);

        //合并span+nextspan到span中
        span->numPages += nextSpan->numPages;
        span->nextSplit = nextSpan->nextSplit;
        span->nextSplit->preSplit = span;
        span->next = nextSpan->next;
        span->next->pre = span;
        delete nextSpan;    
        addFreeSpan(span);
    }
    if(preAddr != nullptr&&allocatedSpan_.count(preAddr)==0)
    {
        Span* preSpan = span->preSplit;

        detachFreeSpan(preSpan);
        detachFreeSpan(span);

        //合并preSpan和span为span
        span->numPages += preSpan->numPages;
        span->preSplit = preSpan->preSplit;
        span->preSplit->nextSplit = span;
        span->pre = preSpan->pre;
        span->pre->next = span;
        delete preSpan;
        addFreeSpan(span);
    }

    // 将合并后的span通过头插法插入空闲列表
    auto& list = freeSpans_[span->numPages];
    span->next = list;
    list = span;
}

//! 只改变next和pre指向关系，方便后续处理
void PageCache::detachFreeSpan(Span* span)
{
    //将合并前的块从freeSpans_中删除
    size_t pageNum = span->numPages;
    Span* currentSpan = freeSpans_[pageNum];
    assert(currentSpan!=nullptr);
    if(span==currentSpan) freeSpans_.erase(pageNum);
    else
    {
        freeSpans_[pageNum] = currentSpan->next;
        currentSpan->pre = nullptr;
    }
}

void PageCache::addFreeSpan(Span* span)
{
    //将span添加至freelist
    size_t pageNum = span->numPages;
    assert(pageNum * PAGE_SIZE <= MAX_BYTES);
    Span* currentSpan = freeSpans_[pageNum];
    freeSpans_[pageNum] = span;
    if(currentSpan != nullptr) 
    {
        currentSpan->pre = span;
        span->next = currentSpan;
    }
}

void* PageCache::systemAlloc(size_t numPages)
{
    size_t size = numPages * PAGE_SIZE;

    // 使用mmap分配内存
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;

    // 清零内存
    memset(ptr, 0, size);
    return ptr;
}

} // namespace memoryPool
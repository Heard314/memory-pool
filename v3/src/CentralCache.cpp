#include "../include/CentralCache.h"
#include "../include/PageCache.h"
#include <cassert>
#include <thread>
#include "CentralCache.h"

namespace MyMemoryPool 
{

// 每次从PageCache获取span大小（以页为单位）
static const size_t SPAN_PAGES = 8;

void* CentralCache::fetchRange(size_t index, size_t batchNum, size_t& actualBatchNum)
{
    // 索引检查，当索引大于等于FREE_LIST_SIZE时，说明申请内存过大应直接向系统申请
    if (index >= FREE_LIST_SIZE || batchNum == 0) 
        return nullptr;

    // 自旋锁保护
    while (locks_[index].test_and_set(std::memory_order_acquire)) // test_and_set:这是原子操作，将value设置为 true，并返回原来的值
    {
        std::this_thread::yield(); // 添加线程让步，避免忙等待，避免过度消耗CPU
    }

    void* result = nullptr;
    try 
    {
        // 尝试从中心缓存获取内存块
        // result = centralFreeList_[index].load(std::memory_order_relaxed);
        result = centralFreeList_[index];

        if (result == nullptr)
        {
            // 如果中心缓存为空，从页缓存获取新的内存块
            size_t size = (index + 1) * ALIGNMENT;
            size_t actualNumPage = batchNum; 
            result = fetchFromPageCache(size * batchNum, actualNumPage); //期望获取batchNum个大小为size的内存块

            // 如果获取不到
            if (!result)
            {
                locks_[index].clear(std::memory_order_release);
                actualBatchNum = 0;
                return nullptr;
            }

            // 将从PageCache获取的内存块切分成小块
            char* start = static_cast<char*>(result);
            size_t totalBlocks = (actualNumPage * PageCache::PAGE_SIZE) / size; //从页缓存中获取到的块的个数
            size_t allocBlocks = std::min(batchNum, totalBlocks);  //返回的分块的数量

            //统计每个页
            //统计第一页
            void* nowPageBatch = result;
            assert(pageBatchStats_.count(nowPageBatch)==0);
            pageBatchStats_[nowPageBatch] = PageBatchStat(totalBlocks,0);

            //记录内存块与所属页之间的映射
            // ownPageBatch_[start] = nowPageBatch;
            actualBatchNum = allocBlocks;

            //若返回给ThreadCache内存块数量大于1，则构建内存块链表
            for (size_t i = 0; i < allocBlocks; ++i) 
            {
                void* current = start + i * size;
                    
                //更新页批量的统计信息 
                allocatedBlock_.insert(current);
                ownPageBatch_[current] = nowPageBatch;
                void* next = start + (i+1) * size;
                *reinterpret_cast<void**>(current) = next;
            }
            *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;

            // 构建保留在CentralCache的链表，并将其放置在表头
            //! 放在表头上的好处是：减少之前的内存块较多的页批量分配的优先级
            if (totalBlocks > allocBlocks)
            {
                //构建链表
                void* remainStart = start + allocBlocks * size;
                for (size_t i = allocBlocks; i < totalBlocks; ++i)
                {
                    void* current = start + i * size;
                    void* next = start + (i+1) * size;
                    //!将暂时保留的页批量也做统计信息更新
                    ownPageBatch_[current] = nowPageBatch;
                    *reinterpret_cast<void**>(current) = next;
                }
                void* remainEnd = start + (totalBlocks - 1) * size;
                *reinterpret_cast<void**>(remainEnd) = centralFreeList_[index];

                //!还需要更新原表头内存块的beforeBlock
                *reinterpret_cast<void**>(centralFreeList_[index]) = remainEnd;
                centralFreeList_[index] = remainStart;
                centralFreeListTail_[index] = remainEnd;
                centralFreeListSize_[index] += totalBlocks-allocBlocks;

                beginBlock_[nowPageBatch] = remainStart;
                endBlock_[nowPageBatch] = remainEnd;
                beforeBlock_[nowPageBatch] = nullptr;
            }
        }
        else // 如果中心缓存有index对应大小的内存块
        {
            // 从现有链表头中获取指定数量的块
            void* current = result;
            void* prev = nullptr;
            size_t count = 0;

            while (current && count < batchNum)
            {
                prev = current;
                current = *reinterpret_cast<void**>(current);
                popBlockFromFreeList(index);
                count++;
            }
            actualBatchNum = count;
            if (prev) // 当前centralFreeList_[index]链表上的内存块大于batchNum时需要用到 
            {
                *reinterpret_cast<void**>(prev) = nullptr;
            }
        }
    }
    catch (...) 
    {
        locks_[index].clear(std::memory_order_release);
        actualBatchNum = 0;
        throw;
    }

    // 释放锁
    locks_[index].clear(std::memory_order_release);
    return result;
}

void CentralCache::popBlockFromFreeList(size_t index)
{
    assert(centralFreeList_[index]==nullptr);
    void* current = centralFreeList_[index];
    void* pageBatchStart = ownPageBatch_[current];

    //暂停准备回收
    if(alreadyPageBatch_.count(pageBatchStart)) alreadyPageBatch_.erase(pageBatchStart);
    //维护空闲链表信息
    centralFreeListSize_[index]--;
    centralFreeList_[index] = *reinterpret_cast<void**>(current);
    if(centralFreeList_[index] == nullptr) centralFreeListTail_[index] = nullptr;

    if(allocatedBlock_.count(current))
    {
        pageBatchStats_[pageBatchStart].current--;
    }
    else
    {
        allocatedBlock_.insert(current);
    }

    bool isOnlyOneBlock = (beginBlock_[pageBatchStart]==endBlock_[pageBatchStart]);
    void* nxtBlock = centralFreeList_[index];
    if(isOnlyOneBlock) 
    {
        beginBlock_.erase(pageBatchStart);
        endBlock_.erase(pageBatchStart);
        beforeBlock_.erase(pageBatchStart);
        
        void* nxtBatchBlock = *reinterpret_cast<void**>(endBlock_[pageBatchStart]);
        if(nxtBatchBlock!=nullptr) beforeBlock_[ownPageBatch_[nxtBatchBlock]] = nullptr;
    }
    else 
    {
        beginBlock_[pageBatchStart] = nxtBlock;
    }
}

//供线程缓存调用，用于归还return个内存块
void CentralCache::returnRange(void* start, size_t returnNum, size_t index)
{   
    assert(start!=nullptr);
    assert(index < FREE_LIST_SIZE);
    while (locks_[index].test_and_set(std::memory_order_acquire)) 
    {
        std::this_thread::yield();
    }
    try 
    {
        // 找到要归还的链表的最后一个节点
        void* end = start;
        size_t count = 1;
        while (*reinterpret_cast<void**>(end) != nullptr && count < returnNum)
        {
            pushBlockForFreeList(end,index);
            end = *reinterpret_cast<void**>(end);
            count++;
        }
        assert(*reinterpret_cast<void**>(end)==nullptr); //检查列表是否被合理断开
        // 将归还的链表连接到中心缓存的链表头部
    }
    catch (...) 
    {
        locks_[index].clear(std::memory_order_release);
        throw;
    }

    locks_[index].clear(std::memory_order_release);
}



// 实现内存块放回空闲链表
inline void CentralCache::pushBlockForFreeList(void* returnBlock, size_t index)
{
    //将被退回的内存块放到空闲链表中
    void* returnPageBatchStart = ownPageBatch_[returnBlock];
    void* current = centralFreeList_[index]; //当前空闲链表中的首元素
    void* currentPageBatchStart = ownPageBatch_[current];
    //TODO 待测试，归还的内存块拼接起来
    //当这个退回的内存块是该页批量中第一个被退回的数据时，将这个内存块放在表头
    //! 这样做的潜在好处：提高程序的局部性，该内存可能会被原线程重新利用；
    //! 能够保护之前具有更多被退回内存块的页批量
    if(beginBlock_.count(returnPageBatchStart)==0) {
        beginBlock_[returnPageBatchStart] = returnBlock;
        *reinterpret_cast<void**>(returnBlock) = current;
        beforeBlock_[currentPageBatchStart] = returnBlock;
        endBlock_[returnPageBatchStart] = returnBlock;
        beforeBlock_[returnPageBatchStart] = nullptr;
    }
    else //!否则将该内存块放到所属页批量的后面
    {
        void* endNxtBlockStart = *reinterpret_cast<void**>(endBlock_[returnPageBatchStart]);
        *reinterpret_cast<void**>(returnBlock) = endNxtBlockStart;
        *reinterpret_cast<void**>(endBlock_[returnPageBatchStart]) = returnBlock;
        endBlock_[returnPageBatchStart] = returnBlock;
        beforeBlock_[ownPageBatch_[endNxtBlockStart]] = returnBlock;
    }   
    

    char* _returnBlock = static_cast<char*>(returnBlock); //方便按照以字节大小进行地址的移动
    
    assert(allocatedBlock_.count(returnBlock));
    pageBatchStats_[returnPageBatchStart].current++;
    
    if(pageBatchStats_[returnPageBatchStart].current==pageBatchStats_[returnPageBatchStart].total)
    {
        //记录该内存可以回收
        alreadyPageBatch_.insert(returnPageBatchStart);
        //维护链表顺序
        void* beginBlock = beginBlock_[returnPageBatchStart];
        void* lastBlock = endBlock_[returnPageBatchStart];
        detachPageBatchFromFreeList(index,returnPageBatchStart);
        //该内存移动到末尾 
        *reinterpret_cast<void**>(centralFreeListTail_[index]) = beginBlock;
        beforeBlock_[returnPageBatchStart] = centralFreeListTail_[index];
        centralFreeListTail_[index] = lastBlock;
    }

    //更新空闲链表信息
    if(centralFreeListSize_[index] == 0)
    {
        centralFreeList_[index] = returnBlock;
        centralFreeListTail_[index] = returnBlock;
    }
    centralFreeListSize_[index]++;

    size_t blockSize = (index+1) * ALIGNMENT;
    if(centralFreeListSize_[index] * blockSize > MAX_SINGLE_FREE_LIST_SIZE) 
    {
        returnPageCache(index);
    }
}

void* CentralCache::returnPageCache(size_t index)
{   
    size_t blockSize = (index+1) * ALIGNMENT;
    size_t maxReturnNum = std::max((size_t)1,(centralFreeListSize_[index] * blockSize - RESERVE_SINGLE_FREE_LIST_SIZE) / blockSize);
    size_t actualReturnNum = 0;
    std::vector<void*> returnPageBatchs;
    for(auto it = alreadyPageBatch_.begin();it != alreadyPageBatch_.end();)
    {
        if(actualReturnNum < maxReturnNum)
        {
            returnPageBatchs.push_back(*it);
            //将PageBatch从空闲列表中删除
            removePageBatchFromFreeList(index,reinterpret_cast<void*>(*it));
            it = alreadyPageBatch_.erase(it);
            actualReturnNum++;
        } else break;
    }
    PageCache::getInstance().returnPageBatchVector(index,returnPageBatchs);
}

//此操作将目标PageBatch从空闲链表中分离出去，暂时不做删除，后续可能会用于移动该页批量
//维护空闲链表剩余块之间的各类信息。
inline void CentralCache::detachPageBatchFromFreeList(size_t index,void* pageBatchStart)
{
    //维护链表顺序
    void* beginBlock = beginBlock_[pageBatchStart];
    void* beforeBlock = beforeBlock_[pageBatchStart];
    void* lastBlock = endBlock_[pageBatchStart];
    void* nxtBlock = *reinterpret_cast<void**>(lastBlock);
    *reinterpret_cast<void**>(beforeBlock) = nxtBlock;
    *reinterpret_cast<void**>(lastBlock) = nullptr;

    //清空当前页批量的前置信息
    beforeBlock_[pageBatchStart] = nullptr;
    //维护剩余空闲内存块的beforeBlock的顺序
    if(nxtBlock!=nullptr) beforeBlock_[ownPageBatch_[nxtBlock]] = beforeBlock;

    //维护centralFreeList的一系列信息
    if(beginBlock == centralFreeList_[index]) centralFreeList_[index] = nxtBlock;
    if(lastBlock == centralFreeListTail_[index]) centralFreeListTail_[index] = beforeBlock;
}

//此操作首先将目标PageBatch从空闲链表中分离，并删除该PageBatch的相关信息
inline void CentralCache::removePageBatchFromFreeList(size_t index,void* pageBatchStart)
{
    // void* nxtBlock_ = *reinterpret_cast<void**>(lastBlock_);
    detachPageBatchFromFreeList(index, pageBatchStart);

    //修改空闲链表大小
    centralFreeListSize_[index] -= pageBatchStats_[pageBatchStart].total;
    //更新ownPageBatch_
    void* beginBlock = beginBlock_[pageBatchStart];
    
    void* lastBlock = endBlock_[pageBatchStart];

    while(beginBlock != lastBlock)
    {
        ownPageBatch_.erase(beginBlock);
        beginBlock = *reinterpret_cast<void**>(beginBlock);
    }
    if(lastBlock!=nullptr) ownPageBatch_.erase(lastBlock);

    //删除其余内存块前后关系信息
    pageBatchStats_.erase(pageBatchStart);
    beginBlock_.erase(pageBatchStart);
    endBlock_.erase(pageBatchStart);
    beforeBlock_.erase(pageBatchStart);
}

void* CentralCache::fetchFromPageCache(size_t size,size_t& actualNumPages)
{   
    // 1. 计算实际需要的页数
    size_t numPages = (size + PageCache::PAGE_SIZE - 1) / PageCache::PAGE_SIZE;

    // 2. 根据大小决定分配策略
    if (size <= SPAN_PAGES * PageCache::PAGE_SIZE) 
    {
        // 小于等于32KB的请求，使用固定8页
        actualNumPages = SPAN_PAGES;
        return PageCache::getInstance().allocateSpan(SPAN_PAGES);
    } 
    else 
    {
        // 大于32KB的请求，按实际需求分配
        actualNumPages = numPages;
        return PageCache::getInstance().allocateSpan(numPages);
    }
}

} // namespace memoryPool
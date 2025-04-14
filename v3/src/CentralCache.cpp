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

        if (!result)
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
            //TODO 思考一下nowPageBatch是否会在之前提供
            PageBatchStat nowStat = pageBatchStats.count(nowPageBatch) ? pageBatchStats[nowPageBatch]:PageBatchStat(0,0);
            nowStat.total++;
            //记录内存块与所属页之间的映射
            ownPageBatch[start] = nowPageBatch;

            //初始化链表第一个节点
            *reinterpret_cast<void**>(result) = nullptr;
            actualBatchNum = allocBlocks;
            //若返回给ThreadCache内存块数量大于1，则构建内存块链表
            if (allocBlocks > 1) 
            {
                for (size_t i = 1; i < allocBlocks; ++i) 
                {
                    void* current = start + (i - 1) * size;
                    //更新页批量的统计信息 
                    updatePageBatchStats(start,i,size,nowPageBatch,nowStat);
                    void* next = start + i * size;

                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (allocBlocks - 1) * size) = nullptr;
            }
            pageBatchStats[nowPageBatch] = nowStat;
            // 构建保留在CentralCache的链表
            if (totalBlocks > allocBlocks)
            {
                void* remainStart = start + allocBlocks * size;
                for (size_t i = allocBlocks + 1; i < totalBlocks; ++i)
                {
                    void* current = start + (i - 1) * size;
                    void* next = start + i * size;
                    *reinterpret_cast<void**>(current) = next;
                }
                *reinterpret_cast<void**>(start + (totalBlocks - 1) * size) = nullptr;

                // centralFreeList_[index].store(remainStart, std::memory_order_release);
                centralFreeList_[index] = remainStart;
                centralFreeListSize_[index] += totalBlocks-allocBlocks;
            }
        }
        else // 如果中心缓存有index对应大小的内存块
        {
            // 从现有链表中获取指定数量的块
            void* current = result;
            void* prev = nullptr;
            size_t count = 0;

            while (current && count < batchNum)
            {
                prev = current;
                current = *reinterpret_cast<void**>(current);
                count++;
            }
            actualBatchNum = count;
            if (prev) // 当前centralFreeList_[index]链表上的内存块大于batchNum时需要用到 
            {
                *reinterpret_cast<void**>(prev) = nullptr;
            }

            // centralFreeList_[index].store(current, std::memory_order_release);
            centralFreeList_[index] = current;
            centralFreeListSize_[index] -= actualBatchNum;
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

//
//TODO1 如何将曾经同一页的内存块聚合起来，每次内存块返回来时，该内存块所属的页进行统计，当凑成很多个整页时返回给PageCache；
//TODO1 WTF，需要考虑怎么统计，可以先用个unorder_map来进行统计
//
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
            centralFreeListSize_[index]++;
            void* pagePtr = ownPageBatch[end];
            void * current = centralFreeList_[index];
            //TODO 待测试，归还的内存块拼接起来
            if(beginBlock.count(pagePtr)==0) {
                beginBlock[pagePtr] = end;
                *reinterpret_cast<void**>(end) = current;
                centralFreeList_[index] = start;
            } else {
                *reinterpret_cast<void**>(end) = *reinterpret_cast<void**>(nextBlock[pagePtr]);
                *reinterpret_cast<void**>(nextBlock[pagePtr]) = end;
            }
            nextBlock[pagePtr] = end;
            updateReturnPageBatchStats(end,index);
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

// //由于一个内存块可能只占用一个页，也可能跨多个页，较为复杂提取为了一个函数
//TODO 250414 
inline void CentralCache::updatePageBatchStats(void* start, size_t num, size_t size, void* &nowPage,PageBatchStat &nowStat)
{
    //处理当前内存块与页之间的关系
    //更新当前页
    void* current = start + (num-1) * size;
    size_t pageNum = 1;
    nowStat.total++;
    //记录内存块与所属页之间的映射
    ownPageBatch[current] = nowPage;

    //处理跨页的信息
    while((start + num * size) >= nowPage + PAGE_SIZE)
    {
        pageBatchStats[nowPage] = nowStat; //整理好当前页，因为当前页不会包含更多内存块
        nowPage += PAGE_SIZE;
        nowStat = pageBatchStats.count(nowPage) ? pageBatchStats[nowPage]:PageBatchStat(0,0);
        nowStat.total++;//更新新一页的统计信息
    }
}

// 中心缓存回收完整的页内存的方法
inline void CentralCache::updateReturnPageBatchStats(void* blockPtr, size_t index)
{
    size_t blockSize = (index+1) * ALIGNMENT;
    char* startPage = static_cast<char*>(ownPageBatch[blockPtr]);

    char* _blockPtr = static_cast<char*>(blockPtr); //方便按照以字节大小进行地址的移动
    while(startPage <= _blockPtr + blockSize)
    {
        pageBatchStats[startPage].current++;
        if(pageBatchStats[startPage].current==pageBatchStats[startPage].total)
        {
            //记录该内存可以回收
            alreadyPage.insert(startPage);
            //同时将该内存移动到末尾 
            //维护链表顺序
            void* beginBlock_ = beginBlock[startPage];
            void* lastBlock_ = nextBlock[startPage];
            removePageFromFreeList(index,startPage);
            *reinterpret_cast<void**>(centralFreeListTail_[index]) = beginBlock_;
            centralFreeListTail_[index] = lastBlock_;
        }
        startPage += PAGE_SIZE;
    }

    if(centralFreeListSize_[index] * blockSize > MAX_SINGLE_FRESS_LIST_SIZE) 
    {
        //TODO 当中心缓存太多时，释放一些完整页空闲内存，由页缓存进一步处理
        returnPageCache(index);
    }
}
void* CentralCache::returnPageCache(void* start, size_t returnPageNum, size_t index)
{   
    size_t blockSize = (index+1) * ALIGNMENT;
    size_t maxReturnNum = MAX_SINGLE_FRESS_LIST_SIZE / blockSize;
    size_t actualReturnNum = 0;
    std::vector<void*> returnPages;
    for(auto it = alreadyPage.begin();it != alreadyPage.end();)
    {
        if(actualReturnNum < maxReturnNum)
        {
            returnPages.push_back(*it);
            it = alreadyPage.erase(it);
            actualReturnNum++;
            //善后其他信息
            removePageFromFreeList(index,reinterpret_cast<void*>(*it));

        }
    }
    PageCache::getInstance().returnPageVector(index,returnPages);
    // PageCache::getInstance().deallocateSpan(ownPageBatch[start],returnPageNum);
    // //更新统计信息
    // centralFreeListSize_[index] -= pageBatchStats[ownPageBatch[start]].current;
}

inline void CentralCache::removePageFromFreeList(size_t index,void* startPage)
{
    //同时将该内存移动到末尾
    //维护链表顺序
    void* beginBlock_ = beginBlock[startPage];
    void* beforeBlock_ = beforeBlock[beginBlock_];
    void* lastBlock_ = nextBlock[startPage];
    void* nxtBlock_ = *reinterpret_cast<void**>(lastBlock_);
    *reinterpret_cast<void**>(beforeBlock_) = nxtBlock_;
    *reinterpret_cast<void**>(lastBlock_) = nullptr;
    //维护beforeBlock的顺序
    beforeBlock[beginBlock_] = centralFreeListTail_[index];
    if(nxtBlock_!=nullptr) beforeBlock[nxtBlock_] = beforeBlock_;

    //维护centralFreeList的一系列信息
    if(beginBlock_ == centralFreeList_[index]) centralFreeList_[index] = nxtBlock_;
    //跨页情况怎么处理
    centralFreeList_[index] = ;
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
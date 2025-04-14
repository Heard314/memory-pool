#pragma once
#include "Common.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace MyMemoryPool 
{

/**
 * TODO 250414，不再以页为单位，以分配的时候的页集群为单位，这样好做回收，回收效率也高，也不会出现跨页问题
 * 
 */


class CentralCache
{
public:
    static CentralCache& getInstance()
    {
        static CentralCache instance;
        return instance;
    }

    // 用于统计返回页数
    struct PageBatchStat
    {
        size_t total;
        size_t current; //目前被退回的内存块的个数
        PageBatchStat() = default;
        PageBatchStat(size_t total_,size_t current_):total(total_),current(current_){};
    };

    void* fetchRange(size_t index, size_t batchNum,size_t& ActualBatchNum);
    void returnRange(void* start, size_t returnNum, size_t bytes);

    void updatePageBatchStats(void* start, size_t num, size_t size, void* &nowPage,PageBatchStat &nowStat);

    void updateReturnPageBatchStats(void *start, size_t index);

    void *returnPageCache(void *start, size_t returnPageNum, size_t index);

    void removePageFromFreeList(size_t index, void *startPage);

private:
    // 相互是还所有原子指针为nullptr
    CentralCache()
    {
        for (auto& ptr : centralFreeList_)
        {
            // ptr.store(nullptr, std::memory_order_relaxed);
            ptr = nullptr;
        }
        for (auto& cnt : centralFreeListSize_)
        {
            cnt = 0;
        }
        // 初始化所有锁
        for (auto& lock : locks_)
        {
            lock.clear();
        }
    }
    // 从页缓存获取内存
    void* fetchFromPageCache(size_t size,size_t& actualNumPages);

private:
    // 中心缓存的自由链表
    std::array<void*,FREE_LIST_SIZE> centralFreeList_;
    std::array<size_t,FREE_LIST_SIZE> centralFreeListSize_;
    std::array<void*, FREE_LIST_SIZE> centralFreeListTail_; //TODO待全局修改
    // 用于同步的自旋锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    //TODO 研究unordered_map的作用
    //TODO 以下链表还需要再研究一下初始化，清0等操作
    std::unordered_map<void*,PageBatchStat> pageBatchStats; //每个页目前的内存退回情况，key是页的起始地址，val是存储该页状态的PageBatchStat
    std::unordered_map<void*,void*> ownPageBatch; //方便每个内存块找到所属页，key是内存块的起始地址，val是内存块所在页的起始地址
    //为了便于更新退回某一内存页后，更新空闲链表中的内存，应该把一整页的内存聚合起来，
    std::unordered_map<void*,void*> beginBlock; //记录某个页的第一块被退回的内存块，key是页的起始地址，val是该页第一个被退回的块的起始地址
    std::unordered_map<void*,void*> beforeBlock; //记录某个页的第一块被退回的内存块的前一块内存块地址，key是页中该页第一个被退回的块的起始地址，val是其在空闲链表中的前一个内存块的起始地址//TODO 更新到全局中去，需要额外维护 //! 最小块的大小不足以存放两个地址指针
    std::unordered_map<void*,void*> nextBlock; //记录了页的最后一块被退回的内存块，key是页的起始地址，val是该页最后一个被退回的块的起始地址
    std::unordered_set<void*> alreadyPage;  //存储了能够被退回的页的地址，需要考虑某一个页的内存被重新使用，存储了可以被退回的页的起始地址
};

} // namespace memoryPool
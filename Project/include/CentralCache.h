#pragma once
#include "Common.h"
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace MyMemoryPool 
{

/**
 * 
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

    void popBlockFromFreeList(size_t index);

    void returnRange(void *start, size_t returnNum, size_t bytes);

    void pushBlockForFreeList(void *returnBlock, size_t index);

    void *returnPageCache(size_t index);

    void detachPageBatchFromFreeList(size_t index, void *pageBatchStart);

private:
    // 相互是还所有原子指针为nullptr
    CentralCache()
    {
        // 初始化链表指针为空，链表尺寸为0
        for (size_t i = 0; i < FREE_LIST_SIZE; ++i)
        {
            centralFreeList_[i] = nullptr;
            centralFreeListSize_[i] = 0;
            centralFreeListTail_[i] = nullptr;

            // 初始化每个锁为清除状态（解锁）
            locks_[i].clear();

            // unordered_map/unordered_set 默认构造函数会自动完成初始化
            pageBatchStats_[i].clear();
            ownPageBatch_[i].clear();
            beginBlock_[i].clear();
            beforeBlock_[i].clear();
            endBlock_[i].clear();
            alreadyPageBatch_[i].clear();
            allocatedBlock_[i].clear();
        }
    }

    ~CentralCache()
    {
        for (size_t i = 0; i < FREE_LIST_SIZE; ++i)
        {
            for(auto pr:pageBatchStats_[i])
            {
                void* pageBatchStart = pr.first;
                alreadyPageBatch_[i].insert(pageBatchStart);
            }
            returnPageCache(i);
        }

        for (size_t i = 0; i < FREE_LIST_SIZE; ++i)
        {
            pageBatchStats_[i].clear();
            ownPageBatch_[i].clear();
            beginBlock_[i].clear();
            beforeBlock_[i].clear();
            endBlock_[i].clear();
            alreadyPageBatch_[i].clear();
            allocatedBlock_[i].clear();

            centralFreeList_[i] = nullptr;
            centralFreeListTail_[i] = nullptr;
            centralFreeListSize_[i] = 0;
            locks_[i].clear();
        }
    }

    void removePageBatch(size_t index, void *pageBatchStart);

    // 从页缓存获取内存
    void* fetchFromPageCache(size_t size,size_t& actualNumPages);

private:
    // 中心缓存的自由链表
    std::array<void*,FREE_LIST_SIZE> centralFreeList_;
    std::array<size_t,FREE_LIST_SIZE> centralFreeListSize_;
    std::array<void*, FREE_LIST_SIZE> centralFreeListTail_;
    // 用于同步的自旋锁
    std::array<std::atomic_flag, FREE_LIST_SIZE> locks_;

    //TODO 研究unordered_map的作用
    //TODO 以下链表还需要再研究一下初始化，清0等操作
    std::array<std::unordered_map<void*,PageBatchStat>,FREE_LIST_SIZE> pageBatchStats_; //每个页批量目前的内存退回情况，key是页批量的起始地址，val是存储该页批量状态的PageBatchStat，存储了当前所有未被释放的页批量的信息
    std::array<std::unordered_map<void*,void*>,FREE_LIST_SIZE> ownPageBatch_; //方便每个内存块找到所属页批量，key是内存块的起始地址，val是内存块所在页批量的起始地址
    //为了便于更新退回某一内存页批量后，更新空闲链表中的内存，应该把一整页批量的内存聚合起来，
    std::array<std::unordered_map<void*,void*>,FREE_LIST_SIZE> beginBlock_; //记录某个页批量的第一块内存块，key是页批量批量的起始地址，val是在空闲列表中该页批量第一个起始地址
    std::array<std::unordered_map<void*,void*>,FREE_LIST_SIZE> beforeBlock_; //记录某个页批量的第一块内存块的前一块内存块地址，key是页批量的起始地址，val是其在空闲链表中的前一个内存块的起始地址
    std::array<std::unordered_map<void*,void*>,FREE_LIST_SIZE> endBlock_; //记录了页批量的最后一块内存块，key是页批量的起始地址，val是在空闲列表中该页批量最后一个块的起始地址
    std::array<std::unordered_set<void*>,FREE_LIST_SIZE> alreadyPageBatch_;  //存储了可以被退回的页批量的起始地址
    //! 只有已经得到过一次分配的内存块才可以算作被退回
    //! beginBlock和endBlock的意义需要有所改变，任何内存块的进出都需要进行维护，在页批量得到分配时就需要进行分配
    //! pageBatchStats_也需要做修改，只有已分配的内存块发生了进出才做维护
    std::array<std::unordered_set<void*>,FREE_LIST_SIZE> allocatedBlock_; //存储了已经得到分配的Block //TODO 更新到整个项目中
};

} // namespace memoryPool
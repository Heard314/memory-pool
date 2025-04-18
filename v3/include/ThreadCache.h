#pragma once
#include "Common.h"
#include <unordered_map>
#include "CentralCache.h"
namespace MyMemoryPool 
{

// 线程本地缓存 单例模式
class ThreadCache
{
public:
    static ThreadCache* getInstance()
    {
        static thread_local ThreadCache instance; //thread_local关键字使该变量线程独立，并且生命周期在线程中自动管理
        return &instance;
    }

    void* allocate(size_t size);
    void deallocate(void* ptr);
private:
    ThreadCache() = default;

    ~ThreadCache() {
        for(auto pr:memSize)
        {
            void* addr = pr.first;
            deallocate(addr);
        }
        memSize.clear();
        for (size_t index = 0; index < FREE_LIST_SIZE; ++index) {
            void* head = freeList_[index];
            CentralCache::getInstance().returnRange(head,freeListSize_[index],index); 
            freeList_[index] = nullptr;
            freeListSize_[index] = 0;
        }
    }

    // 从中心缓存（各个线程共享的内存池）获取内存
    void* fetchFromCentralCache(size_t index);
    // 归还内存到中心缓存
    void returnToCentralCache(void* start, size_t size);
    // 计算批量获取内存块的数量
    size_t getBatchNum(size_t size);
    // 判断是否需要归还内存给中心缓存
    bool shouldReturnToCentralCache(size_t index);

    std::array<void*, FREE_LIST_SIZE> freeList_; // 每个线程的自由链表数组，对象被析构时，std::array会自动析构
    std::array<size_t, FREE_LIST_SIZE> freeListSize_; // 自由链表大小统计
    std::unordered_map<void*, size_t> memSize;
};

} // namespace memoryPool
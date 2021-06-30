#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cache.hpp"
#include "lru_cache_policy.hpp"
#include "lfu_cache_policy.hpp"
#include "fifo_cache_policy.hpp"

using namespace std;

// alias for easy class typing
template <typename Key, typename Value>
using lru_cache_t = typename caches::fixed_sized_cache<Key, Value, caches::LRUCachePolicy<Key>>;
template <typename Key, typename Value>
using lfu_cache_t = typename caches::fixed_sized_cache<Key, Value, caches::LFUCachePolicy<Key>>;
template <typename Key, typename Value>
using fifo_cache_t = typename caches::fixed_sized_cache<Key, Value, caches::FIFOCachePolicy<Key>>;

constexpr std::size_t CACHE_SIZE = 20;
lru_cache_t<int, int> lru_cache(CACHE_SIZE);
lfu_cache_t<int, int> lfu_cache(CACHE_SIZE);
fifo_cache_t<int, int> fifo_cache(CACHE_SIZE);

void lru_func(int tid)
{
    if (tid % 3 == 0) {
        for (int i = 0; i < 30; i++) {
            lru_cache.Put(i, i);
        }
    } 
    else if (tid % 3 ==  1) {
        for (int i = 15; i < 50; i++) {
            lru_cache.Put(i, i);
        }
    } 
    else {
        for (int i = 10; i < 40; i++) {
            lru_cache.Put(i, i);
        }
    }
}

void lfu_func(int tid)
{
    if (tid % 3 == 0) {
        for (int i = 0; i < 30; i++) {
            lfu_cache.Put(i, i);
        }
    } else if (tid % 3 ==  1) {
        for (int i = 15; i < 30; i++) {
            lfu_cache.Put(i, i);
        }
    } else {
        for (int i = 10; i < 40; i++) {
            lfu_cache.Put(i, i);
        }
    }
}

void fifo_func(int tid)
{
    if (tid % 3 == 0) {
        for (int i = 0; i < 30; i++) {
            fifo_cache.Put(i, i);
        }
    } else if (tid % 3 ==  1) {
        for (int i = 15; i < 30; i++) {
            fifo_cache.Put(i, i);
        }
    } else {
        for (int i = 10; i < 40; i++) {
            fifo_cache.Put(i, i);
        }
    }
}

int main() {
    int nr_threads = 3;
    vector<thread> lru_thd, lfu_thd, fifo_thd;
    //vector<thread> lru_thd(nr_threads);

    // LRU
    for (int i = 0; i < nr_threads; i++) {
        lru_thd.push_back(thread(lru_func, i));
    }
    for (auto &t : lru_thd)
        t.join();
    lru_cache.PrintCache();

#if 0
    // LFU 
    for (int i = 0; i < nr_threads; i++) {
        lfu_thd.push_back(thread(lfu_func, i));
    }
    for (auto &t : lfu_thd)
        t.join();
    lfu_cache.PrintCache();

    // FIFO 
    for (int i = 0; i < nr_threads; i++) {
        fifo_thd.push_back(thread(fifo_func, i));
    }
    for (auto &t : fifo_thd)
        t.join();
    fifo_cache.PrintCache();
#endif 

}

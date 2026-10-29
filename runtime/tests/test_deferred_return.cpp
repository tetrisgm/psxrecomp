#include "deferred_return.h"
#include <array>
#undef NDEBUG
#include <cassert>
#include <thread>
#include <vector>
struct Item : DeferredReturnLink { unsigned id=0; };
int main() {
    DeferredReturn<Item> returns;
    assert(!returns.pop());
    Item reused;
    for(unsigned i=0;i<10000;++i) {
        returns.push(&reused);assert(returns.pop()==&reused);assert(!returns.pop());
    }
    constexpr unsigned writers=4, count=10000;
    std::vector<Item> items(writers*count);
    std::array<std::thread,writers> threads;
    std::vector<bool> seen(items.size());
    for(unsigned w=0;w<writers;++w) threads[w]=std::thread([&,w] {
        for(unsigned n=0;n<count;++n) {
            auto &item=items[w*count+n];item.id=w*count+n;
            returns.push(&item);
        }
    });
    for(unsigned received=0;received<items.size();) {
        if(auto* item=returns.pop()) {
            assert(item->id<items.size() && !seen[item->id]);
            seen[item->id]=true;++received;
        } else std::this_thread::yield();
    }
    for(auto &t:threads) t.join();
    assert(!returns.pop());
}

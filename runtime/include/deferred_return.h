#ifndef PSX_DEFERRED_RETURN_H
#define PSX_DEFERRED_RETURN_H
#include <atomic>
struct DeferredReturnLink { std::atomic<DeferredReturnLink*> returned_next{nullptr}; };
/* Intrusive MPSC retirement. A producer uses one exchange and one store,
 * without allocation, retries or waiting for reclamation. The sole consumer
 * may see a temporarily incomplete link; it retries on its next service tick.
 * An item must not be returned again until the consumer has reclaimed it. */
template<class T> class DeferredReturn {
    static_assert(std::atomic<DeferredReturnLink*>::is_always_lock_free);
    DeferredReturnLink stub;
    std::atomic<DeferredReturnLink*> head{&stub};
    DeferredReturnLink* tail=&stub;
    void push_link(DeferredReturnLink* node) {
        node->returned_next.store(nullptr,std::memory_order_relaxed);
        auto* previous=head.exchange(node,std::memory_order_acq_rel);
        previous->returned_next.store(node,std::memory_order_release);
    }
public:
    void push(T* node) { push_link(node); }
    T* pop() {
        auto* node=tail;
        auto* next=node->returned_next.load(std::memory_order_acquire);
        if(node==&stub) {
            if(!next) return nullptr;
            tail=next;node=next;
            next=node->returned_next.load(std::memory_order_acquire);
        }
        if(next) {tail=next;return static_cast<T*>(node);}
        if(node!=head.load(std::memory_order_acquire)) return nullptr;
        push_link(&stub);
        next=node->returned_next.load(std::memory_order_acquire);
        if(!next) return nullptr;
        tail=next;return static_cast<T*>(node);
    }
};
#endif

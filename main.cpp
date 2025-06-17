#include <vector>    // for std::vector
#include <string>    // for std::string
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <stdexcept>
#ifdef __APPLE__
#include <mach/mach.h>
#endif // for std::runtime_error
#include <chrono>    // for timing

// Pool class: vector-like, supports push_back and operator[]
// Dynamically allocates new memory blocks when needed

template<typename T>
class pool {
    // Calculate padding so that sizeof(Element) is a multiple of 64
    static constexpr size_t element_padding = (64 - (sizeof(T) % 64)) % 64;
    struct alignas(64) Element {
        T obj;
        char padding[element_padding];
    };

public:
    explicit pool(size_t initial_capacity)
        : block_size(initial_capacity), count(0), total_capacity(initial_capacity) {
        add_block(block_size);
    }
    ~pool() {
        for (size_t i = 0; i < object_ptrs.size(); ++i) {
            if (object_ptrs[i]) {
                reinterpret_cast<T*>(object_ptrs[i])->~T();
            }
        }
        for (auto& block : blocks) {
            ::operator delete[](block.ptr, std::align_val_t(64));
        }
    }
    template<typename... Args>
    void push_back(Args&&... args) {
        size_t insert_idx;
        if (!free_list.empty()) {
            insert_idx = free_list.back();
            free_list.pop_back();
            size_t block_idx, offset;
            get_block_and_offset(insert_idx, block_idx, offset);
            Element* element_place = reinterpret_cast<Element*>(static_cast<char*>(blocks[block_idx].ptr) + offset * sizeof(Element));
            new (&(element_place->obj)) T(std::forward<Args>(args)...);
            object_ptrs[insert_idx] = &(element_place->obj);
            ++blocks[block_idx].refcount;
        } else {
            if (count == total_capacity) {
                grow();
            }
            size_t block_idx, offset;
            get_block_and_offset(count, block_idx, offset);
            Element* element_place = reinterpret_cast<Element*>(static_cast<char*>(blocks[block_idx].ptr) + offset * sizeof(Element));
            new (&(element_place->obj)) T(std::forward<Args>(args)...);
            object_ptrs.push_back(&(element_place->obj));
            ++blocks[block_idx].refcount;
            ++count;
        }
    }
    T& operator[](size_t idx) {
        if (idx >= object_ptrs.size() || !object_ptrs[idx]) throw std::out_of_range("Index out of range or deleted");
        return *reinterpret_cast<T*>(object_ptrs[idx]);
    }
    const T& operator[](size_t idx) const {
        if (idx >= object_ptrs.size() || !object_ptrs[idx]) throw std::out_of_range("Index out of range or deleted");
        return *reinterpret_cast<T*>(object_ptrs[idx]);
    }
    size_t size() const { return count; }
    size_t capacity() const { return total_capacity; }
public:
    void erase(size_t idx) {
        if (idx >= object_ptrs.size() || !object_ptrs[idx]) throw std::out_of_range("Index out of range or already deleted");
        // Найти блок и offset
        size_t block_idx, offset;
        get_block_and_offset(idx, block_idx, offset);
        reinterpret_cast<T*>(object_ptrs[idx])->~T();
        object_ptrs[idx] = nullptr;
        free_list.push_back(idx);
        --blocks[block_idx].refcount;
        // Если блок полностью пуст — освободить
        if (blocks[block_idx].refcount == 0) {
            // Удалить все object_ptrs и free_list, относящиеся к этому блоку
            size_t base = 0;
            for (size_t i = 0; i < block_idx; ++i) base += blocks[i].capacity;
            for (size_t j = 0; j < blocks[block_idx].capacity; ++j) {
                size_t abs_idx = base + j;
                if (abs_idx < object_ptrs.size()) object_ptrs[abs_idx] = nullptr;
            }
            free_list.erase(std::remove_if(free_list.begin(), free_list.end(), [&](size_t i) {
                return (i >= base && i < base + blocks[block_idx].capacity);
            }), free_list.end());
            ::operator delete[](blocks[block_idx].ptr, std::align_val_t(64));
            blocks[block_idx].ptr = nullptr;
        }
    }
    bool is_alive(size_t idx) const {
        return idx < object_ptrs.size() && object_ptrs[idx];
    }
private:
    struct BlockInfo {
        void* ptr;
        size_t capacity;
        size_t refcount;
    };
    std::vector<BlockInfo> blocks;
    std::vector<void*> object_ptrs;
    std::vector<size_t> free_list;
    size_t block_size;
    size_t count;
    size_t total_capacity;

    void add_block(size_t block_capacity) {
        auto t1 = std::chrono::high_resolution_clock::now();
        void* mem = ::operator new[](block_capacity * sizeof(Element), std::align_val_t(64));
        auto t2 = std::chrono::high_resolution_clock::now();
        blocks.push_back({mem, block_capacity, 0});
        auto ms = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        std::cout << "[pool] Allocated block for " << block_capacity << " objects (" << sizeof(Element) << " bytes each, aligned to 64) in " << ms << " microseconds\n";
    }
    void grow() {
        size_t new_block_size = block_size * 2;
        add_block(new_block_size);
        total_capacity += new_block_size;
        block_size = new_block_size;
    }
    void get_block_and_offset(size_t global_idx, size_t& block_idx, size_t& offset) const {
        size_t idx = global_idx;
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (idx < blocks[i].capacity) {
                block_idx = i;
                offset = idx;
                return;
            }
            idx -= blocks[i].capacity;
        }
        throw std::out_of_range("Internal pool index error");
    }

};

class MyClass {
    char data[4096]; // 4 KB per object
    std::string info;
public:
    MyClass(int x_, const std::string& s) : info(s) {
        data[0] = static_cast<char>(x_); // just to use x_
        // std::cout << "Constructed: " << info << "\n";
    }
    ~MyClass() {
        // std::cout << "Destructed: " << info << "\n";
    }
    void print() const {
        // std::cout << "MyClass: data[0]=" << int(data[0]) << " info=" << info << "\n";
    }
};

#ifdef __APPLE__
void print_memory_usage() {
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count) == KERN_SUCCESS) {
        std::cout << "Resident size: " << t_info.resident_size / (1024.0 * 1024.0) << " MB\n";
    } else {
        std::cout << "Could not get memory usage info\n";
    }
}
#else
void print_memory_usage() {}
#endif

int main() {
    constexpr size_t N = 10000000; // 10 million
    pool<MyClass> p(4);
    // Measure push_back
    auto t_push1 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < N; ++i) {
        p.push_back(static_cast<int>(i), "PoolFabric#" + std::to_string(i));
    }
    auto t_push2 = std::chrono::high_resolution_clock::now();
    auto ns_push = std::chrono::duration_cast<std::chrono::nanoseconds>(t_push2 - t_push1).count();
    std::cout << "push_back: total " << ns_push << " ns, avg " << (ns_push / double(N)) << " ns per op\n";
   
    // Measure operator[]
    volatile size_t checksum = 0;
    auto t_idx1 = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < N; ++i) {
        checksum += reinterpret_cast<const void*>(&p[i]) != nullptr;
    }
    auto t_idx2 = std::chrono::high_resolution_clock::now();
    auto ns_idx = std::chrono::duration_cast<std::chrono::nanoseconds>(t_idx2 - t_idx1).count();
    std::cout << "operator[]: total " << ns_idx << " ns, avg " << (ns_idx / double(N)) << " ns per op\n";
    std::cout << "Checksum: " << checksum << " (ignore, prevents optimization)\n";
    print_memory_usage();
    std::cout << "Now erasing all objects...\n";
    for (size_t i = 0; i < N; ++i) {
        if (p.is_alive(i)) p.erase(i);
    }
    std::cout << "After erase and block release:\n";
    print_memory_usage();
    return 0;
}
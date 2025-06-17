#include <vector>    // for std::vector
#include <string>    // for std::string
#include <iostream>  // for std::cout
#include <stdexcept> // for std::runtime_error
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
        // Call destructors for all objects
        for (size_t i = 0; i < count; ++i) {
            reinterpret_cast<T*>(object_ptrs[i])->~T();
        }
        // Free all allocated blocks
        for (void* block : blocks) {
            ::operator delete[](block, std::align_val_t(64));
        }
    }
    template<typename... Args>
    void push_back(Args&&... args) {
        if (count == total_capacity) {
            grow();
        }
        size_t block_idx, offset;
        get_block_and_offset(count, block_idx, offset);
        // Place new object in aligned Element
        Element* element_place = reinterpret_cast<Element*>(static_cast<char*>(blocks[block_idx]) + offset * sizeof(Element));
        new (&(element_place->obj)) T(std::forward<Args>(args)...);
        object_ptrs.push_back(&(element_place->obj));
        ++count;
    }
    T& operator[](size_t idx) {
        if (idx >= count) throw std::out_of_range("Index out of range");
        return *reinterpret_cast<T*>(object_ptrs[idx]);
    }
    const T& operator[](size_t idx) const {
        if (idx >= count) throw std::out_of_range("Index out of range");
        return *reinterpret_cast<T*>(object_ptrs[idx]);
    }
    size_t size() const { return count; }
    size_t capacity() const { return total_capacity; }
private:
    void add_block(size_t block_capacity) {
        auto t1 = std::chrono::high_resolution_clock::now();
        // Allocate memory aligned to 64 bytes
        void* mem = ::operator new[](block_capacity * sizeof(Element), std::align_val_t(64));
        auto t2 = std::chrono::high_resolution_clock::now();
        blocks.push_back(mem);
        block_capacities.push_back(block_capacity);
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
        for (size_t i = 0; i < block_capacities.size(); ++i) {
            if (idx < block_capacities[i]) {
                block_idx = i;
                offset = idx;
                return;
            }
            idx -= block_capacities[i];
        }
        throw std::out_of_range("Internal pool index error");
    }
    std::vector<void*> blocks;
    std::vector<size_t> block_capacities;
    std::vector<void*> object_ptrs;
    size_t block_size;
    size_t count;
    size_t total_capacity;
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
    return 0;
}
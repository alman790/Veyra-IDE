#include "ts/Structures.hpp"
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}

template <class F> void throws(F fn) {
    bool caught = false;
    try {
        fn();
    } catch (const std::exception&) {
        caught = true;
    }
    require(caught, "Expected checked failure");
}

struct Tracked {
    inline static int alive = 0, copies = -1;
    int value = 0;

    explicit Tracked(int n = 0) : value(n) {
        ++alive;
    }

    Tracked(const Tracked& other) : value(other.value) {
        if (copies == 0)
            throw std::runtime_error("copy fault");
        if (copies > 0)
            --copies;
        ++alive;
    }

    Tracked(Tracked&& other) noexcept(false) : value(other.value) {
        other.value = -1;
        ++alive;
    }

    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;

    ~Tracked() {
        --alive;
    }
};

struct alignas(256) Aligned {
    int value = 42;
};

struct ThrowCompare {
    std::shared_ptr<int> budget;

    bool operator()(int a, int b) const {
        if (*budget == 0)
            throw std::runtime_error("comparison fault");
        if (*budget > 0)
            --*budget;
        return a < b;
    }
};

void arrays() {
    ts::Array<Aligned> aligned;
    for (int i = 0; i < 100; ++i)
        aligned.emplaceBack();
    require(reinterpret_cast<std::uintptr_t>(aligned.begin()) % alignof(Aligned) == 0,
            "Array over-alignment");
    ts::Array<std::string> alias;
    alias.reserve(1);
    alias.pushBack("original");
    alias.emplaceBack(alias[0]);
    require(alias.size() == 2 && alias[0] == "original" && alias[1] == "original",
            "emplace aliases element during growth");
    ts::Array<Tracked> values;
    values.reserve(4);
    for (int i = 0; i < 4; ++i)
        values.emplaceBack(i);
    const auto oldCapacity = values.capacity();
    Tracked::copies = 2;
    throws([&] { values.emplaceBack(99); });
    require(values.size() == 4 && values.capacity() == oldCapacity, "Array rollback metadata");
    for (int i = 0; i < 4; ++i)
        require(values[i].value == i, "Array rollback values");
    require(Tracked::alive == 4, "Array rollback destroys constructed values");
    Tracked::copies = -1;
    ts::Array<Tracked> copy = values;
    copy = copy;
    copy = std::move(copy);
    require(copy.size() == 4 && values.size() == 4, "Array self assignment");
    ts::Array<Tracked> moved = std::move(copy);
    require(copy.empty() && moved.size() == 4, "Array move ownership");
    throws([&] { values.at(4); });
    throws([&] { values.reserve(static_cast<std::size_t>(-1)); });
    ts::Array<int> large;
    for (int i = 0; i < 50000; ++i)
        large.pushBack(i);
    for (int i = 49999; i >= 0; --i) {
        require(large.back() == i, "Array stress order");
        large.popBack();
    }
    throws([&] { large.popBack(); });
}

void lists() {
    ts::List<Tracked> list;
    for (int i = 0; i < 8; ++i)
        list.emplaceBack(i);
    Tracked::copies = 3;
    throws([&] { ts::List<Tracked> copy(list); });
    require(Tracked::alive == 8, "List copy exception leak");
    Tracked::copies = 0;
    throws([&] { list.popFront(); });
    require(list.size() == 8 && list.front().value == 0, "List extraction rollback");
    Tracked::copies = -1;
    ts::List<Tracked> copy(list);
    copy = copy;
    copy = std::move(copy);
    require(copy.size() == 8, "List self assignment");
    ts::List<int> integers;
    integers.pushFront(2);
    integers.pushFront(1);
    integers.pushBack(3);
    require(integers.eraseFirst(1) && integers.eraseFirst(3) && integers.front() == 2 && integers.back() == 2,
            "List endpoint erase");
    require(integers.popBack() == 2 && integers.empty(), "List last removal");
    throws([&] { integers.front(); });
    throws([&] { integers.popFront(); });
}

void adaptors() {
    ts::Queue<int> queue;
    ts::Stack<int> stack;
    ts::Deque<int> deque;
    for (int i = 0; i < 20000; ++i) {
        queue.push(i);
        stack.push(i);
        deque.pushBack(i);
    }
    for (int i = 0; i < 20000; ++i) {
        require(queue.pop() == i, "FIFO");
        require(stack.pop() == 19999 - i, "LIFO");
    }
    for (int i = 0; i < 10000; ++i) {
        require(deque.popFront() == i, "Deque front");
        require(deque.popBack() == 19999 - i, "Deque back");
    }
    require(deque.empty(), "Deque endpoints");
    throws([&] { deque.popBack(); });
    throws([&] { stack.top(); });
    throws([&] { queue.front(); });
    ts::Deque<std::unique_ptr<int>> moveOnly;
    moveOnly.pushBack(std::make_unique<int>(7));
    moveOnly.pushFront(std::make_unique<int>(8));
    require(*moveOnly.popFront() == 8 && *moveOnly.popBack() == 7, "Move-only Deque");
}

void maps() {
    ts::Map<int, Tracked> map;
    for (int i = 0; i < 20; ++i)
        map.insertOrAssign(i, Tracked{i});
    const int before = Tracked::alive;
    Tracked::copies = 0;
    require(map.erase(10) && !map.erase(30) && map.size() == 19, "Map erase must not copy other entries");
    require(Tracked::alive == before - 1 && map.at(11).value == 11, "Map erase preserves unrelated objects");
    Tracked::copies = -1;
    ts::Map<int, int> large;
    for (int i = 0; i < 5000; ++i)
        large.insertOrAssign(i, i * 2);
    for (int i = 0; i < 5000; ++i)
        require(large.at(i) == i * 2, "Map stress lookup");
    large.insertOrAssign(2, 7);
    require(large.size() == 5000 && large.at(2) == 7, "Map replace");
    throws([&] { large.at(-1); });
}

void heaps() {
    auto budget = std::make_shared<int>(-1);
    ts::BinaryHeap<int, ThrowCompare> heap(ThrowCompare{budget});
    for (int i = 99; i >= 0; --i)
        heap.push(i);
    *budget = 0;
    throws([&] { heap.push(-1); });
    require(heap.size() == 100 && heap.top() == 0, "Heap insertion comparison rollback");
    throws([&] { heap.pop(); });
    require(heap.size() == 100 && heap.top() == 0, "Heap removal comparison rollback");
    *budget = -1;
    for (int i = 0; i < 100; ++i)
        require(heap.pop() == i, "Heap remains ordered after exception");
    throws([&] { heap.pop(); });
    throws([&] { heap.top(); });
    ts::BinaryHeap<int> duplicates;
    for (int i = 0; i < 30000; ++i)
        duplicates.push(i % 17);
    int previous = -1;
    while (!duplicates.empty()) {
        const int value = duplicates.pop();
        require(value >= previous, "Heap duplicates ordering");
        previous = value;
    }
}
}

int main() {
    try {
        arrays();
        require(Tracked::alive == 0, "Array leak");
        lists();
        require(Tracked::alive == 0, "List leak");
        adaptors();
        maps();
        require(Tracked::alive == 0, "Map leak");
        heaps();
        std::cout << "PASS: seven structures, alignment, aliasing, 50k stress, exceptions and lifetime "
                     "accounting\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

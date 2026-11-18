#pragma once

#include <cstddef>
#include <functional>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ts {

template <class T> class Array {
    T* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;

    static T* allocate(std::size_t n) {
        if (!n)
            return nullptr;
        if (n > static_cast<std::size_t>(-1) / sizeof(T))
            throw std::length_error("Array allocation overflow");
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            return static_cast<T*>(::operator new(sizeof(T) * n, std::align_val_t{alignof(T)}));
        else
            return static_cast<T*>(::operator new(sizeof(T) * n));
    }

    static void deallocate(T* p) noexcept {
        if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            ::operator delete(p, std::align_val_t{alignof(T)});
        else
            ::operator delete(p);
    }

  public:
    Array() = default;

    explicit Array(std::size_t count, const T& value = T()) {
        try {
            reserve(count);
            for (std::size_t i = 0; i < count; ++i)
                pushBack(value);
        } catch (...) {
            clear();
            deallocate(data_);
            throw;
        }
    }

    Array(const Array& other) {
        try {
            reserve(other.size_);
            for (const auto& v : other)
                pushBack(v);
        } catch (...) {
            clear();
            deallocate(data_);
            throw;
        }
    }

    Array(Array&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {
    }

    Array& operator=(const Array& other) {
        if (this != &other) {
            Array temp(other);
            swap(temp);
        }
        return *this;
    }

    Array& operator=(Array&& other) noexcept {
        if (this != &other) {
            clear();
            deallocate(data_);
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, 0);
        }
        return *this;
    }

    ~Array() {
        clear();
        deallocate(data_);
    }

    void swap(Array& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
    }

    std::size_t size() const noexcept {
        return size_;
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

    T* begin() noexcept {
        return data_;
    }

    T* end() noexcept {
        return data_ ? data_ + size_ : nullptr;
    }

    const T* begin() const noexcept {
        return data_;
    }

    const T* end() const noexcept {
        return data_ ? data_ + size_ : nullptr;
    }

    T& operator[](std::size_t i) noexcept {
        return data_[i];
    }

    const T& operator[](std::size_t i) const noexcept {
        return data_[i];
    }

    T& at(std::size_t i) {
        if (i >= size_)
            throw std::out_of_range("Array::at");
        return data_[i];
    }

    const T& at(std::size_t i) const {
        if (i >= size_)
            throw std::out_of_range("Array::at");
        return data_[i];
    }

    T& back() {
        return at(size_ - 1);
    }

    const T& back() const {
        return at(size_ - 1);
    }

    void reserve(std::size_t newCap) {
        if (newCap <= capacity_)
            return;
        if (newCap > static_cast<std::size_t>(-1) / sizeof(T))
            throw std::length_error("Array allocation overflow");
        T* target = allocate(newCap);
        std::size_t i = 0;
        try {
            for (; i < size_; ++i)
                new (target + i) T(std::move_if_noexcept(data_[i]));
        } catch (...) {
            while (i)
                target[--i].~T();
            deallocate(target);
            throw;
        }
        for (std::size_t j = 0; j < size_; ++j)
            data_[j].~T();
        deallocate(data_);
        data_ = target;
        capacity_ = newCap;
    }

    template <typename... Args> T& emplaceBack(Args&&... args) {
        if (size_ < capacity_) {
            new (data_ + size_) T(std::forward<Args>(args)...);
            return data_[size_++];
        }
        const std::size_t limit = static_cast<std::size_t>(-1) / sizeof(T);
        if (size_ == limit)
            throw std::length_error("Array capacity overflow");
        const std::size_t next =
            capacity_ ? (capacity_ > limit / 2 ? limit : capacity_ * 2) : (limit < 4 ? limit : 4);
        T* target = allocate(next);
        std::size_t moved = 0;
        bool appended = false;
        try {

            new (target + size_) T(std::forward<Args>(args)...);
            appended = true;
            for (; moved < size_; ++moved)
                new (target + moved) T(std::move_if_noexcept(data_[moved]));
        } catch (...) {
            while (moved)
                target[--moved].~T();
            if (appended)
                target[size_].~T();
            deallocate(target);
            throw;
        }
        for (std::size_t i = 0; i < size_; ++i)
            data_[i].~T();
        deallocate(data_);
        data_ = target;
        capacity_ = next;
        return data_[size_++];
    }

    void pushBack(const T& value) {
        emplaceBack(value);
    }

    void pushBack(T&& value) {
        emplaceBack(std::move(value));
    }

    void popBack() {
        if (empty())
            throw std::out_of_range("Array::popBack");
        data_[--size_].~T();
    }

    void clear() noexcept {
        while (size_)
            data_[--size_].~T();
    }

    void eraseAt(std::size_t index) {
        if (index >= size_)
            throw std::out_of_range("Array::eraseAt");
        for (std::size_t i = index; i + 1 < size_; ++i)
            data_[i] = std::move(data_[i + 1]);
        popBack();
    }
};

template <class T, class Compare = std::less<T>> class BinaryHeap {
    Array<T> nodes_;
    Compare before_{};

    template <class U> void insert(U&& value) {
        static_assert(std::is_nothrow_swappable_v<T>, "BinaryHeap elements must support noexcept swap");
        nodes_.pushBack(std::forward<U>(value));
        const std::size_t added = nodes_.size() - 1;
        std::size_t destination = added;
        try {
            while (destination > 0) {
                const auto parent = (destination - 1) / 2;
                if (!before_(nodes_[added], nodes_[parent]))
                    break;
                destination = parent;
            }
        } catch (...) {
            nodes_.popBack();
            throw;
        }
        for (std::size_t at = added; at > destination; at = (at - 1) / 2)
            std::swap(nodes_[at], nodes_[(at - 1) / 2]);
    }

  public:
    BinaryHeap() = default;

    explicit BinaryHeap(Compare comparison) : before_(std::move(comparison)) {
    }

    bool empty() const noexcept {
        return nodes_.empty();
    }

    std::size_t size() const noexcept {
        return nodes_.size();
    }

    void clear() noexcept {
        nodes_.clear();
    }

    const T& top() const {
        if (empty())
            throw std::out_of_range("BinaryHeap::top");
        return nodes_[0];
    }

    void push(const T& value) {
        insert(value);
    }

    void push(T&& value) {
        insert(std::move(value));
    }

    T pop() {
        if (empty())
            throw std::out_of_range("BinaryHeap::pop");
        static_assert(std::is_nothrow_swappable_v<T> && std::is_nothrow_move_constructible_v<T>,
                      "BinaryHeap removal requires noexcept element move and swap");
        const std::size_t count = nodes_.size() - 1;
        std::size_t path[sizeof(std::size_t) * 8]{};
        std::size_t length = 0, at = 0;
        while (at < count) {
            const std::size_t left = at * 2 + 1;
            if (left >= count)
                break;
            const std::size_t right = left + 1;
            const std::size_t child = right < count && before_(nodes_[right], nodes_[left]) ? right : left;
            if (!before_(nodes_[child], nodes_[count]))
                break;
            path[length++] = child;
            at = child;
        }
        T result = std::move(nodes_[0]);
        if (count > 0)
            std::swap(nodes_[0], nodes_[count]);
        nodes_.popBack();
        at = 0;
        for (std::size_t i = 0; i < length; ++i) {
            std::swap(nodes_[at], nodes_[path[i]]);
            at = path[i];
        }
        return result;
    }
};

template <class T> class List {
    struct Node {
        T value;
        Node* prev = nullptr;
        Node* next = nullptr;

        template <typename... A> explicit Node(A&&... args) : value(std::forward<A>(args)...) {
        }
    };

    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    std::size_t size_ = 0;

  public:
    class iterator {
        Node* p_ = nullptr;

      public:
        explicit iterator(Node* p) : p_(p) {
        }

        T& operator*() const {
            return p_->value;
        }

        iterator& operator++() {
            p_ = p_->next;
            return *this;
        }

        bool operator!=(const iterator& other) const {
            return p_ != other.p_;
        }

        bool operator==(const iterator& other) const {
            return p_ == other.p_;
        }
    };

    class const_iterator {
        const Node* p_ = nullptr;

      public:
        explicit const_iterator(const Node* p) : p_(p) {
        }

        const T& operator*() const {
            return p_->value;
        }

        const_iterator& operator++() {
            p_ = p_->next;
            return *this;
        }

        bool operator!=(const const_iterator& other) const {
            return p_ != other.p_;
        }
    };

    List() = default;

    List(const List& other) {
        try {
            for (const auto& x : other)
                pushBack(x);
        } catch (...) {
            clear();
            throw;
        }
    }

    List(List&& other) noexcept
        : head_(std::exchange(other.head_, nullptr)), tail_(std::exchange(other.tail_, nullptr)),
          size_(std::exchange(other.size_, 0)) {
    }

    List& operator=(const List& other) {
        if (this != &other) {
            List tmp(other);
            swap(tmp);
        }
        return *this;
    }

    List& operator=(List&& other) noexcept {
        if (this != &other) {
            clear();
            head_ = std::exchange(other.head_, nullptr);
            tail_ = std::exchange(other.tail_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~List() {
        clear();
    }

    void swap(List& other) noexcept {
        std::swap(head_, other.head_);
        std::swap(tail_, other.tail_);
        std::swap(size_, other.size_);
    }

    bool empty() const noexcept {
        return !head_;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    iterator begin() {
        return iterator(head_);
    }

    iterator end() {
        return iterator(nullptr);
    }

    const_iterator begin() const {
        return const_iterator(head_);
    }

    const_iterator end() const {
        return const_iterator(nullptr);
    }

    T& front() {
        if (!head_)
            throw std::out_of_range("List::front");
        return head_->value;
    }

    const T& front() const {
        if (!head_)
            throw std::out_of_range("List::front");
        return head_->value;
    }

    T& back() {
        if (!tail_)
            throw std::out_of_range("List::back");
        return tail_->value;
    }

    const T& back() const {
        if (!tail_)
            throw std::out_of_range("List::back");
        return tail_->value;
    }

    template <class... A> void emplaceBack(A&&... args) {
        Node* n = new Node(std::forward<A>(args)...);
        n->prev = tail_;
        if (tail_)
            tail_->next = n;
        else
            head_ = n;
        tail_ = n;
        ++size_;
    }

    template <class... A> void emplaceFront(A&&... args) {
        Node* n = new Node(std::forward<A>(args)...);
        n->next = head_;
        if (head_)
            head_->prev = n;
        else
            tail_ = n;
        head_ = n;
        ++size_;
    }

    void pushBack(const T& v) {
        emplaceBack(v);
    }

    void pushBack(T&& v) {
        emplaceBack(std::move(v));
    }

    void pushFront(const T& v) {
        emplaceFront(v);
    }

    void pushFront(T&& v) {
        emplaceFront(std::move(v));
    }

    T popFront() {
        if (!head_)
            throw std::out_of_range("List::popFront");
        Node* n = head_;
        T v = std::move_if_noexcept(n->value);
        head_ = n->next;
        if (head_)
            head_->prev = nullptr;
        else
            tail_ = nullptr;
        delete n;
        --size_;
        return v;
    }

    T popBack() {
        if (!tail_)
            throw std::out_of_range("List::popBack");
        Node* n = tail_;
        T v = std::move_if_noexcept(n->value);
        tail_ = n->prev;
        if (tail_)
            tail_->next = nullptr;
        else
            head_ = nullptr;
        delete n;
        --size_;
        return v;
    }

    template <class Predicate> bool eraseFirstIf(Predicate predicate) {
        for (Node* p = head_; p; p = p->next)
            if (predicate(p->value)) {
                if (p->prev)
                    p->prev->next = p->next;
                else
                    head_ = p->next;
                if (p->next)
                    p->next->prev = p->prev;
                else
                    tail_ = p->prev;
                delete p;
                --size_;
                return true;
            }
        return false;
    }

    bool eraseFirst(const T& value) {
        return eraseFirstIf([&](const T& item) { return item == value; });
    }

    void clear() noexcept {
        while (head_) {
            Node* next = head_->next;
            delete head_;
            head_ = next;
        }
        tail_ = nullptr;
        size_ = 0;
    }
};

template <class T> class Queue {
    List<T> data_;

  public:
    void push(const T& v) {
        data_.pushBack(v);
    }

    void push(T&& v) {
        data_.pushBack(std::move(v));
    }

    T pop() {
        return data_.popFront();
    }

    const T& front() const {
        return data_.front();
    }

    bool empty() const noexcept {
        return data_.empty();
    }

    std::size_t size() const noexcept {
        return data_.size();
    }

    void clear() noexcept {
        data_.clear();
    }
};

template <class T> class Stack {
    List<T> data_;

  public:
    void push(const T& v) {
        data_.pushBack(v);
    }

    void push(T&& v) {
        data_.pushBack(std::move(v));
    }

    T pop() {
        return data_.popBack();
    }

    T& top() {
        return data_.back();
    }

    const T& top() const {
        return data_.back();
    }

    bool empty() const noexcept {
        return data_.empty();
    }

    std::size_t size() const noexcept {
        return data_.size();
    }
};

template <class T> class Deque {
    List<T> data_;

  public:
    void pushFront(const T& v) {
        data_.pushFront(v);
    }

    void pushFront(T&& v) {
        data_.pushFront(std::move(v));
    }

    void pushBack(const T& v) {
        data_.pushBack(v);
    }

    void pushBack(T&& v) {
        data_.pushBack(std::move(v));
    }

    T popFront() {
        return data_.popFront();
    }

    T popBack() {
        return data_.popBack();
    }

    bool empty() const noexcept {
        return data_.empty();
    }

    std::size_t size() const noexcept {
        return data_.size();
    }

    void clear() noexcept {
        data_.clear();
    }
};

template <class K, class V> class Map {
  public:
    struct Entry {
        K key;
        V value;
    };

  private:
    List<Entry> data_;

  public:
    std::size_t size() const noexcept {
        return data_.size();
    }

    bool empty() const noexcept {
        return data_.empty();
    }

    V* find(const K& k) {
        for (auto& e : data_)
            if (e.key == k)
                return &e.value;
        return nullptr;
    }

    const V* find(const K& k) const {
        for (const auto& e : data_)
            if (e.key == k)
                return &e.value;
        return nullptr;
    }

    bool contains(const K& k) const {
        return find(k) != nullptr;
    }

    void insertOrAssign(const K& k, const V& v) {
        if (auto* p = find(k))
            *p = v;
        else
            data_.pushBack(Entry{k, v});
    }

    void insertOrAssign(const K& k, V&& v) {
        if (auto* p = find(k))
            *p = std::move(v);
        else
            data_.pushBack(Entry{k, std::move(v)});
    }

    V& at(const K& k) {
        auto* p = find(k);
        if (!p)
            throw std::out_of_range("Map::at");
        return *p;
    }

    const V& at(const K& k) const {
        auto* p = find(k);
        if (!p)
            throw std::out_of_range("Map::at");
        return *p;
    }

    bool erase(const K& key) {
        return data_.eraseFirstIf([&](const Entry& entry) { return entry.key == key; });
    }

    void clear() noexcept {
        data_.clear();
    }

    auto begin() {
        return data_.begin();
    }

    auto end() {
        return data_.end();
    }

    auto begin() const {
        return data_.begin();
    }

    auto end() const {
        return data_.end();
    }
};
}

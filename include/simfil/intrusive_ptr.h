#pragma once

#include <atomic>

namespace simfil
{

template <class Derived>
struct ref_counted
{
    ref_counted() noexcept
        : ref_(0)
    {}

    ref_counted(const ref_counted& other) noexcept
        : ref_(0)
    {}

    auto operator=(const ref_counted&) -> ref_counted&
    {
        return *this;
    }

    ~ref_counted() = default;

    auto refcount() const
    {
        return ref_.load();
    }

    mutable std::atomic<uint32_t> ref_{0};
};

template <class Object>
struct intrusive_ptr
{
    using ptr_type = Object*;

    auto ref(const ptr_type obj)
    {
        ++obj->ref_;
    }

    auto deref(const ptr_type obj) -> ptr_type
    {
        if ((--obj->ref_) == 0) {
            delete obj;
            return nullptr;
        }
        return obj;
    }

    intrusive_ptr()
        : ptr_(nullptr)
    {}

    intrusive_ptr(ptr_type ptr, bool add_ref = true)
        : ptr_(ptr)
    {
        if (ptr_ && add_ref) {
            ref(ptr_);
        }
    }

    template <class Other>
    intrusive_ptr(const intrusive_ptr<Other>& other)
        : ptr_(other.ptr_)
    {
        if (ptr_) ref(ptr_);
    }

    intrusive_ptr(const intrusive_ptr& other)
        : ptr_(other.ptr_)
    {
        if (ptr_) ref(ptr_);
    }

    template <class Other>
    intrusive_ptr(intrusive_ptr<Other>&& other)
        : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    intrusive_ptr(intrusive_ptr&& other)
        : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    template <class Other>
    auto operator=(const intrusive_ptr<Other>& other) -> intrusive_ptr&
    {
        intrusive_ptr<Object>(other).swap(*this);
        return *this;
    }

    template <class Other>
    auto operator=(intrusive_ptr<Other>&& other) -> intrusive_ptr&
    {
        intrusive_ptr<Object>(std::move(other)).swap(*this);
        return *this;
    }

    auto operator=(const intrusive_ptr& other) -> intrusive_ptr&
    {
        intrusive_ptr<Object>(other).swap(*this);
        return *this;
    }

    auto operator=(intrusive_ptr&& other) -> intrusive_ptr&
    {
        intrusive_ptr<Object>(std::move(other)).swap(*this);
        return *this;
    }

    ~intrusive_ptr()
    {
        if (ptr_) ptr_ = deref(ptr_);
    }

    auto get() -> ptr_type
    {
        return ptr_;
    }

    auto get() const -> const ptr_type
    {
        return ptr_;
    }

    auto swap(intrusive_ptr& other)
    {
        auto* tmp = other.ptr_;
        other.ptr_ = ptr_;
        ptr_ = tmp;
    }

    auto reset(const ptr_type ptr, bool add_ref = true)
    {
        intrusive_ptr<Object>(ptr, add_ref).swap(*this);
    }

    operator bool() const
    {
        return ptr_ != nullptr;
    }

    auto operator->() {return ptr_;}
    auto operator->() const {return ptr_;}

    auto operator*() -> Object& {return *ptr_;}
    auto operator*() const -> Object& {return *ptr_;}

    ptr_type ptr_{nullptr};
};

/**
 * Return new intrusive pointer instance.
 */
template <class Object, class... Args>
auto make_intrusive(Args&& ...args)
{
    return intrusive_ptr<Object>(new Object(std::forward<Args>(args)...),
                                 true);
}

}

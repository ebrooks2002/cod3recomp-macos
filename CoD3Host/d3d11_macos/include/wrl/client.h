#pragma once

#include "d3dcommon.h"
#include <utility>

namespace Microsoft {
namespace WRL {

template <typename T>
class ComPtr {
public:
    typedef T InterfaceType;

    ComPtr() noexcept : ptr_(nullptr) {}
    ComPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}

    template <class U>
    ComPtr(U* other) noexcept : ptr_(other) {
        InternalAddRef();
    }

    ComPtr(const ComPtr& other) noexcept : ptr_(other.ptr_) {
        InternalAddRef();
    }

    template <class U>
    ComPtr(const ComPtr<U>& other) noexcept : ptr_(other.Get()) {
        InternalAddRef();
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    template <class U>
    ComPtr(ComPtr<U>&& other) noexcept : ptr_(other.Detach()) {}

    ~ComPtr() noexcept {
        InternalRelease();
    }

    ComPtr& operator=(std::nullptr_t) noexcept {
        InternalRelease();
        return *this;
    }

    ComPtr& operator=(T* other) noexcept {
        if (ptr_ != other) {
            if (other) other->AddRef();
            InternalRelease();
            ptr_ = other;
        }
        return *this;
    }

    template <typename U>
    ComPtr& operator=(U* other) noexcept {
        if (ptr_ != other) {
            if (other) other->AddRef();
            InternalRelease();
            ptr_ = other;
        }
        return *this;
    }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (ptr_ != other.ptr_) {
            if (other.ptr_) other.ptr_->AddRef();
            InternalRelease();
            ptr_ = other.ptr_;
        }
        return *this;
    }

    template <class U>
    ComPtr& operator=(const ComPtr<U>& other) noexcept {
        if (ptr_ != other.Get()) {
            if (other.Get()) other.Get()->AddRef();
            InternalRelease();
            ptr_ = other.Get();
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != reinterpret_cast<const ComPtr*>(&reinterpret_cast<const char&>(other))) {
            InternalRelease();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    template <class U>
    ComPtr& operator=(ComPtr<U>&& other) noexcept {
        InternalRelease();
        ptr_ = other.Detach();
        return *this;
    }

    explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

    T* Get() const noexcept {
        return ptr_;
    }

    T* operator->() const noexcept {
        return ptr_;
    }

    T* const* GetAddressOf() const noexcept {
        return &ptr_;
    }

    T** GetAddressOf() noexcept {
        return &ptr_;
    }

    T** ReleaseAndGetAddressOf() noexcept {
        InternalRelease();
        return &ptr_;
    }

    T** operator&() noexcept {
        return ReleaseAndGetAddressOf();
    }

    T* const* operator&() const noexcept {
        return GetAddressOf();
    }

    T* Detach() noexcept {
        T* ptr = ptr_;
        ptr_ = nullptr;
        return ptr;
    }

    void Attach(InterfaceType* other) noexcept {
        if (ptr_ != nullptr) {
            ptr_->Release();
        }
        ptr_ = other;
    }

    unsigned long Reset() noexcept {
        return InternalRelease();
    }

    template <typename U>
    HRESULT As(ComPtr<U>* p) const noexcept {
        if (!p) return E_INVALIDARG;
        return ptr_ ? ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(p->ReleaseAndGetAddressOf())) : E_FAIL;
    }

    template <typename U>
    HRESULT As(U** p) const noexcept {
        if (!p) return E_INVALIDARG;
        return ptr_ ? ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(p)) : E_FAIL;
    }

    bool operator==(std::nullptr_t) const noexcept { return ptr_ == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return ptr_ != nullptr; }
    bool operator==(const ComPtr<T>& o) const noexcept { return ptr_ == o.ptr_; }
    bool operator!=(const ComPtr<T>& o) const noexcept { return ptr_ != o.ptr_; }
    bool operator==(T* p) const noexcept { return ptr_ == p; }
    bool operator!=(T* p) const noexcept { return ptr_ != p; }

protected:
    InterfaceType* ptr_;

    void InternalAddRef() const noexcept {
        if (ptr_ != nullptr) {
            ptr_->AddRef();
        }
    }

    unsigned long InternalRelease() noexcept {
        unsigned long ref = 0;
        T* temp = ptr_;
        if (temp != nullptr) {
            ptr_ = nullptr;
            ref = temp->Release();
        }
        return ref;
    }
};

} // namespace WRL
} // namespace Microsoft

# Caver Memory Runtime & Smart Pointer Infrastructure

## 1. System Overview & Purpose

The decompiled Ghidra codebase reveals that Swordigo heavily relies on custom Boost smart pointer variants (`boost::shared_ptr`, `sp_counted_impl_p`) and custom fast vector dynamic allocation containers (`FastVector_Caver_*`).

Understanding this memory runtime is critical for ensuring memory safety, zero-leak scene reloads, and proper ownership semantics in the C++ PC rewrite and native port (`swd`).

---

## 2. Namespace & Symbol Structure

```
boost::
 ├── shared_ptr<T> (Reference-Counted Smart Pointer Container)
 └── detail::
      └── sp_counted_impl_p<T> (Atomic Reference Counter Control Block)

Caver::
 ├── FastVector<T> (Low-Overhead Dynamic Array Container)
 ├── shared_count (Reference Counter Manager)
 └── MemoryPool (Static / Dynamic Block Allocator)
```

---

## 3. Reference Counting Mechanism (`sp_counted_impl_p`)

In the decompiled C source, shared pointer blocks are represented by `sp_counted_impl_p_Caver_<Class>_`.

### Control Block Layout & Allocation Pattern
Each `shared_ptr` consists of two main elements:
1. **Pointee Pointer**: Direct pointer to the managed object instance.
2. **Control Block Pointer**: Points to a heap-allocated `sp_counted_impl_p` structure containing:
   - **Use Count**: Strong reference count. Controls managed object destruction.
   - **Weak Count**: Weak reference count. Controls control block deletion.
   - **Virtual Destructor Table**: Provides polymorphic object cleanup.

### Decompiled Increment / Decrement Pattern
```cpp
// Decompiled reference increment pattern (Exclusive Monitor Atomic / Interlocked)
void AcquireSharedPtrRef(sp_counted_impl* impl) {
    if (impl != nullptr) {
        atomic_increment(&impl->use_count);
    }
}

// Decompiled reference decrement & release pattern
void ReleaseSharedPtrRef(sp_counted_impl* impl) {
    if (impl != nullptr) {
        if (atomic_decrement(&impl->use_count) == 0) {
            impl->dispose_object(); // Invokes virtual destructor
            if (atomic_decrement(&impl->weak_count) == 0) {
                impl->destroy_self();  // Frees control block
            }
        }
    }
}
```

---

## 4. FastVector Dynamic Array Implementation

Swordigo replaces standard `std::vector` in performance-critical sections (such as particle arrays, entity component lists, and vertex buffers) with `FastVector`.

### Key Characteristics:
- **No Exception Safety Overhead**: Stripped bounds checking and exception handling for mobile ARM performance.
- **Trivial Copying**: Uses `memcpy`/`memmove` for pod types (like vertices and matrix arrays).
- **In-Place Growth**: Exponential growth strategy ($1.5\times$ expansion factor) to reduce reallocation frequency.

---

## 5. Reverse Engineering Cross-References

- **GlossHook Hooks**: GlossHook target functions intercept `shared_count` constructor and destructor calls to monitor object leaks and inject custom memory allocators.
- **Native SDK Integration**: Memory allocations interface with platform-native heap routines (`malloc`/`free` or custom arena allocators).

---

## 6. PC Port (`swd`) Implementation & Modernization Plan

1. **Standardize to `std::shared_ptr`**: Replace all legacy `boost::shared_ptr` and custom `sp_counted_impl_p` implementations directly with standard C++17/C++20 `std::shared_ptr` and `std::weak_ptr`.
2. **Standardize to `std::vector` with Reserve**: Replace `FastVector` with `std::vector`, utilizing `.reserve()` calls during scene initialization to match zero-reallocation performance.
3. **RAII Validation**: Ensure scene teardown routines call `.reset()` on all active view controllers, GUI views, and component outlets to prevent cyclic references.

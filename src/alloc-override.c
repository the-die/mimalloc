/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#if !defined(MI_IN_ALLOC_C)
#error "this file should be included from 'alloc.c' (so aliases can work)"
#endif

#if defined(MI_MALLOC_OVERRIDE) && defined(_WIN32) && !(defined(MI_SHARED_LIB) && defined(_DLL))
#error "It is only possible to override "malloc" on Windows when building as a DLL (and linking the C runtime as a DLL)"
#endif

#if defined(MI_MALLOC_OVERRIDE) && !(defined(_WIN32))

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
mi_decl_externc void   vfree(void* p);
mi_decl_externc size_t malloc_size(const void* p);
mi_decl_externc size_t malloc_good_size(size_t size);
#endif

// helper definition for C override of C++ new
typedef void* mi_nothrow_t;

// ------------------------------------------------------
// Override system malloc
// ------------------------------------------------------

#if (defined(__GNUC__) || defined(__clang__)) && !defined(__APPLE__) && !MI_TRACK_ENABLED
  // gcc, clang: use aliasing to alias the exported function to one of our `mi_` functions
  #if (defined(__GNUC__) && __GNUC__ >= 9)
    // https://gcc.gnu.org/onlinedocs/gcc/Diagnostic-Pragmas.html
    #pragma GCC diagnostic ignored "-Wattributes"  // or we get warnings that nodiscard is ignored on a forward
    // https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html
    // alias ("target")
    //    The alias variable attribute causes the declaration to be emitted as an alias for another
    //    symbol known as an alias target. Except for top-level qualifiers the alias target must
    //    have the same type as the alias.
    //
    // used
    //   This attribute, attached to a variable with static storage, means that the variable must be
    //   emitted even if it appears that the variable is not referenced.
    //
    //   When applied to a static data member of a C++ class template, the attribute also means that
    //   the member is instantiated if the class itself is instantiated.
    //
    // copy
    // copy (variable)
    //    The copy attribute applies the set of attributes with which variable has been declared to
    //    the declaration of the variable to which the attribute is applied. The attribute is
    //    designed for libraries that define aliases that are expected to specify the same set of
    //    attributes as the aliased symbols. The copy attribute can be used with variables,
    //    functions or types. However, the kind of symbol to which the attribute is applied (either
    //    varible or function) must match the kind of symbol to which the argument refers. The copy
    //    attribute copies only syntactic and semantic attributes but not attributes that affect a
    //    symbol’s linkage or visibility such as alias, visibility, or weak. The deprecated
    //    attribute is also not copied.
    #define MI_FORWARD(fun)      __attribute__((alias(#fun), used, visibility("default"), copy(fun)));
  #else
    #define MI_FORWARD(fun)      __attribute__((alias(#fun), used, visibility("default")));
  #endif
  #define MI_FORWARD1(fun,x)      MI_FORWARD(fun)
  #define MI_FORWARD2(fun,x,y)    MI_FORWARD(fun)
  #define MI_FORWARD3(fun,x,y,z)  MI_FORWARD(fun)
  #define MI_FORWARD0(fun,x)      MI_FORWARD(fun)
  #define MI_FORWARD02(fun,x,y)   MI_FORWARD(fun)
#else
  // otherwise use forwarding by calling our `mi_` function
  #define MI_FORWARD1(fun,x)      { return fun(x); }
  #define MI_FORWARD2(fun,x,y)    { return fun(x,y); }
  #define MI_FORWARD3(fun,x,y,z)  { return fun(x,y,z); }
  #define MI_FORWARD0(fun,x)      { fun(x); }
  #define MI_FORWARD02(fun,x,y)   { fun(x,y); }
#endif


#if defined(__APPLE__) && defined(MI_SHARED_LIB_EXPORT) && defined(MI_OSX_INTERPOSE)
  // define MI_OSX_IS_INTERPOSED as we should not provide forwarding definitions for
  // functions that are interposed (or the interposing does not work)
  #define MI_OSX_IS_INTERPOSED

  mi_decl_externc size_t mi_malloc_size_checked(void *p) {
    if (!mi_is_in_heap_region(p)) return 0;
    return mi_usable_size(p);
  }

  // use interposing so `DYLD_INSERT_LIBRARIES` works without `DYLD_FORCE_FLAT_NAMESPACE=1`
  // See: <https://books.google.com/books?id=K8vUkpOXhN4C&pg=PA73>
  struct mi_interpose_s {
    const void* replacement;
    const void* target;
  };
  #define MI_INTERPOSE_FUN(oldfun,newfun) { (const void*)&newfun, (const void*)&oldfun }
  #define MI_INTERPOSE_MI(fun)            MI_INTERPOSE_FUN(fun,mi_##fun)

  __attribute__((used)) static struct mi_interpose_s _mi_interposes[]  __attribute__((section("__DATA, __interpose"))) =
  {
    MI_INTERPOSE_MI(malloc),
    MI_INTERPOSE_MI(calloc),
    MI_INTERPOSE_MI(realloc),
    MI_INTERPOSE_MI(strdup),
    #if defined(MAC_OS_X_VERSION_10_7) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_7
    MI_INTERPOSE_MI(strndup),
    #endif
    MI_INTERPOSE_MI(realpath),
    MI_INTERPOSE_MI(posix_memalign),
    MI_INTERPOSE_MI(reallocf),
    MI_INTERPOSE_MI(valloc),
    MI_INTERPOSE_FUN(malloc_size,mi_malloc_size_checked),
    MI_INTERPOSE_MI(malloc_good_size),
    #if defined(MAC_OS_X_VERSION_10_15) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_15
    MI_INTERPOSE_MI(aligned_alloc),
    #endif
    #ifdef MI_OSX_ZONE
    // we interpose malloc_default_zone in alloc-override-osx.c so we can use mi_free safely
    MI_INTERPOSE_MI(free),
    MI_INTERPOSE_FUN(vfree,mi_free),
    #else
    // sometimes code allocates from default zone but deallocates using plain free :-( (like NxHashResizeToCapacity <https://github.com/nneonneo/osx-10.9-opensource/blob/master/objc4-551.1/runtime/hashtable2.mm>)
    MI_INTERPOSE_FUN(free,mi_cfree), // use safe free that checks if pointers are from us
    MI_INTERPOSE_FUN(vfree,mi_cfree),
    #endif
  };

  #ifdef __cplusplus
  extern "C" {
  #endif
  void  _ZdlPv(void* p);   // delete
  void  _ZdaPv(void* p);   // delete[]
  void  _ZdlPvm(void* p, size_t n);  // delete
  void  _ZdaPvm(void* p, size_t n);  // delete[]
  void* _Znwm(size_t n);  // new
  void* _Znam(size_t n);  // new[]
  void* _ZnwmRKSt9nothrow_t(size_t n, mi_nothrow_t tag); // new nothrow
  void* _ZnamRKSt9nothrow_t(size_t n, mi_nothrow_t tag); // new[] nothrow
  #ifdef __cplusplus
  }
  #endif
  __attribute__((used)) static struct mi_interpose_s _mi_cxx_interposes[]  __attribute__((section("__DATA, __interpose"))) =
  {
    MI_INTERPOSE_FUN(_ZdlPv,mi_free),
    MI_INTERPOSE_FUN(_ZdaPv,mi_free),
    MI_INTERPOSE_FUN(_ZdlPvm,mi_free_size),
    MI_INTERPOSE_FUN(_ZdaPvm,mi_free_size),
    MI_INTERPOSE_FUN(_Znwm,mi_new),
    MI_INTERPOSE_FUN(_Znam,mi_new),
    MI_INTERPOSE_FUN(_ZnwmRKSt9nothrow_t,mi_new_nothrow),
    MI_INTERPOSE_FUN(_ZnamRKSt9nothrow_t,mi_new_nothrow),
  };

#elif defined(_MSC_VER)
  // cannot override malloc unless using a dll.
  // we just override new/delete which does work in a static library.
#else
  // On all other systems forward allocation primitives to our API
  mi_decl_export void* malloc(size_t size)              MI_FORWARD1(mi_malloc, size)
  mi_decl_export void* calloc(size_t size, size_t n)    MI_FORWARD2(mi_calloc, size, n)
  mi_decl_export void* realloc(void* p, size_t newsize) MI_FORWARD2(mi_realloc, p, newsize)
  mi_decl_export void  free(void* p)                    MI_FORWARD0(mi_free, p)  
  // In principle we do not need to forward `strdup`/`strndup` but on some systems these do not use `malloc` internally (but a more primitive call)
  // We only override if `strdup` is not a macro (as on some older libc's, see issue #885)
  #if !defined(strdup)
  mi_decl_export char* strdup(const char* str)             MI_FORWARD1(mi_strdup, str)
  #endif
  #if !defined(strndup) && (!defined(__APPLE__) || (defined(MAC_OS_X_VERSION_10_7) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_7))
  mi_decl_export char* strndup(const char* str, size_t n)  MI_FORWARD2(mi_strndup, str, n)   
  #endif
#endif

#if (defined(__GNUC__) || defined(__clang__)) && !defined(__APPLE__)
// https://gcc.gnu.org/onlinedocs/gcc/Visibility-Pragmas.html
// #pragma GCC visibility push(visibility) ¶
// #pragma GCC visibility pop
//    This pragma allows the user to set the visibility for multiple declarations without having to
//    give each a visibility attribute (see Declaring Attributes of Functions).
//
//    In C++, ‘#pragma GCC visibility’ affects only namespace-scope declarations. Class members and
//    template specializations are not affected; if you want to override the visibility for a
//    particular member or instantiation, you must use an attribute.
#pragma GCC visibility push(default)
#endif

// ------------------------------------------------------
// Override new/delete
// This is not really necessary as they usually call
// malloc/free anyway, but it improves performance.
// ------------------------------------------------------
#ifdef __cplusplus
  // ------------------------------------------------------
  // With a C++ compiler we override the new/delete operators.
  // see <https://en.cppreference.com/w/cpp/memory/new/operator_new>
  // and <https://en.cppreference.com/w/cpp/memory/new/operator_delete>
  // ------------------------------------------------------
  #include <new>

  #ifndef MI_OSX_IS_INTERPOSED
    void operator delete(void* p) noexcept              MI_FORWARD0(mi_free,p)
    void operator delete[](void* p) noexcept            MI_FORWARD0(mi_free,p)

    void* operator new(std::size_t n) noexcept(false)   MI_FORWARD1(mi_new,n)
    void* operator new[](std::size_t n) noexcept(false) MI_FORWARD1(mi_new,n)

    void* operator new  (std::size_t n, const std::nothrow_t& tag) noexcept { MI_UNUSED(tag); return mi_new_nothrow(n); }
    void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept { MI_UNUSED(tag); return mi_new_nothrow(n); }

    #if (__cplusplus >= 201402L || _MSC_VER >= 1916)
    void operator delete  (void* p, std::size_t n) noexcept MI_FORWARD02(mi_free_size,p,n)
    void operator delete[](void* p, std::size_t n) noexcept MI_FORWARD02(mi_free_size,p,n)
    #endif
  #endif

  // https://en.cppreference.com/w/cpp/feature_test
  #if (__cplusplus > 201402L && defined(__cpp_aligned_new)) && (!defined(__GNUC__) || (__GNUC__ > 5))
  void operator delete  (void* p, std::align_val_t al) noexcept { mi_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete[](void* p, std::align_val_t al) noexcept { mi_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete  (void* p, std::size_t n, std::align_val_t al) noexcept { mi_free_size_aligned(p, n, static_cast<size_t>(al)); };
  void operator delete[](void* p, std::size_t n, std::align_val_t al) noexcept { mi_free_size_aligned(p, n, static_cast<size_t>(al)); };
  void operator delete  (void* p, std::align_val_t al, const std::nothrow_t&) noexcept { mi_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept { mi_free_aligned(p, static_cast<size_t>(al)); }

  void* operator new( std::size_t n, std::align_val_t al)   noexcept(false) { return mi_new_aligned(n, static_cast<size_t>(al)); }
  void* operator new[]( std::size_t n, std::align_val_t al) noexcept(false) { return mi_new_aligned(n, static_cast<size_t>(al)); }
  void* operator new  (std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return mi_new_aligned_nothrow(n, static_cast<size_t>(al)); }
  void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return mi_new_aligned_nothrow(n, static_cast<size_t>(al)); }
  #endif

#elif (defined(__GNUC__) || defined(__clang__))
  // ------------------------------------------------------
  // Override by defining the mangled C++ names of the operators (as
  // used by GCC and CLang).
  // See <https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling>
  // ------------------------------------------------------

  void _ZdlPv(void* p)            MI_FORWARD0(mi_free,p) // delete
  void _ZdaPv(void* p)            MI_FORWARD0(mi_free,p) // delete[]
  void _ZdlPvm(void* p, size_t n) MI_FORWARD02(mi_free_size,p,n)
  void _ZdaPvm(void* p, size_t n) MI_FORWARD02(mi_free_size,p,n)
  
  void _ZdlPvSt11align_val_t(void* p, size_t al)            { mi_free_aligned(p,al); }
  void _ZdaPvSt11align_val_t(void* p, size_t al)            { mi_free_aligned(p,al); }
  void _ZdlPvmSt11align_val_t(void* p, size_t n, size_t al) { mi_free_size_aligned(p,n,al); }
  void _ZdaPvmSt11align_val_t(void* p, size_t n, size_t al) { mi_free_size_aligned(p,n,al); }

  void _ZdlPvRKSt9nothrow_t(void* p, mi_nothrow_t tag)      { MI_UNUSED(tag); mi_free(p); }  // operator delete(void*, std::nothrow_t const&) 
  void _ZdaPvRKSt9nothrow_t(void* p, mi_nothrow_t tag)      { MI_UNUSED(tag); mi_free(p); }  // operator delete[](void*, std::nothrow_t const&)
  void _ZdlPvSt11align_val_tRKSt9nothrow_t(void* p, size_t al, mi_nothrow_t tag) { MI_UNUSED(tag); mi_free_aligned(p,al); } // operator delete(void*, std::align_val_t, std::nothrow_t const&) 
  void _ZdaPvSt11align_val_tRKSt9nothrow_t(void* p, size_t al, mi_nothrow_t tag) { MI_UNUSED(tag); mi_free_aligned(p,al); } // operator delete[](void*, std::align_val_t, std::nothrow_t const&) 
  
  #if (MI_INTPTR_SIZE==8)
    void* _Znwm(size_t n)                             MI_FORWARD1(mi_new,n)  // new 64-bit
    void* _Znam(size_t n)                             MI_FORWARD1(mi_new,n)  // new[] 64-bit
    void* _ZnwmRKSt9nothrow_t(size_t n, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_nothrow(n); }
    void* _ZnamRKSt9nothrow_t(size_t n, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_nothrow(n); }
    void* _ZnwmSt11align_val_t(size_t n, size_t al)   MI_FORWARD2(mi_new_aligned, n, al)
    void* _ZnamSt11align_val_t(size_t n, size_t al)   MI_FORWARD2(mi_new_aligned, n, al)
    void* _ZnwmSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_aligned_nothrow(n,al); }
    void* _ZnamSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_aligned_nothrow(n,al); }
  #elif (MI_INTPTR_SIZE==4)
    void* _Znwj(size_t n)                             MI_FORWARD1(mi_new,n)  // new 64-bit
    void* _Znaj(size_t n)                             MI_FORWARD1(mi_new,n)  // new[] 64-bit
    void* _ZnwjRKSt9nothrow_t(size_t n, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_nothrow(n); }
    void* _ZnajRKSt9nothrow_t(size_t n, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_nothrow(n); }
    void* _ZnwjSt11align_val_t(size_t n, size_t al)   MI_FORWARD2(mi_new_aligned, n, al)
    void* _ZnajSt11align_val_t(size_t n, size_t al)   MI_FORWARD2(mi_new_aligned, n, al)
    void* _ZnwjSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_aligned_nothrow(n,al); }
    void* _ZnajSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, mi_nothrow_t tag) { MI_UNUSED(tag); return mi_new_aligned_nothrow(n,al); }
  #else
    #error "define overloads for new/delete for this platform (just for performance, can be skipped)"
  #endif
#endif // __cplusplus

// ------------------------------------------------------
// Further Posix & Unix functions definitions
// ------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MI_OSX_IS_INTERPOSED
  // Forward Posix/Unix calls as well
  // https://man.freebsd.org/cgi/man.cgi?query=reallocf
  void*  reallocf(void* p, size_t newsize) MI_FORWARD2(mi_reallocf,p,newsize)
  // https://www.unix.com/man-page/osx/3/malloc_size/
  size_t malloc_size(const void* p)        MI_FORWARD1(mi_usable_size,p)
  #if !defined(__ANDROID__) && !defined(__FreeBSD__)
  // https://man7.org/linux/man-pages/man3/malloc_usable_size.3.html
  size_t malloc_usable_size(void *p)       MI_FORWARD1(mi_usable_size,p)
  #else
  // https://man.freebsd.org/cgi/man.cgi?query=malloc
  size_t malloc_usable_size(const void *p) MI_FORWARD1(mi_usable_size,p)
  #endif

  // No forwarding here due to aliasing/name mangling issues
  // https://man7.org/linux/man-pages/man3/posix_memalign.3.html
  void*  valloc(size_t size)               { return mi_valloc(size); }
  // https://www.unix.com/man-page/redhat/9/vfree
  void   vfree(void* p)                    { mi_free(p); }
  // https://www.unix.com/man-page/osx/3/malloc_good_size
  size_t malloc_good_size(size_t size)     { return mi_malloc_good_size(size); }
  // https://man7.org/linux/man-pages/man3/posix_memalign.3.html
  int    posix_memalign(void** p, size_t alignment, size_t size) { return mi_posix_memalign(p, alignment, size); }

  // `aligned_alloc` is only available when __USE_ISOC11 is defined.
  // Note: it seems __USE_ISOC11 is not defined in musl (and perhaps other libc's) so we only check
  // for it if using glibc.
  // Note: Conda has a custom glibc where `aligned_alloc` is declared `static inline` and we cannot
  // override it, but both _ISOC11_SOURCE and __USE_ISOC11 are undefined in Conda GCC7 or GCC9.
  // Fortunately, in the case where `aligned_alloc` is declared as `static inline` it
  // uses internally `memalign`, `posix_memalign`, or `_aligned_malloc` so we  can avoid overriding it ourselves.
  #if !defined(__GLIBC__) || __USE_ISOC11
  void* aligned_alloc(size_t alignment, size_t size) { return mi_aligned_alloc(alignment, size); }
  #endif
#endif

// no forwarding here due to aliasing/name mangling issues
// https://man7.org/linux/man-pages/man3/cfree.3.html
void  cfree(void* p)                                    { mi_free(p); }
// https://linux.die.net/man/3/pvalloc
void* pvalloc(size_t size)                              { return mi_pvalloc(size); }
// https://linux.die.net/man/3/memalign
void* memalign(size_t alignment, size_t size)           { return mi_memalign(alignment, size); }
// https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/aligned-malloc?view=msvc-170
void* _aligned_malloc(size_t alignment, size_t size)    { return mi_aligned_alloc(alignment, size); }
// https://man.freebsd.org/cgi/man.cgi?query=reallocarray
void* reallocarray(void* p, size_t count, size_t size)  { return mi_reallocarray(p, count, size); }
// https://man.netbsd.org/reallocarr.3
// some systems define reallocarr so mark it as a weak symbol (#751)
mi_decl_weak int reallocarr(void* p, size_t count, size_t size)    { return mi_reallocarr(p, count, size); }

#if defined(__wasi__)
  // forward __libc interface (see PR #667)
  void* __libc_malloc(size_t size)                      MI_FORWARD1(mi_malloc, size)
  void* __libc_calloc(size_t count, size_t size)        MI_FORWARD2(mi_calloc, count, size)
  void* __libc_realloc(void* p, size_t size)            MI_FORWARD2(mi_realloc, p, size)
  void  __libc_free(void* p)                            MI_FORWARD0(mi_free, p)
  void* __libc_memalign(size_t alignment, size_t size)  { return mi_memalign(alignment, size); }

#elif defined(__GLIBC__) && defined(__linux__)
  // https://sourceware.org/git/?p=glibc.git;a=blob;f=malloc/malloc.c
  // forward __libc interface (needed for glibc-based Linux distributions)
  void* __libc_malloc(size_t size)                      MI_FORWARD1(mi_malloc,size)
  void* __libc_calloc(size_t count, size_t size)        MI_FORWARD2(mi_calloc,count,size)
  void* __libc_realloc(void* p, size_t size)            MI_FORWARD2(mi_realloc,p,size)
  void  __libc_free(void* p)                            MI_FORWARD0(mi_free,p)
  void  __libc_cfree(void* p)                           MI_FORWARD0(mi_free,p)

  void* __libc_valloc(size_t size)                      { return mi_valloc(size); }
  void* __libc_pvalloc(size_t size)                     { return mi_pvalloc(size); }
  void* __libc_memalign(size_t alignment, size_t size)  { return mi_memalign(alignment,size); }
  int   __posix_memalign(void** p, size_t alignment, size_t size) { return mi_posix_memalign(p,alignment,size); }
#endif

#ifdef __cplusplus
}
#endif

#if (defined(__GNUC__) || defined(__clang__)) && !defined(__APPLE__)
#pragma GCC visibility pop
#endif

#endif // MI_MALLOC_OVERRIDE && !_WIN32

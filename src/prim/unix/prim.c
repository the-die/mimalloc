/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// This file is included in `src/prim/prim.c`

// _DEFAULT_SOURCE
//   https://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   // ensure mmap flags and syscall are defined
#endif

// _XOPEN_SOURCE, _POSIX_C_SOURCE
//   https://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html

#if defined(__sun)
// illumos provides new mman.h api when any of these are defined
// otherwise the old api based on caddr_t which predates the void pointers one.
// stock solaris provides only the former, chose to atomically to discard those
// flags only here rather than project wide tough.
#undef _XOPEN_SOURCE
#undef _POSIX_C_SOURCE
#endif

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/atomic.h"
#include "mimalloc/prim.h"

#include <sys/mman.h>  // mmap
#include <unistd.h>    // sysconf
#include <fcntl.h>     // open, close, read, access

// sys/mman.h
//   https://man7.org/linux/man-pages/man0/sys_mman.h.0p.html

#if defined(__linux__)
  #include <features.h>
  #if defined(MI_NO_THP)
  #include <sys/prctl.h>
  #endif
  #if defined(__GLIBC__)
  #include <linux/mman.h> // linux mmap flags
  #else
  #include <sys/mman.h>
  #endif
#elif defined(__APPLE__)
  #include <AvailabilityMacros.h>
  #include <TargetConditionals.h>
  #if !defined(TARGET_OS_OSX) || TARGET_OS_OSX   // see issue #879, used to be (!TARGET_IOS_IPHONE && !TARGET_IOS_SIMULATOR)
  #include <mach/vm_statistics.h>    // VM_MAKE_TAG, VM_FLAGS_SUPERPAGE_SIZE_2MB, etc.
  #endif
  #if !defined(MAC_OS_X_VERSION_10_7)
  #define MAC_OS_X_VERSION_10_7   1070
  #endif
#elif defined(__FreeBSD__) || defined(__DragonFly__)
  #include <sys/param.h>
  #if __FreeBSD_version >= 1200000
  #include <sys/cpuset.h>
  #include <sys/domainset.h>
  #endif
  #include <sys/sysctl.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__)
  #define MI_HAS_SYSCALL_H
  // sys/syscall.h
  //   https://www.gnu.org/software/libc/manual/html_node/System-Calls.html
  //   https://man7.org/linux/man-pages/man2/syscall.2.html
  #include <sys/syscall.h>
#endif


//------------------------------------------------------------------------------------
// Use syscalls for some primitives to allow for libraries that override open/read/close etc.
// and do allocation themselves; using syscalls prevents recursion when mimalloc is
// still initializing (issue #713)
// Declare inline to avoid unused function warnings.
//------------------------------------------------------------------------------------

#if defined(MI_HAS_SYSCALL_H) && defined(SYS_open) && defined(SYS_close) && defined(SYS_read) && defined(SYS_access)

// https://man7.org/linux/man-pages/man2/syscalls.2.html

// https://man7.org/linux/man-pages/man2/open.2.html
static inline int mi_prim_open(const char* fpath, int open_flags) {
  return syscall(SYS_open,fpath,open_flags,0);
}
// https://man7.org/linux/man-pages/man2/read.2.html
static inline ssize_t mi_prim_read(int fd, void* buf, size_t bufsize) {
  return syscall(SYS_read,fd,buf,bufsize);
}
// https://man7.org/linux/man-pages/man2/close.2.html
static inline int mi_prim_close(int fd) {
  return syscall(SYS_close,fd);
}
// https://man7.org/linux/man-pages/man2/access.2.html
static inline int mi_prim_access(const char *fpath, int mode) {
  return syscall(SYS_access,fpath,mode);
}

#else

static inline int mi_prim_open(const char* fpath, int open_flags) {
  return open(fpath,open_flags);
}
static inline ssize_t mi_prim_read(int fd, void* buf, size_t bufsize) {
  return read(fd,buf,bufsize);
}
static inline int mi_prim_close(int fd) {
  return close(fd);
}
static inline int mi_prim_access(const char *fpath, int mode) {
  return access(fpath,mode);
}

#endif



//---------------------------------------------
// init
//---------------------------------------------

// overcommit
//   https://www.kernel.org/doc/Documentation/vm/overcommit-accounting
//   https://www.baeldung.com/linux/overcommit-modes
//
// /proc/sys/vm/overcommit_memory
//   https://man7.org/linux/man-pages/man5/proc.5.html
static bool unix_detect_overcommit(void) {
  bool os_overcommit = true;
#if defined(__linux__)
  int fd = mi_prim_open("/proc/sys/vm/overcommit_memory", O_RDONLY);
  if (fd >= 0) {
    char buf[32];
    ssize_t nread = mi_prim_read(fd, &buf, sizeof(buf));
    mi_prim_close(fd);
    // <https://www.kernel.org/doc/Documentation/vm/overcommit-accounting>
    // 0: heuristic overcommit, 1: always overcommit, 2: never overcommit (ignore NORESERVE)
    if (nread >= 1) {
      os_overcommit = (buf[0] == '0' || buf[0] == '1');
    }
  }
#elif defined(__FreeBSD__)
  int val = 0;
  size_t olen = sizeof(val);
  if (sysctlbyname("vm.overcommit", &val, &olen, NULL, 0) == 0) {
    os_overcommit = (val != 0);
  }
#else
  // default: overcommit is true
#endif
  return os_overcommit;
}

// sysconf
//   https://man7.org/linux/man-pages/man3/sysconf.3.html
void _mi_prim_mem_init( mi_os_mem_config_t* config )
{
  long psize = sysconf(_SC_PAGESIZE);
  if (psize > 0) {
    config->page_size = (size_t)psize;
    config->alloc_granularity = (size_t)psize;
  }
  config->large_page_size = 2*MI_MiB; // TODO: can we query the OS for this?
  config->has_overcommit = unix_detect_overcommit();
  // https://man7.org/linux/man-pages/man3/munmap.3p.html
  // The munmap() function shall remove any mappings for those entire
  // pages containing any part of the address space of the process
  // starting at addr and continuing for len bytes.
  config->has_partial_free = true;    // mmap can free in parts
  config->has_virtual_reserve = true; // todo: check if this true for NetBSD?  (for anonymous mmap with PROT_NONE)

  // Transparent Hugepage
  //   https://docs.kernel.org/admin-guide/mm/transhuge.html
  //   https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/6/html/performance_tuning_guide/s-memory-transhuge

  // disable transparent huge pages for this process?
  #if (defined(__linux__) || defined(__ANDROID__)) && defined(PR_GET_THP_DISABLE)
  #if defined(MI_NO_THP)
  if (true)
  #else
  if (!mi_option_is_enabled(mi_option_allow_large_os_pages)) // disable THP also if large OS pages are not allowed in the options
  #endif
  {
    int val = 0;
    // prctl
    //   https://man7.org/linux/man-pages/man2/prctl.2.html
    // PR_GET_THP_DISABLE
    //   https://man7.org/linux/man-pages/man2/PR_GET_THP_DISABLE.2const.html
    if (prctl(PR_GET_THP_DISABLE, &val, 0, 0, 0) != 0) {
      // Most likely since distros often come with always/madvise settings.
      val = 1;
      // Disabling only for mimalloc process rather than touching system wide settings
      (void)prctl(PR_SET_THP_DISABLE, &val, 0, 0, 0);
    }
  }
  #endif
}


//---------------------------------------------
// free
//---------------------------------------------

// munmap
//   https://man7.org/linux/man-pages/man2/mmap.2.html
//   https://man7.org/linux/man-pages/man3/munmap.3p.html
int _mi_prim_free(void* addr, size_t size ) {
  bool err = (munmap(addr, size) == -1);
  return (err ? errno : 0);
}


//---------------------------------------------
// mmap
//---------------------------------------------

// madvise
//   https://man7.org/linux/man-pages/man2/madvise.2.html
//
// The madvise() system call is used to give advice or directions to
// the kernel about the address range beginning at address addr and
// with size length.  madvise() only operates on whole pages,
// therefore addr must be page-aligned.  The value of length is
// rounded up to a multiple of page size.  In most cases, the goal
// of such advice is to improve system or application performance.
//
// Initially, the system call supported a set of "conventional"
// advice values, which are also available on several other
// implementations.  (Note, though, that madvise() is not specified
// in POSIX.)  Subsequently, a number of Linux-specific advice
// values have been added.
static int unix_madvise(void* addr, size_t size, int advice) {
  #if defined(__sun)
  return madvise((caddr_t)addr, size, advice);  // Solaris needs cast (issue #520)
  #else
  return madvise(addr, size, advice);
  #endif
}

// mmap
//   https://man7.org/linux/man-pages/man2/mmap.2.html
//   https://man7.org/linux/man-pages/man3/mmap.3p.html
static void* unix_mmap_prim(void* addr, size_t size, size_t try_alignment, int protect_flags, int flags, int fd) {
  MI_UNUSED(try_alignment);
  void* p = NULL;
  // MAP_ALIGNED
  //   https://man.freebsd.org/cgi/man.cgi?mmap(2)
  #if defined(MAP_ALIGNED)  // BSD
  if (addr == NULL && try_alignment > 1 && (try_alignment % _mi_os_page_size()) == 0) {
    size_t n = mi_bsr(try_alignment);
    if (((size_t)1 << n) == try_alignment && n >= 12 && n <= 30) {  // alignment is a power of 2 and 4096 <= alignment <= 1GiB
      p = mmap(addr, size, protect_flags, flags | MAP_ALIGNED(n), fd, 0);
      if (p==MAP_FAILED || !_mi_is_aligned(p,try_alignment)) {
        int err = errno;
        _mi_trace_message("unable to directly request aligned OS memory (error: %d (0x%x), size: 0x%zx bytes, alignment: 0x%zx, hint address: %p)\n", err, err, size, try_alignment, addr);
      }
      if (p!=MAP_FAILED) return p;
      // fall back to regular mmap
    }
  }
  // MAP_ALIGN
  //   https://docs.oracle.com/cd/E88353_01/html/E37841/mmap-2.html
  #elif defined(MAP_ALIGN)  // Solaris
  if (addr == NULL && try_alignment > 1 && (try_alignment % _mi_os_page_size()) == 0) {
    p = mmap((void*)try_alignment, size, protect_flags, flags | MAP_ALIGN, fd, 0);  // addr parameter is the required alignment
    if (p!=MAP_FAILED) return p;
    // fall back to regular mmap
  }
  #endif
  #if (MI_INTPTR_SIZE >= 8) && !defined(MAP_ALIGNED)
  // on 64-bit systems, use the virtual address area after 2TiB for 4MiB aligned allocations
  if (addr == NULL) {
    void* hint = _mi_os_get_aligned_hint(try_alignment, size);
    if (hint != NULL) {
      // If addr is NULL, then the kernel chooses the (page-aligned)
      // address at which to create the mapping; this is the most portable
      // method of creating a new mapping.  If addr is not NULL, then the
      // kernel takes it as a hint about where to place the mapping; on
      // Linux, the kernel will pick a nearby page boundary (but always
      // above or equal to the value specified by
      // /proc/sys/vm/mmap_min_addr) and attempt to create the mapping
      // there.  If another mapping already exists there, the kernel picks
      // a new address that may or may not depend on the hint.  The
      // address of the new mapping is returned as the result of the call.
      p = mmap(hint, size, protect_flags, flags, fd, 0);
      if (p==MAP_FAILED || !_mi_is_aligned(p,try_alignment)) {
        #if MI_TRACK_ENABLED  // asan sometimes does not instrument errno correctly?
        int err = 0;
        #else
        int err = errno;
        #endif
        _mi_trace_message("unable to directly request hinted aligned OS memory (error: %d (0x%x), size: 0x%zx bytes, alignment: 0x%zx, hint address: %p)\n", err, err, size, try_alignment, hint);
      }
      if (p!=MAP_FAILED) return p;
      // fall back to regular mmap
    }
  }
  #endif
  // regular mmap
  //
  // The contents of a file mapping (as opposed to an anonymous
  // mapping; see MAP_ANONYMOUS below), are initialized using length
  // bytes starting at offset offset in the file (or other object)
  // referred to by the file descriptor fd.  offset must be a multiple
  // of the page size as returned by sysconf(_SC_PAGE_SIZE).
  //
  // After the mmap() call has returned, the file descriptor, fd, can
  // be closed immediately without invalidating the mapping.
  p = mmap(addr, size, protect_flags, flags, fd, 0);
  // On success, mmap() returns a pointer to the mapped area.  On
  // error, the value MAP_FAILED (that is, (void *) -1) is returned,
  // and errno is set to indicate the error.
  if (p!=MAP_FAILED) return p;
  // failed to allocate
  return NULL;
}

static int unix_mmap_fd(void) {
  // VM_MAKE_TAG
  //   https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html
  #if defined(VM_MAKE_TAG)
  // macOS: tracking anonymous page with a specific ID. (All up to 98 are taken officially but LLVM sanitizers had taken 99)
  int os_tag = (int)mi_option_get(mi_option_os_tag);
  if (os_tag < 100 || os_tag > 255) { os_tag = 100; }
  return VM_MAKE_TAG(os_tag);
  #else
  // MAP_ANONYMOUS
  //
  // The mapping is not backed by any file; its contents are
  // initialized to zero.  The fd argument is ignored; however,
  // some implementations require fd to be -1 if MAP_ANONYMOUS
  // (or MAP_ANON) is specified, and portable applications
  // should ensure this.  The offset argument should be zero.
  // Support for MAP_ANONYMOUS in conjunction with MAP_SHARED
  // was added in Linux 2.4.
  return -1;
  #endif
}

// https://www.gnu.org/software/libc/manual/html_node/Memory_002dmapped-I_002fO.html
static void* unix_mmap(void* addr, size_t size, size_t try_alignment, int protect_flags, bool large_only, bool allow_large, bool* is_large) {
  // MAP_ANONYMOUS, MAP_ANON
  //   https://man7.org/linux/man-pages/man2/mmap.2.html
  #if !defined(MAP_ANONYMOUS)
  #define MAP_ANONYMOUS  MAP_ANON
  #endif
  // MAP_NORESERVE
  //   https://man7.org/linux/man-pages/man2/mmap.2.html
  #if !defined(MAP_NORESERVE)
  #define MAP_NORESERVE  0
  #endif
  void* p = NULL;
  const int fd = unix_mmap_fd();
  // MAP_ANONYMOUS, MAP_PRIVATE
  //   https://man7.org/linux/man-pages/man2/mmap.2.html
  //
  // MAP_PRIVATE
  //
  // Create a private copy-on-write mapping.  Updates to the
  // mapping are not visible to other processes mapping the
  // same file, and are not carried through to the underlying
  // file.  It is unspecified whether changes made to the file
  // after the mmap() call are visible in the mapped region.
  //
  // MAP_ANONYMOUS
  //
  // The mapping is not backed by any file; its contents are
  // initialized to zero.  The fd argument is ignored; however,
  // some implementations require fd to be -1 if MAP_ANONYMOUS
  // (or MAP_ANON) is specified, and portable applications
  // should ensure this.  The offset argument should be zero.
  // Support for MAP_ANONYMOUS in conjunction with MAP_SHARED
  // was added in Linux 2.4.
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (_mi_os_has_overcommit()) {
    // MAP_NORESERVE
    //   https://man7.org/linux/man-pages/man2/mmap.2.html
    //
    //   Do not reserve swap space for this mapping.  When swap
    //   space is reserved, one has the guarantee that it is
    //   possible to modify the mapping.  When swap space is not
    //   reserved one might get SIGSEGV upon a write if no physical
    //   memory is available.  See also the discussion of the file
    //   /proc/sys/vm/overcommit_memory in proc(5).  Before Linux
    //   2.6, this flag had effect only for private writable
    //   mappings.
    flags |= MAP_NORESERVE;
  }
  // PROT_MAZ
  //   https://man.freebsd.org/cgi/man.cgi?mmap(2)
  #if defined(PROT_MAX)
  protect_flags |= PROT_MAX(PROT_READ | PROT_WRITE); // BSD
  #endif
  // huge page allocation
  if ((large_only || _mi_os_use_large_page(size, try_alignment)) && allow_large) {
    static _Atomic(size_t) large_page_try_ok; // = 0;
    size_t try_ok = mi_atomic_load_acquire(&large_page_try_ok);
    if (!large_only && try_ok > 0) {
      // If the OS is not configured for large OS pages, or the user does not have
      // enough permission, the `mmap` will always fail (but it might also fail for other reasons).
      // Therefore, once a large page allocation failed, we don't try again for `large_page_try_ok` times
      // to avoid too many failing calls to mmap.
      mi_atomic_cas_strong_acq_rel(&large_page_try_ok, &try_ok, try_ok - 1);
    }
    else {
      int lflags = flags & ~MAP_NORESERVE;  // using NORESERVE on huge pages seems to fail on Linux
      int lfd = fd;
      // MAP_ALIGNED_SUPER
      //   https://man.freebsd.org/cgi/man.cgi?mmap(2)
      #ifdef MAP_ALIGNED_SUPER
      lflags |= MAP_ALIGNED_SUPER;
      #endif
      // MAP_HUGETLB
      //   https://man7.org/linux/man-pages/man2/mmap.2.html
      //
      // MAP_HUGETLB (since Linux 2.6.32)
      //
      // Allocate the mapping using "huge" pages.  See the Linux
      // kernel source file
      // Documentation/admin-guide/mm/hugetlbpage.rst for further
      // information, as well as NOTES, below.
      #ifdef MAP_HUGETLB
      lflags |= MAP_HUGETLB;
      #endif
      // MAP_HUGE_1GB
      //   https://man7.org/linux/man-pages/man2/mmap.2.html
      //
      // MAP_HUGE_2MB
      // MAP_HUGE_1GB (since Linux 3.8)
      //
      // Used in conjunction with MAP_HUGETLB to select alternative
      // hugetlb page sizes (respectively, 2 MB and 1 GB) on
      // systems that support multiple hugetlb page sizes.
      //
      // More generally, the desired huge page size can be
      // configured by encoding the base-2 logarithm of the desired
      // page size in the six bits at the offset MAP_HUGE_SHIFT.
      // (A value of zero in this bit field provides the default
      // huge page size; the default huge page size can be
      // discovered via the Hugepagesize field exposed by
      // /proc/meminfo.)  Thus, the above two constants are defined
      // as:
      //
      //     #define MAP_HUGE_2MB    (21 << MAP_HUGE_SHIFT)
      //     #define MAP_HUGE_1GB    (30 << MAP_HUGE_SHIFT)
      //
      // The range of huge page sizes that are supported by the
      // system can be discovered by listing the subdirectories in
      // /sys/kernel/mm/hugepages.
      #ifdef MAP_HUGE_1GB
      static bool mi_huge_pages_available = true;
      if ((size % MI_GiB) == 0 && mi_huge_pages_available) {
        lflags |= MAP_HUGE_1GB;
      }
      else
      #endif
      {
        // MAP_HUGE_2MB
        //   https://man7.org/linux/man-pages/man2/mmap.2.html
        #ifdef MAP_HUGE_2MB
        lflags |= MAP_HUGE_2MB;
        #endif
      }
      // VM_FLAGS_SUPERPAGE_SIZE_2MB
      //   https://www.unix.com/man-page/osx/2/mmap
      #ifdef VM_FLAGS_SUPERPAGE_SIZE_2MB
      lfd |= VM_FLAGS_SUPERPAGE_SIZE_2MB;
      #endif
      if (large_only || lflags != flags) {
        // try large OS page allocation
        *is_large = true;
        p = unix_mmap_prim(addr, size, try_alignment, protect_flags, lflags, lfd);
        // MAP_HUGE_1GB
        //   https://man7.org/linux/man-pages/man2/mmap.2.html
        #ifdef MAP_HUGE_1GB
        if (p == NULL && (lflags & MAP_HUGE_1GB) == MAP_HUGE_1GB) {
          mi_huge_pages_available = false; // don't try huge 1GiB pages again
          _mi_warning_message("unable to allocate huge (1GiB) page, trying large (2MiB) pages instead (errno: %i)\n", errno);
          lflags = ((lflags & ~MAP_HUGE_1GB) | MAP_HUGE_2MB);
          p = unix_mmap_prim(addr, size, try_alignment, protect_flags, lflags, lfd);
        }
        #endif
        if (large_only) return p;
        if (p == NULL) {
          mi_atomic_store_release(&large_page_try_ok, (size_t)8);  // on error, don't try again for the next N allocations
        }
      }
    }
  }
  // regular allocation
  if (p == NULL) {
    *is_large = false;
    p = unix_mmap_prim(addr, size, try_alignment, protect_flags, flags, fd);
    if (p != NULL) {
      // MADV_HUGEPAGE
      //   https://man7.org/linux/man-pages/man2/madvise.2.html
      //
      // MADV_HUGEPAGE (since Linux 2.6.38)
      //
      // Enable Transparent Huge Pages (THP) for pages in the range
      // specified by addr and length.  The kernel will regularly
      // scan the areas marked as huge page candidates to replace
      // them with huge pages.  The kernel will also allocate huge
      // pages directly when the region is naturally aligned to the
      // huge page size (see posix_memalign(2)).
      #if defined(MADV_HUGEPAGE)
      // Many Linux systems don't allow MAP_HUGETLB but they support instead
      // transparent huge pages (THP). Generally, it is not required to call `madvise` with MADV_HUGE
      // though since properly aligned allocations will already use large pages if available
      // in that case -- in particular for our large regions (in `memory.c`).
      // However, some systems only allow THP if called with explicit `madvise`, so
      // when large OS pages are enabled for mimalloc, we call `madvise` anyways.
      if (allow_large && _mi_os_use_large_page(size, try_alignment)) {
        if (unix_madvise(p, size, MADV_HUGEPAGE) == 0) {
          *is_large = true; // possibly
        };
      }
      #elif defined(__sun)
      if (allow_large && _mi_os_use_large_page(size, try_alignment)) {
        struct memcntl_mha cmd = {0};
        cmd.mha_pagesize = _mi_os_large_page_size();
        cmd.mha_cmd = MHA_MAPSIZE_VA;
        if (memcntl((caddr_t)p, size, MC_HAT_ADVISE, (caddr_t)&cmd, 0, 0) == 0) {
          *is_large = true;
        }
      }
      #endif
    }
  }
  return p;
}

// mimalloc primitive allocate
// Note: the `try_alignment` is just a hint and the returned pointer is not guaranteed to be aligned.
int _mi_prim_alloc(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, void** addr) {
  mi_assert_internal(size > 0 && (size % _mi_os_page_size()) == 0);
  mi_assert_internal(commit || !allow_large);  // `commit` is false, `allow_large` must be false
  mi_assert_internal(try_alignment > 0);

  // MAP_ANONYMOUS
  // The mapping is not backed by any file; its contents are
  // initialized to zero.
  *is_zero = true;
  int protect_flags = (commit ? (PROT_WRITE | PROT_READ) : PROT_NONE);
  *addr = unix_mmap(NULL, size, try_alignment, protect_flags, false, allow_large, is_large);
  return (*addr != NULL ? 0 : errno);
}


//---------------------------------------------
// Commit/Reset
//---------------------------------------------

static void unix_mprotect_hint(int err) {
  #if defined(__linux__) && (MI_SECURE>=2) // guard page around every mimalloc page
  // ENOMEM
  //
  // Changing the protection of a memory region would result in
  // the total number of mappings with distinct attributes
  // (e.g., read versus read/write protection) exceeding the
  // allowed maximum.  (For example, making the protection of a
  // range PROT_READ in the middle of a region currently
  // protected as PROT_READ|PROT_WRITE would result in three
  // mappings: two read/write mappings at each end and a read-
  // only mapping in the middle.)
  if (err == ENOMEM) {
    _mi_warning_message("The next warning may be caused by a low memory map limit.\n"
                        "  On Linux this is controlled by the vm.max_map_count -- maybe increase it?\n"
                        "  For example: sudo sysctl -w vm.max_map_count=262144\n");
  }
  #else
  MI_UNUSED(err);
  #endif
}

int _mi_prim_commit(void* start, size_t size, bool* is_zero) {
  // commit: ensure we can access the area
  // note: we may think that *is_zero can be true since the memory
  // was either from mmap PROT_NONE, or from decommit MADV_DONTNEED, but
  // we sometimes call commit on a range with still partially committed
  // memory and `mprotect` does not zero the range.
  *is_zero = false;
  // mprotect
  //   https://man7.org/linux/man-pages/man2/mprotect.2.html
  int err = mprotect(start, size, (PROT_READ | PROT_WRITE));
  if (err != 0) {
    err = errno;
    unix_mprotect_hint(err);
  }
  return err;
}

int _mi_prim_decommit(void* start, size_t size, bool* needs_recommit) {
  int err = 0;
  // MADV_DONTNEED
  //   https://man7.org/linux/man-pages/man2/madvise.2.html
  //
  // Do not expect access in the near future.  (For the time
  // being, the application is finished with the given range,
  // so the kernel can free resources associated with it.)
  //
  // After a successful MADV_DONTNEED operation, the semantics
  // of memory access in the specified region are changed:
  // subsequent accesses of pages in the range will succeed,
  // but will result in either repopulating the memory contents
  // from the up-to-date contents of the underlying mapped file
  // (for shared file mappings, shared anonymous mappings, and
  // shmem-based techniques such as System V shared memory
  // segments) or zero-fill-on-demand pages for anonymous
  // private mappings.
  //
  // Note that, when applied to shared mappings, MADV_DONTNEED
  // might not lead to immediate freeing of the pages in the
  // range.  The kernel is free to delay freeing the pages
  // until an appropriate moment.  The resident set size (RSS)
  // of the calling process will be immediately reduced
  // however.
  //
  // MADV_FREE (since Linux 4.5)
  //
  // The application no longer requires the pages in the range
  // specified by addr and len.  The kernel can thus free these
  // pages, but the freeing could be delayed until memory
  // pressure occurs.  For each of the pages that has been
  // marked to be freed but has not yet been freed, the free
  // operation will be canceled if the caller writes into the
  // page.  After a successful MADV_FREE operation, any stale
  // data (i.e., dirty, unwritten pages) will be lost when the
  // kernel frees the pages.  However, subsequent writes to
  // pages in the range will succeed and then kernel cannot
  // free those dirtied pages, so that the caller can always
  // see just written data.  If there is no subsequent write,
  // the kernel can free the pages at any time.  Once pages in
  // the range have been freed, the caller will see zero-fill-
  // on-demand pages upon subsequent page references.
  //
  // The MADV_FREE operation can be applied only to private
  // anonymous pages (see mmap(2)).  Before Linux 4.12, when
  // freeing pages on a swapless system, the pages in the given
  // range are freed instantly, regardless of memory pressure.
  //
  // decommit: use MADV_DONTNEED as it decreases rss immediately (unlike MADV_FREE)
  err = unix_madvise(start, size, MADV_DONTNEED);
  #if !MI_DEBUG && !MI_SECURE
    *needs_recommit = false;
  #else
    *needs_recommit = true;
    // PROT_NONE
    //   https://man7.org/linux/man-pages/man2/mprotect.2.html
    mprotect(start, size, PROT_NONE);
  #endif
  /*
  // decommit: use mmap with MAP_FIXED and PROT_NONE to discard the existing memory (and reduce rss)
  *needs_recommit = true;
  const int fd = unix_mmap_fd();
  void* p = mmap(start, size, PROT_NONE, (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), fd, 0);
  if (p != start) { err = errno; }
  */
  return err;
}

int _mi_prim_reset(void* start, size_t size) {
  // MADV_FREE
  //   https://man7.org/linux/man-pages/man2/madvise.2.html
  //
  // We try to use `MADV_FREE` as that is the fastest. A drawback though is that it 
  // will not reduce the `rss` stats in tools like `top` even though the memory is available
  // to other processes. With the default `MIMALLOC_PURGE_DECOMMITS=1` we ensure that by
  // default `MADV_DONTNEED` is used though.
  #if defined(MADV_FREE)
  static _Atomic(size_t) advice = MI_ATOMIC_VAR_INIT(MADV_FREE);
  int oadvice = (int)mi_atomic_load_relaxed(&advice);
  int err;
  // EAGAIN
  // A kernel resource was temporarily unavailable.
  while ((err = unix_madvise(start, size, oadvice)) != 0 && errno == EAGAIN) { errno = 0; };
  if (err != 0 && errno == EINVAL && oadvice == MADV_FREE) {
    // if MADV_FREE is not supported, fall back to MADV_DONTNEED from now on
    mi_atomic_store_release(&advice, (size_t)MADV_DONTNEED);
    err = unix_madvise(start, size, MADV_DONTNEED);
  }
  #else
  int err = unix_madvise(start, size, MADV_DONTNEED);
  #endif
  return err;
}

// https://man7.org/linux/man-pages/man2/mprotect.2.html
int _mi_prim_protect(void* start, size_t size, bool protect) {
  int err = mprotect(start, size, protect ? PROT_NONE : (PROT_READ | PROT_WRITE));
  if (err != 0) { err = errno; }
  unix_mprotect_hint(err);
  return err;
}



//---------------------------------------------
// Huge page allocation
//---------------------------------------------

#if (MI_INTPTR_SIZE >= 8) && !defined(__HAIKU__) && !defined(__CYGWIN__)

#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif

// mbind
//   https://man7.org/linux/man-pages/man2/mbind.2.html
//
// mbind() sets the NUMA memory policy, which consists of a policy
// mode and zero or more nodes, for the memory range starting with
// addr and continuing for len bytes.  The memory policy defines
// from which node memory is allocated.
#if defined(MI_HAS_SYSCALL_H) && defined(SYS_mbind)
static long mi_prim_mbind(void* start, unsigned long len, unsigned long mode, const unsigned long* nmask, unsigned long maxnode, unsigned flags) {
  return syscall(SYS_mbind, start, len, mode, nmask, maxnode, flags);
}
#else
static long mi_prim_mbind(void* start, unsigned long len, unsigned long mode, const unsigned long* nmask, unsigned long maxnode, unsigned flags) {
  MI_UNUSED(start); MI_UNUSED(len); MI_UNUSED(mode); MI_UNUSED(nmask); MI_UNUSED(maxnode); MI_UNUSED(flags);
  return 0;
}
#endif

int _mi_prim_alloc_huge_os_pages(void* hint_addr, size_t size, int numa_node, bool* is_zero, void** addr) {
  bool is_large = true;
  *is_zero = true;
  *addr = unix_mmap(hint_addr, size, MI_SEGMENT_SIZE, PROT_READ | PROT_WRITE, true, true, &is_large);
  if (*addr != NULL && numa_node >= 0 && numa_node < 8*MI_INTPTR_SIZE) { // at most 64 nodes
    unsigned long numa_mask = (1UL << numa_node);
    // MPOL_PREFERRED
    //   https://man7.org/linux/man-pages/man2/mbind.2.html
    //
    // This mode sets the preferred node for allocation.  The
    // kernel will try to allocate pages from this node first and
    // fall back to other nodes if the preferred nodes is low on
    // free memory.  If nodemask specifies more than one node ID,
    // the first node in the mask will be selected as the
    // preferred node.  If the nodemask and maxnode arguments
    // specify the empty set, then the memory is allocated on the
    // node of the CPU that triggered the allocation.
    //
    // TODO: does `mbind` work correctly for huge OS pages? should we
    // use `set_mempolicy` before calling mmap instead?
    // see: <https://lkml.org/lkml/2017/2/9/875>
    long err = mi_prim_mbind(*addr, size, MPOL_PREFERRED, &numa_mask, 8*MI_INTPTR_SIZE, 0);
    if (err != 0) {
      err = errno;
      _mi_warning_message("failed to bind huge (1GiB) pages to numa node %d (error: %d (0x%x))\n", numa_node, err, err);
    }
  }
  return (*addr != NULL ? 0 : errno);
}

#else

int _mi_prim_alloc_huge_os_pages(void* hint_addr, size_t size, int numa_node, bool* is_zero, void** addr) {
  MI_UNUSED(hint_addr); MI_UNUSED(size); MI_UNUSED(numa_node);
  *is_zero = false;
  *addr = NULL;
  return ENOMEM;
}

#endif

//---------------------------------------------
// NUMA nodes
//---------------------------------------------

#if defined(__linux__)

// NUMA
//   https://man7.org/linux/man-pages/man7/numa.7.html
//   https://man7.org/linux/man-pages/man3/numa.3.html

// getcpu
//   https://man7.org/linux/man-pages/man2/getcpu.2.html
//
// The getcpu() system call identifies the processor and node on
// which the calling thread or process is currently running and
// writes them into the integers pointed to by the cpu and node
// arguments.  The processor is a unique small integer identifying a
// CPU.  The node is a unique small identifier identifying a NUMA
// node.  When either cpu or node is NULL nothing is written to the
// respective pointer.
size_t _mi_prim_numa_node(void) {
  #if defined(MI_HAS_SYSCALL_H) && defined(SYS_getcpu)
    unsigned long node = 0;
    unsigned long ncpu = 0;
    long err = syscall(SYS_getcpu, &ncpu, &node, NULL);
    if (err != 0) return 0;
    return node;
  #else
    return 0;
  #endif
}

// /sys/devices/system/node/node
//   https://www.kernel.org/doc/Documentation/ABI/stable/sysfs-devices-node
size_t _mi_prim_numa_node_count(void) {
  char buf[128];
  unsigned node = 0;
  for(node = 0; node < 256; node++) {
    // enumerate node entries -- todo: it there a more efficient way to do this? (but ensure there is no allocation)
    _mi_snprintf(buf, 127, "/sys/devices/system/node/node%u", node + 1);
    if (mi_prim_access(buf,R_OK) != 0) break;
  }
  return (node+1);
}

#elif defined(__FreeBSD__) && __FreeBSD_version >= 1200000

size_t _mi_prim_numa_node(void) {
  domainset_t dom;
  size_t node;
  int policy;
  if (cpuset_getdomain(CPU_LEVEL_CPUSET, CPU_WHICH_PID, -1, sizeof(dom), &dom, &policy) == -1) return 0ul;
  for (node = 0; node < MAXMEMDOM; node++) {
    if (DOMAINSET_ISSET(node, &dom)) return node;
  }
  return 0ul;
}

size_t _mi_prim_numa_node_count(void) {
  size_t ndomains = 0;
  size_t len = sizeof(ndomains);
  if (sysctlbyname("vm.ndomains", &ndomains, &len, NULL, 0) == -1) return 0ul;
  return ndomains;
}

#elif defined(__DragonFly__)

size_t _mi_prim_numa_node(void) {
  // TODO: DragonFly does not seem to provide any userland means to get this information.
  return 0ul;
}

size_t _mi_prim_numa_node_count(void) {
  size_t ncpus = 0, nvirtcoresperphys = 0;
  size_t len = sizeof(size_t);
  if (sysctlbyname("hw.ncpu", &ncpus, &len, NULL, 0) == -1) return 0ul;
  if (sysctlbyname("hw.cpu_topology_ht_ids", &nvirtcoresperphys, &len, NULL, 0) == -1) return 0ul;
  return nvirtcoresperphys * ncpus;
}

#else

size_t _mi_prim_numa_node(void) {
  return 0;
}

size_t _mi_prim_numa_node_count(void) {
  return 1;
}

#endif

// ----------------------------------------------------------------
// Clock
// ----------------------------------------------------------------

#include <time.h>

// CLOCK_REALTIME, CLOCK_MONOTONIC, clock_gettime
//   https://man7.org/linux/man-pages/man2/clock_gettime.2.html

#if defined(CLOCK_REALTIME) || defined(CLOCK_MONOTONIC)

mi_msecs_t _mi_prim_clock_now(void) {
  struct timespec t;
  #ifdef CLOCK_MONOTONIC
  // CLOCK_MONOTONIC
  // A nonsettable system-wide clock that represents monotonic time
  // since—as described by POSIX—"some unspecified point in the
  // past".  On Linux, that point corresponds to the number of sec‐
  // onds that the system has been running since it was booted.
  clock_gettime(CLOCK_MONOTONIC, &t);
  #else
  // CLOCK_REALTIME
  // A settable system-wide clock that measures real (i.e., wall-
  // clock) time.  Setting this clock requires appropriate privi‐
  // leges.  This clock is affected by discontinuous jumps in the
  // system time (e.g., if the system administrator manually
  // changes the clock), and by the incremental adjustments per‐
  // formed by adjtime(3) and NTP.
  clock_gettime(CLOCK_REALTIME, &t);
  #endif
  return ((mi_msecs_t)t.tv_sec * 1000) + ((mi_msecs_t)t.tv_nsec / 1000000);
}

#else

// CLOCKS_PER_SEC, clock
//   https://man7.org/linux/man-pages/man3/clock.3.html

// low resolution timer
mi_msecs_t _mi_prim_clock_now(void) {
  #if !defined(CLOCKS_PER_SEC) || (CLOCKS_PER_SEC == 1000) || (CLOCKS_PER_SEC == 0)
  return (mi_msecs_t)clock();
  #elif (CLOCKS_PER_SEC < 1000)
  return (mi_msecs_t)clock() * (1000 / (mi_msecs_t)CLOCKS_PER_SEC);
  #else
  return (mi_msecs_t)clock() / ((mi_msecs_t)CLOCKS_PER_SEC / 1000);
  #endif
}

#endif




//----------------------------------------------------------------
// Process info
//----------------------------------------------------------------

#if defined(__unix__) || defined(__unix) || defined(unix) || defined(__APPLE__) || defined(__HAIKU__)
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#if defined(__HAIKU__)
#include <kernel/OS.h>
#endif

// timeval
//   https://man7.org/linux/man-pages/man3/timeval.3type.html
static mi_msecs_t timeval_secs(const struct timeval* tv) {
  return ((mi_msecs_t)tv->tv_sec * 1000L) + ((mi_msecs_t)tv->tv_usec / 1000L);
}

// getrusage
//   https://man7.org/linux/man-pages/man2/getrusage.2.html

void _mi_prim_process_info(mi_process_info_t* pinfo)
{
  // struct rusage {
  //     struct timeval ru_utime; /* user CPU time used */
  //     struct timeval ru_stime; /* system CPU time used */
  //     long   ru_maxrss;        /* maximum resident set size */
  //     long   ru_ixrss;         /* integral shared memory size */
  //     long   ru_idrss;         /* integral unshared data size */
  //     long   ru_isrss;         /* integral unshared stack size */
  //     long   ru_minflt;        /* page reclaims (soft page faults) */
  //     long   ru_majflt;        /* page faults (hard page faults) */
  //     long   ru_nswap;         /* swaps */
  //     long   ru_inblock;       /* block input operations */
  //     long   ru_oublock;       /* block output operations */
  //     long   ru_msgsnd;        /* IPC messages sent */
  //     long   ru_msgrcv;        /* IPC messages received */
  //     long   ru_nsignals;      /* signals received */
  //     long   ru_nvcsw;         /* voluntary context switches */
  //     long   ru_nivcsw;        /* involuntary context switches */
  // };
  struct rusage rusage;
  // RUSAGE_SELF
  // Return resource usage statistics for the calling process,
  // which is the sum of resources used by all threads in the
  // process.
  getrusage(RUSAGE_SELF, &rusage);
  pinfo->utime = timeval_secs(&rusage.ru_utime);
  pinfo->stime = timeval_secs(&rusage.ru_stime);
#if !defined(__HAIKU__)
  pinfo->page_faults = rusage.ru_majflt;
#endif
#if defined(__HAIKU__)
  // Haiku does not have (yet?) a way to
  // get these stats per process
  thread_info tid;
  area_info mem;
  ssize_t c;
  get_thread_info(find_thread(0), &tid);
  while (get_next_area_info(tid.team, &c, &mem) == B_OK) {
    pinfo->peak_rss += mem.ram_size;
  }
  pinfo->page_faults = 0;
#elif defined(__APPLE__)
  pinfo->peak_rss = rusage.ru_maxrss;         // macos reports in bytes
  #ifdef MACH_TASK_BASIC_INFO
  struct mach_task_basic_info info;
  mach_msg_type_number_t infoCount = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &infoCount) == KERN_SUCCESS) {
    pinfo->current_rss = (size_t)info.resident_size;
  }
  #else
  struct task_basic_info info;
  mach_msg_type_number_t infoCount = TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&info, &infoCount) == KERN_SUCCESS) {
    pinfo->current_rss = (size_t)info.resident_size;
  }
  #endif
#else
  // ru_maxrss (since Linux 2.6.32)
  // This is the maximum resident set size used (in kilobytes).
  // For RUSAGE_CHILDREN, this is the resident set size of the
  // largest child, not the maximum resident set size of the
  // process tree.
  pinfo->peak_rss = rusage.ru_maxrss * 1024;  // Linux/BSD report in KiB
#endif
  // use defaults for commit
}

#else

#ifndef __wasi__
// WebAssembly instances are not processes
#pragma message("define a way to get process info")
#endif

void _mi_prim_process_info(mi_process_info_t* pinfo)
{
  // use defaults
  MI_UNUSED(pinfo);
}

#endif


//----------------------------------------------------------------
// Output
//----------------------------------------------------------------

// fputs
//   https://man7.org/linux/man-pages/man3/fputs.3p.html
void _mi_prim_out_stderr( const char* msg ) {
  fputs(msg,stderr);
}


//----------------------------------------------------------------
// Environment
//----------------------------------------------------------------

// __has_include
//   https://gcc.gnu.org/onlinedocs/cpp/index.html

#if !defined(MI_USE_ENVIRON) || (MI_USE_ENVIRON!=0)
// On Posix systemsr use `environ` to access environment variables
// even before the C runtime is initialized.
#if defined(__APPLE__) && defined(__has_include) && __has_include(<crt_externs.h>)
#include <crt_externs.h>
static char** mi_get_environ(void) {
  return (*_NSGetEnviron());
}
#else
// environ
//   https://man7.org/linux/man-pages/man7/environ.7.html
extern char** environ;
static char** mi_get_environ(void) {
  return environ;
}
#endif
bool _mi_prim_getenv(const char* name, char* result, size_t result_size) {
  if (name==NULL) return false;
  const size_t len = _mi_strlen(name);
  if (len == 0) return false;
  char** env = mi_get_environ();
  if (env == NULL) return false;
  // compare up to 10000 entries
  for (int i = 0; i < 10000 && env[i] != NULL; i++) {
    const char* s = env[i];
    if (_mi_strnicmp(name, s, len) == 0 && s[len] == '=') { // case insensitive
      // found it
      _mi_strlcpy(result, s + len + 1, result_size);
      return true;
    }
  }
  return false;
}
#else
// getenv
//   https://man7.org/linux/man-pages/man3/getenv.3.html
// fallback: use standard C `getenv` but this cannot be used while initializing the C runtime
bool _mi_prim_getenv(const char* name, char* result, size_t result_size) {
  // cannot call getenv() when still initializing the C runtime.
  if (_mi_preloading()) return false;
  const char* s = getenv(name);
  if (s == NULL) {
    // we check the upper case name too.
    char buf[64+1];
    size_t len = _mi_strnlen(name,sizeof(buf)-1);
    for (size_t i = 0; i < len; i++) {
      buf[i] = _mi_toupper(name[i]);
    }
    buf[len] = 0;
    s = getenv(buf);
  }
  if (s == NULL || _mi_strnlen(s,result_size) >= result_size)  return false;
  _mi_strlcpy(result, s, result_size);
  return true;
}
#endif  // !MI_USE_ENVIRON


//----------------------------------------------------------------
// Random
//----------------------------------------------------------------

#if defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_15) && (MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_15)
#include <CommonCrypto/CommonCryptoError.h>
#include <CommonCrypto/CommonRandom.h>

bool _mi_prim_random_buf(void* buf, size_t buf_len) {
  // We prefere CCRandomGenerateBytes as it returns an error code while arc4random_buf
  // may fail silently on macOS. See PR #390, and <https://opensource.apple.com/source/Libc/Libc-1439.40.11/gen/FreeBSD/arc4random.c.auto.html>
  return (CCRandomGenerateBytes(buf, buf_len) == kCCSuccess);
}

#elif defined(__ANDROID__) || defined(__DragonFly__) || \
      defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
      defined(__sun) || \
      (defined(__APPLE__) && (MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_7))

#include <stdlib.h>
bool _mi_prim_random_buf(void* buf, size_t buf_len) {
  arc4random_buf(buf, buf_len);
  return true;
}

#elif defined(__APPLE__) || defined(__linux__) || defined(__HAIKU__)   // also for old apple versions < 10.7 (issue #829)

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

// getrandom
//   https://man7.org/linux/man-pages/man2/getrandom.2.html

bool _mi_prim_random_buf(void* buf, size_t buf_len) {
  // Modern Linux provides `getrandom` but different distributions either use `sys/random.h` or `linux/random.h`
  // and for the latter the actual `getrandom` call is not always defined.
  // (see <https://stackoverflow.com/questions/45237324/why-doesnt-getrandom-compile>)
  // We therefore use a syscall directly and fall back dynamically to /dev/urandom when needed.
  #if defined(MI_HAS_SYSCALL_H) && defined(SYS_getrandom)
    #ifndef GRND_NONBLOCK
    #define GRND_NONBLOCK (1)
    #endif
    static _Atomic(uintptr_t) no_getrandom; // = 0
    if (mi_atomic_load_acquire(&no_getrandom)==0) {
      // GRND_NONBLOCK
      // By default, when reading from the random source,
      // getrandom() blocks if no random bytes are available, and
      // when reading from the urandom source, it blocks if the
      // entropy pool has not yet been initialized.  If the
      // GRND_NONBLOCK flag is set, then getrandom() does not block
      // in these cases, but instead immediately returns -1 with
      // errno set to EAGAIN.
      ssize_t ret = syscall(SYS_getrandom, buf, buf_len, GRND_NONBLOCK);
      if (ret >= 0) return (buf_len == (size_t)ret);
      // ENOSYS
      // The glibc wrapper function for getrandom() determined that
      //the underlying kernel does not implement this system call.
      if (errno != ENOSYS) return false;
      mi_atomic_store_release(&no_getrandom, (uintptr_t)1); // don't call again, and fall back to /dev/urandom
    }
  #endif
  int flags = O_RDONLY;
  #if defined(O_CLOEXEC)
  // O_CLOEXEC (since Linux 2.6.23)
  //
  // Enable the close-on-exec flag for the new file descriptor.
  // Specifying this flag permits a program to avoid additional
  // fcntl(2) F_SETFD operations to set the FD_CLOEXEC flag.
  //
  // Note that the use of this flag is essential in some
  // multithreaded programs, because using a separate fcntl(2)
  // F_SETFD operation to set the FD_CLOEXEC flag does not
  // suffice to avoid race conditions where one thread opens a
  // file descriptor and attempts to set its close-on-exec flag
  // using fcntl(2) at the same time as another thread does a
  // fork(2) plus execve(2).  Depending on the order of
  // execution, the race may lead to the file descriptor
  // returned by open() being unintentionally leaked to the
  // program executed by the child process created by fork(2).
  // (This kind of race is in principle possible for any system
  // call that creates a file descriptor whose close-on-exec
  // flag should be set, and various other Linux system calls
  // provide an equivalent of the O_CLOEXEC flag to deal with
  // this problem.)
  flags |= O_CLOEXEC;
  #endif
  // /dev/urandom
  // https://man7.org/linux/man-pages/man4/random.4.html
  int fd = mi_prim_open("/dev/urandom", flags);
  if (fd < 0) return false;
  size_t count = 0;
  while(count < buf_len) {
    ssize_t ret = mi_prim_read(fd, (char*)buf + count, buf_len - count);
    if (ret<=0) {
      if (errno!=EAGAIN && errno!=EINTR) break;
    }
    else {
      count += ret;
    }
  }
  mi_prim_close(fd);
  return (count==buf_len);
}

#else

bool _mi_prim_random_buf(void* buf, size_t buf_len) {
  return false;
}

#endif


//----------------------------------------------------------------
// Thread init/done
//----------------------------------------------------------------

#if defined(MI_USE_PTHREADS)

// use pthread local storage keys to detect thread ending
// (and used with MI_TLS_PTHREADS for the default heap)
pthread_key_t _mi_heap_default_key = (pthread_key_t)(-1);

static void mi_pthread_done(void* value) {
  if (value!=NULL) {
    _mi_thread_done((mi_heap_t*)value);
  }
}

// Thread-specific Data
// https://sourceware.org/glibc/manual/latest/html_node/Thread_002dspecific-Data.html

// pthread_key_create
//   https://man7.org/linux/man-pages/man3/pthread_key_create.3p.html
//   https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_key_create.html
void _mi_prim_thread_init_auto_done(void) {
  mi_assert_internal(_mi_heap_default_key == (pthread_key_t)(-1));
  pthread_key_create(&_mi_heap_default_key, &mi_pthread_done);
}

// pthread_key_delete
//   https://www.man7.org/linux/man-pages/man3/pthread_key_delete.3p.html
//   https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_key_delete.html
void _mi_prim_thread_done_auto_done(void) {
  if (_mi_heap_default_key != (pthread_key_t)(-1)) {  // do not leak the key, see issue #809
    pthread_key_delete(_mi_heap_default_key);
  }
}

// pthread_setspecific
//   https://man7.org/linux/man-pages/man3/pthread_setspecific.3p.html
//   https://pubs.opengroup.org/onlinepubs/9699919799/functions/pthread_setspecific.html
void _mi_prim_thread_associate_default_heap(mi_heap_t* heap) {
  if (_mi_heap_default_key != (pthread_key_t)(-1)) {  // can happen during recursive invocation on freeBSD
    pthread_setspecific(_mi_heap_default_key, heap);
  }
}

#else

void _mi_prim_thread_init_auto_done(void) {
  // nothing
}

void _mi_prim_thread_done_auto_done(void) {
  // nothing
}

void _mi_prim_thread_associate_default_heap(mi_heap_t* heap) {
  MI_UNUSED(heap);
}

#endif

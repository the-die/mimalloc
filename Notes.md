# mimalloc

[Mimalloc: Free List Sharding in Action](https://www.microsoft.com/en-us/research/publication/mimalloc-free-list-sharding-in-action/)

## Free List Sharding

* free list sharding
* local free list
* thread free list

### The Allocation Free List

```text
free list sharding

+ heap
|
+----+ page
     |
     +----+ free list


```

### No Bump Pointer

```c
struct block_t { struct block_t* next; }

void* malloc_in_page( page_t* page, size_t size ) {
  block_t* block = page->free; // page-local free list
  if (block==NULL) return malloc_generic(size); // slow path
  page->free = block->next;
  page->used++;
  return block;
}
```

The `used` increment is needed to
be able to efficiently determine when all objects in a page are freed.

### The Local Free List

deferred decrement list

* regular free list
  * allocate
* local free list
  * free

```c
page->free = page->local_free; // move the list
page->local_free = NULL; // and the local list is empty again
```

### The Thread Free List

* regular free list
* local free list
* thread free list

```text
T1 --free--> p --belong--> T1
T1 --free--> p --belong--> T2
```

```c
atomic_push( &page->thread_free, p );

void atomic_push( block_t** list, block_t* block ) {
  do {
     block->next = *list;
  } while (!atomic_compare_and_swap(list, block, block->next));
}
```

```c
tfree = atomic_swap( &page->thread_free, NULL );
append( page->free, tfree );
```

## Implementation

```text
                                   tld
                                    ^
+-----+        heap                 |
| tlb | ----> +================+    |
+-----+       |   **********   | <--+
              +================+ <--+
              |       8        |    |
              +----------------+    |
              |       16       |    |
              +----------------+    |
              |       24       |    |
              +----------------+    |
              |       .        |    +---> pages_free_direct
              |       .        |    |
              |       .        |    |
              +----------------+    |
              |      1016      |    |
              +----------------+    |
              |      1024      |    |
              +================+ <==+
              |       8        |    |
              +----------------+    |
              |       16       |    |
              +----------------+    |
              |       32       |    |
              +----------------+    |
              |       .        |    +---> pages
              |       .        |    |
              |       .        |    |
              +----------------+    |
              |      512K      |    |
              +----------------+    |
              |      full      |    |
              +================+ <--+
              |   **********   | <------> thread_delayed_free
              +================+
              |   **********   | <------> thread_id
              +================+
              |   **********   | <------> arena_id
              +================+
              |   **********   | <------> cookie
              +================+
              |       .        |
              |       .        |
              |       .        |
              +================+
```

### Malloc

thread local heap (tlb)

small object(under 1Kb)

```c
void* malloc_small( size_t n ) { // 0 < n <= 1024
  heap_t* heap = tlb;
  page_t* page = heap->pages_direct[(n+7)>>3]; // divide up by 8
  block_t* block = page->free;
  if (block==NULL) return malloc_generic(heap,n); // slow path
  page->free = block->next;
  page->used++;
  return block;
}
```

`segment`(`slab` or `arena`): segment meta data + page meta data + page

small objects under 8KiB: 64KiB, 64
large objects under 512KiB: one page that span the whole segment
huge objects over 512KiB: one page of the required size

### Free

When freeing a pointer, we need to be
able to find the page meta data belonging to that pointer.

```c
void free( void* p ) {
  segment_t* segment = (segment_t*)((uintptr_t)p & ~(4*MB));
  if (segment==NULL) return;
  page_t* page = &segment->pages[(p - segment) >> segment->page_shift];
  block_t* block = (block_t*)p;
  if (thread_id() == segment->thread_id) { // local free
    block->next = page->local_free;
    page->local_free = block;
    page->used--;
    if (page->used - page->thread_freed == 0) page_free(page);
  }
  else { // non-local free
    atomic_push( &page->thread_free, block);
    atomic_incr( &page->thread_freed );
  }
}
```

Note that we read the thread shared thread_freed count without a __read-barrier__ meaning there is tiny probability that we miss that all objects in the page were just all freed. However, that is okay – since we are guaranteed to call the __generic allocation routine__ sometimes, we can collect any such pages later on (and indeed – on asymmetric workloads where some threads only allocate and others only free, the collection in the generic routine is the only way such pages get freed).

### Generic Allocation

The __generic allocation routine__, `malloc_generic`, is our “__slow path__” which is guaranteed to be called every once in a while. This routine gives us the opportunity to do more expensive operations whose cost is amortized over many allocations, and can almost be seen as a form of __garbage collector__.

```c
void* malloc_generic( heap_t* heap, size_t size ) {
  deferred_free();
  foreach( page in heap->pages[size_class(size)] ) {
    page_collect(page);
    if (page->used - page->thread_freed == 0) {
      page_free(page);
    }
    else if (page->free != NULL) {
      return malloc(size);
    }
  }
  .... // allocate a fresh page and malloc from there
}

void page_collect(page) {
  page->free = page->local_free; // move the local free list
  page->local_free = NULL;
  ... // move the thread free list atomically
}

```

## Detail

### `LD_PRELOAD`

[env](https://www.man7.org/linux/man-pages/man1/env.1.html)
[LD_PRELOAD](https://man7.org/linux/man-pages/man8/ld.so.8.html)

### `atomic.h`

Atomic operations library

### `bitmap.h` `bitmap.c`

Concurrent bitmap that can set/reset sequences of bits atomically.

### `random.c`

Random number generator

### `static.c`

You can also directly build the single `src/static.c` file as part of your project without needing `cmake` at all. Make sure to also add the mimalloc `include` directory to the include path.

### `prim.h` `prim.c` `unix/prim.c`

### `init.c`

```
_mi_process_init
    mi_process_load

mi_process_load
    mi_heap_main_init
    _mi_option_init
    mi_process_setup_auto_thread_done
    mi_process_init
    mi_allocator_init
    _mi_random_reInit_if_weak

mi_thread_init
    mi_process_init
    _mi_thread_heap_init

mi_process_init
    mi_heap_main_init
    mi_process_setup_auto_thread_done
    mi_detect_cpu_features
    _mi_os_init
    mi_heap_main_init
    mi_thread_init

_mi_thread_heap_init
    mi_heap_main_init
    _mi_heap_set_default_direct
    mi_thread_data_zalloc
    _mi_tld_init
    _mi_heap_init
    _mi_heap_set_default_direct
```

## TODO

### MI_TLS_RECURSE_GUARD

### mi_process_init

1. mi_option_reserve_huge_os_pages
1. mi_option_reserve_os_memory

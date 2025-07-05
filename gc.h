#ifndef GC_H
#define GC_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <setjmp.h>

#define GC_HEAP_SIZE (1024 * 1024) 
#define GC_THRESHOLD 0.8          
#define GC_MAX_OBJECTS 10000        
#define GC_ALIGNMENT 8          

#define GC_DEBUG 1

#if GC_DEBUG
#define GC_LOG(fmt, ...) printf("[GC] " fmt "\n", ##__VA_ARGS__)
#else
#define GC_LOG(fmt, ...) ((void)0)
#endif

typedef struct gc_object {
    size_t size;
    int marked;
    struct gc_object* next;
    char data[];
} gc_object_t;

typedef struct gc_free_block {
    size_t size;
    struct gc_free_block* next;
} gc_free_block_t;

static struct {
    char* heap;
    size_t heap_size;
    size_t heap_used;
    gc_object_t* objects;
    gc_free_block_t* free_list;
    void* stack_bottom;
    void* stack_top;
    int initialized;
    jmp_buf registers;
    int collection_count;
    int allocation_count;
} gc_state = {0};

// Forward declarations
static void gc_init(void);
static void gc_collect(void);
static void gc_mark_roots(void);
static void gc_mark_object(void* ptr);
static void gc_sweep(void);
static int gc_is_pointer(void* ptr);
static void gc_cleanup(void);
static void* gc_alloc_from_freelist(size_t size);
static void gc_add_to_freelist(void* ptr, size_t size);
static void gc_coalesce_freelist(void);
static gc_object_t* gc_find_object_containing(void* ptr);

// Cleanup function for exit handler
static void gc_cleanup(void) {
    GC_LOG("Cleanup called - unmapping heap");
    if (gc_state.heap != MAP_FAILED) {
        munmap(gc_state.heap, gc_state.heap_size);
    }
    GC_LOG("Final stats - Collections: %d, Allocations: %d", 
           gc_state.collection_count, gc_state.allocation_count);
}

// Get current stack pointer (architecture-specific)
static void* gc_get_stack_pointer(void) {
    void* sp;
#if defined(__x86_64__)
    __asm__ volatile ("movq %%rsp, %0" : "=r" (sp));
#elif defined(__i386__)
    __asm__ volatile ("movl %%esp, %0" : "=r" (sp));
#elif defined(__arm__)
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
#elif defined(__aarch64__)
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
#else
    // Fallback: use address of local variable
    volatile int dummy;
    sp = (void*)&dummy;
#endif
    GC_LOG("Stack pointer: %p", sp);
    return sp;
}

// this functio nwill run automatically before main().
__attribute__((constructor))
static void gc_init_constructor(void) {
    GC_LOG("Constructor called - initializing GC");
    // Get the stack bottom at the earliest possible moment.
    volatile int stack_var;
    gc_state.stack_bottom = (void*)&stack_var;
    GC_LOG("Stack bottom set to: %p", gc_state.stack_bottom);

    // Perform the rest of the initialization.
    gc_init();
}

static void gc_init(void) {
    if (gc_state.initialized) {
        GC_LOG("GC already initialized, skipping");
        return;
    }

    GC_LOG("Initializing GC with heap size: %zu bytes", GC_HEAP_SIZE);

    gc_state.heap = mmap(NULL, GC_HEAP_SIZE, 
                        PROT_READ | PROT_WRITE, 
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (gc_state.heap == MAP_FAILED) {
        fprintf(stderr, "GC: Failed to allocate heap\n");
        exit(1);
    }

    GC_LOG("Heap allocated at: %p", gc_state.heap);

    gc_state.heap_size = GC_HEAP_SIZE;
    gc_state.heap_used = 0;
    gc_state.objects = NULL;
    gc_state.collection_count = 0;
    gc_state.allocation_count = 0;

    gc_state.free_list = (gc_free_block_t*)gc_state.heap;
    gc_state.free_list->size = GC_HEAP_SIZE;
    gc_state.free_list->next = NULL;

    GC_LOG("Initial free block: %p, size: %zu", gc_state.free_list, gc_state.free_list->size);

    gc_state.initialized = 1;

    atexit(gc_cleanup);
    GC_LOG("GC initialization complete");
}

// Check if pointer is within our heap
static int gc_is_pointer(void* ptr) {
    int result = ptr >= (void*)gc_state.heap && 
                 ptr < (void*)(gc_state.heap + gc_state.heap_size);
    if (result) {
        GC_LOG("Pointer %p is within heap bounds", ptr);
    }
    return result;
}

// Check if a value could be a valid pointer
static int gc_is_valid_pointer(uintptr_t value) {
    // Check alignment
    if (value % sizeof(void*) != 0) {
        GC_LOG("Value 0x%lx failed alignment check", value);
        return 0;
    }
    
    // Check if it's in our heap
    int result = gc_is_pointer((void*)value);
    if (result) {
        GC_LOG("Value 0x%lx is a valid pointer", value);
    }
    return result;
}

// Find the object that contains a given pointer
static gc_object_t* gc_find_object_containing(void* ptr) {
    if (!gc_is_pointer(ptr)) {
        GC_LOG("Pointer %p not in heap, cannot find containing object", ptr);
        return NULL;
    }

    GC_LOG("Searching for object containing pointer %p", ptr);
    
    // Search through all objects to find one that contains this pointer
    for (gc_object_t* obj = gc_state.objects; obj; obj = obj->next) {
        void* obj_start = obj->data;
        void* obj_end = (char*)obj->data + obj->size;
        
        if (ptr >= obj_start && ptr < obj_end) {
            GC_LOG("Found object containing %p: object at %p, size %zu", 
                   ptr, obj_start, obj->size);
            return obj;
        }
    }
    
    GC_LOG("No object found containing pointer %p", ptr);
    return NULL;
}

// Mark an object and recursively mark referenced objects
static void gc_mark_object(void* ptr) {
    if (!gc_is_pointer(ptr)) {
        GC_LOG("Pointer %p not in heap, skipping mark", ptr);
        return;
    }

    GC_LOG("Attempting to mark object at %p", ptr);

    // Find the object that contains this pointer
    gc_object_t* obj = gc_find_object_containing(ptr);
    
    if (!obj) {
        GC_LOG("No object found containing %p, skipping mark", ptr);
        return;
    }

    if (obj->marked) {
        GC_LOG("Object at %p already marked, skipping", obj->data);
        return;
    }

    GC_LOG("Marking object at %p, size %zu", obj->data, obj->size);
    obj->marked = 1;
    
    // Scan object data for pointers
    uintptr_t* data = (uintptr_t*)obj->data;
    size_t word_count = obj->size / sizeof(uintptr_t);
    
    GC_LOG("Scanning object data for pointers: %zu words", word_count);
    
    for (size_t i = 0; i < word_count; i++) {
        if (gc_is_valid_pointer(data[i])) {
            GC_LOG("Found potential pointer at offset %zu: 0x%lx", i * sizeof(uintptr_t), data[i]);
            gc_mark_object((void*)data[i]);
        }
    }
    
    GC_LOG("Finished marking object at %p", obj->data);
}

// Mark all reachable objects from roots (stack + registers)
static void gc_mark_roots(void) {
    GC_LOG("Starting root marking phase");
    
    // Save registers to stack
    setjmp(gc_state.registers);
    GC_LOG("Registers saved to jmp_buf");
    
    // Get current stack pointer
    gc_state.stack_top = gc_get_stack_pointer();
    
    // Determine stack direction and bounds
    void* stack_start = gc_state.stack_top;
    void* stack_end = gc_state.stack_bottom;
    
    GC_LOG("Stack scan range: %p to %p", stack_start, stack_end);
    
    // Ensure we scan in the right direction
    if (stack_start > stack_end) {
        void* temp = stack_start;
        stack_start = stack_end;
        stack_end = temp;
        GC_LOG("Stack grows upward, adjusted range: %p to %p", stack_start, stack_end);
    }
    
    // Scan stack for potential pointers
    uintptr_t* ptr = (uintptr_t*)stack_start;
    uintptr_t* end = (uintptr_t*)stack_end;
    
    size_t stack_words = (char*)end - (char*)ptr;
    stack_words /= sizeof(uintptr_t);
    GC_LOG("Scanning stack: %zu words", stack_words);
    
    int stack_pointers_found = 0;
    while (ptr < end) {
        if (gc_is_valid_pointer(*ptr)) {
            GC_LOG("Found stack root at %p: 0x%lx", ptr, *ptr);
            gc_mark_object((void*)*ptr);
            stack_pointers_found++;
        }
        ptr++;
    }
    
    GC_LOG("Stack scan complete: %d potential pointers found", stack_pointers_found);
    
    // Also scan the jmp_buf for registers
    uintptr_t* reg_ptr = (uintptr_t*)&gc_state.registers;
    size_t reg_count = sizeof(jmp_buf) / sizeof(uintptr_t);
    
    GC_LOG("Scanning registers: %zu words", reg_count);
    
    int reg_pointers_found = 0;
    for (size_t i = 0; i < reg_count; i++) {
        if (gc_is_valid_pointer(reg_ptr[i])) {
            GC_LOG("Found register root at index %zu: 0x%lx", i, reg_ptr[i]);
            gc_mark_object((void*)reg_ptr[i]);
            reg_pointers_found++;
        }
    }
    
    GC_LOG("Register scan complete: %d potential pointers found", reg_pointers_found);
    GC_LOG("Root marking phase complete");
}

// Add a block to the free list
static void gc_add_to_freelist(void* ptr, size_t size) {
    if (size < sizeof(gc_free_block_t)) {
        GC_LOG("Block too small for free list: %zu bytes", size);
        return;
    }

    GC_LOG("Adding block to free list: %p, size %zu", ptr, size);

    gc_free_block_t* new_block = (gc_free_block_t*)ptr;
    new_block->size = size;

    // Insert into free list, keeping it sorted by address.
    gc_free_block_t** current = &gc_state.free_list;
    while (*current && (char*)*current < (char*)new_block) {
        current = &(*current)->next;
    }

    new_block->next = *current;
    *current = new_block;
    
    GC_LOG("Block added to free list successfully");
}

static void gc_coalesce_freelist(void) {
    if (!gc_state.free_list) {
        GC_LOG("No free blocks to coalesce");
        return;
    }
    
    GC_LOG("Starting free list coalescing");
    
    gc_free_block_t* current = gc_state.free_list;
    int coalesced_count = 0;
    
    while (current && current->next) {
        char* current_end = (char*)current + current->size;
        char* next_start = (char*)current->next;
        
        if (current_end == next_start) {
            // Adjacent blocks - merge them
            gc_free_block_t* next = current->next;
            GC_LOG("Coalescing blocks: %p (size %zu) + %p (size %zu)", 
                   current, current->size, next, next->size);
            
            current->size += next->size;
            current->next = next->next;
            coalesced_count++;
            
            GC_LOG("Merged block now has size %zu", current->size);
            // Don't advance current - check for more merges
        } else {
            current = current->next;
        }
    }
    
    GC_LOG("Coalescing complete: %d blocks merged", coalesced_count);
}

// Allocate from free list
static void* gc_alloc_from_freelist(size_t size) {
    GC_LOG("Allocating from free list: %zu bytes", size);
    
    gc_free_block_t** current = &gc_state.free_list;
    
    while (*current) {
        gc_free_block_t* block = *current;
        
        GC_LOG("Checking free block: %p, size %zu", block, block->size);
        
        if (block->size >= size) {
            GC_LOG("Found suitable block: %p, size %zu", block, block->size);
            
            // Found a suitable block
            if (block->size >= size + sizeof(gc_free_block_t) + GC_ALIGNMENT) {
                // Split the block
                GC_LOG("Splitting block: using %zu bytes, leaving %zu bytes", 
                       size, block->size - size);
                
                gc_free_block_t* new_block = (gc_free_block_t*)((char*)block + size);
                new_block->size = block->size - size;
                new_block->next = block->next;
                *current = new_block;
                
                GC_LOG("Block split successful");
                return block;
            } else {
                // Use entire block
                GC_LOG("Using entire block");
                *current = block->next;
                return block;
            }
        }
        
        current = &block->next;
    }
    
    GC_LOG("No suitable block found in free list");
    return NULL; // No suitable block found
}

// Sweep phase - free unmarked objects and rebuild free list
static void gc_sweep(void) {
    GC_LOG("Starting sweep phase");
    
    gc_object_t** curr = &gc_state.objects;
    int objects_swept = 0;
    int objects_kept = 0;
    size_t bytes_freed = 0;
    
    while (*curr) {
        gc_object_t* obj = *curr;
        
        if (!obj->marked) {
            GC_LOG("Sweeping unmarked object: %p, size %zu", obj->data, obj->size);
            
            // Remove from objects list
            *curr = obj->next;
            
            // Add to free list
            size_t total_size = sizeof(gc_object_t) + obj->size;
            gc_add_to_freelist(obj, total_size);
            gc_state.heap_used -= total_size;
            
            objects_swept++;
            bytes_freed += total_size;
        } else {
            GC_LOG("Keeping marked object: %p, size %zu", obj->data, obj->size);
            
            // Reset mark for next collection
            obj->marked = 0;
            curr = &obj->next;
            objects_kept++;
        }
    }
    
    GC_LOG("Sweep phase complete: %d objects swept (%zu bytes), %d objects kept", 
           objects_swept, bytes_freed, objects_kept);
    
    // Coalesce adjacent free blocks
    gc_coalesce_freelist();
}

// Main garbage collection routine
static void gc_collect(void) {
    if (!gc_state.initialized) {
        GC_LOG("GC not initialized, skipping collection");
        return;
    }
    
    GC_LOG("===== GARBAGE COLLECTION STARTED (collection #%d) =====", 
           gc_state.collection_count + 1);
    
    size_t heap_used_before = gc_state.heap_used;
    
    // Count objects before collection
    int objects_before = 0;
    for (gc_object_t* obj = gc_state.objects; obj; obj = obj->next) {
        objects_before++;
    }
    
    GC_LOG("Pre-collection state: %d objects, %zu bytes used", 
           objects_before, heap_used_before);
    
    // Mark phase
    GC_LOG("----- MARK PHASE -----");
    gc_mark_roots();
    
    // Count marked objects
    int marked_objects = 0;
    for (gc_object_t* obj = gc_state.objects; obj; obj = obj->next) {
        if (obj->marked) marked_objects++;
    }
    GC_LOG("Mark phase complete: %d objects marked as reachable", marked_objects);
    
    // Sweep phase
    GC_LOG("----- SWEEP PHASE -----");
    gc_sweep();
    
    // Count objects after collection
    int objects_after = 0;
    for (gc_object_t* obj = gc_state.objects; obj; obj = obj->next) {
        objects_after++;
    }
    
    size_t heap_used_after = gc_state.heap_used;
    size_t bytes_freed = heap_used_before - heap_used_after;
    
    gc_state.collection_count++;
    
    GC_LOG("Post-collection state: %d objects, %zu bytes used", 
           objects_after, heap_used_after);
    GC_LOG("Collection results: %zu bytes freed, %d objects collected", 
           bytes_freed, objects_before - objects_after);
    GC_LOG("===== GARBAGE COLLECTION COMPLETE =====");
}

// Custom allocator that replaces malloc
void* gc_malloc(size_t size) {
    if (!gc_state.initialized) {
        GC_LOG("GC not initialized, initializing now");
        gc_init();
    }
    
    GC_LOG("Allocation request: %zu bytes", size);
    
    // Align size to pointer boundary
    size_t aligned_size = (size + GC_ALIGNMENT - 1) & ~(GC_ALIGNMENT - 1);
    if (aligned_size != size) {
        GC_LOG("Size aligned from %zu to %zu", size, aligned_size);
    }
    size = aligned_size;
    
    size_t total_size = sizeof(gc_object_t) + size;
    
    GC_LOG("Total allocation size (with header): %zu bytes", total_size);
    
    // Check if we need to collect garbage
    double heap_usage = (double)gc_state.heap_used / gc_state.heap_size;
    if (gc_state.heap_used + total_size > gc_state.heap_size * GC_THRESHOLD) {
        GC_LOG("Heap usage %.1f%% exceeds threshold %.1f%%, triggering collection", 
               heap_usage * 100, GC_THRESHOLD * 100);
        gc_collect();
    }
    
    // Try to allocate from free list
    void* ptr = gc_alloc_from_freelist(total_size);
    
    if (!ptr) {
        GC_LOG("No suitable free block found, trying collection");
        // No suitable free block found
        gc_collect(); // Try collecting again
        ptr = gc_alloc_from_freelist(total_size);
        
        if (!ptr) {
            GC_LOG("Still no memory after collection - OUT OF MEMORY");
            fprintf(stderr, "GC: Out of memory\n");
            return NULL;
        }
    }
    
    // Initialize object header
    gc_object_t* obj = (gc_object_t*)ptr;
    obj->size = size;
    obj->marked = 0;
    obj->next = gc_state.objects;
    gc_state.objects = obj;
    
    gc_state.heap_used += total_size;
    gc_state.allocation_count++;
    
    // Zero the memory
    memset(obj->data, 0, size);
    
    GC_LOG("Allocation successful: %p (object #%d, user data at %p)", 
           obj, gc_state.allocation_count, obj->data);
    
    return obj->data;
}

// Replacement for calloc
void* gc_calloc(size_t num, size_t size) {
    GC_LOG("Calloc request: %zu items of %zu bytes each", num, size);
    return gc_malloc(num * size);  // gc_malloc already zeros memory
}

// Replacement for realloc
void* gc_realloc(void* ptr, size_t new_size) {
    GC_LOG("Realloc request: %p to %zu bytes", ptr, new_size);
    
    if (!ptr) {
        return gc_malloc(new_size);
    }
    if (new_size == 0) {
        // gc_free is a no-op, but this mimics standard realloc behavior.
        // The object will be collected later if no other references exist.
        return NULL;
    }

    // Find the object header for the given pointer.
    gc_object_t* obj = NULL;
    for (gc_object_t* curr = gc_state.objects; curr; curr = curr->next) {
        if (curr->data == ptr) {
            obj = curr;
            break;
        }
    }

    if (!obj) {
        GC_LOG("Warning: realloc called on non-GC pointer or stale pointer.");
        return gc_malloc(new_size); // Fallback to malloc
    }

    size_t aligned_new_size = (new_size + GC_ALIGNMENT - 1) & ~(GC_ALIGNMENT - 1);

    // If the new size fits in the old block, just return the same pointer.
    if (aligned_new_size <= obj->size) {
        GC_LOG("Shrinking in place. Old size: %zu, New size: %zu", obj->size, aligned_new_size);
        return ptr;

    // --- FALLBACK: Allocate new block and copy ---
    GC_LOG("Fallback: allocating new block and copying data.");
    void* new_ptr = gc_malloc(new_size);
    if (!new_ptr) {
        return NULL; // Out of memory
    }
    
    // Copy data from the old block to the new one.
    memcpy(new_ptr, ptr, obj->size); // Copy up to the old size.

    // The old object (ptr) is now garbage and will be collected on the next cycle.
    return new_ptr;
}

// No-op free
void gc_free(void* ptr) {
    GC_LOG("Free called on %p (no-op - GC handles deallocation)", ptr);
}

// Manual GC trigger (optional)
void gc_force_collect(void) {
    GC_LOG("Force collection requested");
    gc_collect();
}

// Get GC statistics
void gc_stats(void) {
    printf("GC Stats:\n");
    printf("  Heap size: %zu bytes\n", gc_state.heap_size);
    printf("  Heap used: %zu bytes (%.1f%%)\n", 
           gc_state.heap_used, 
           (double)gc_state.heap_used / gc_state.heap_size * 100);
    
    int obj_count = 0;
    for (gc_object_t* curr = gc_state.objects; curr; curr = curr->next) {
        obj_count++;
    }
    printf("  Objects: %d\n", obj_count);
    
    int free_blocks = 0;
    size_t free_bytes = 0;
    for (gc_free_block_t* curr = gc_state.free_list; curr; curr = curr->next) {
        free_blocks++;
        free_bytes += curr->size;
    }
    printf("  Free blocks: %d (%zu bytes)\n", free_blocks, free_bytes);
    printf("  Collections: %d\n", gc_state.collection_count);
    printf("  Allocations: %d\n", gc_state.allocation_count);
}

// Macro overrides for standard allocation functions
#define malloc(size) gc_malloc(size)
#define calloc(num, size) gc_calloc(num, size)
#define realloc(ptr, size) gc_realloc(ptr, size)
#define free(ptr) gc_free(ptr)

#endif
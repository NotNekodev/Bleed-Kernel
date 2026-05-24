#include <mm/pmm.h>
#include <mm/vmm.h>
#include <mm/kalloc.h>
#include <mm/spinlock.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <drivers/serial/serial.h>
#include <ansii.h>

#define SLAB_MAGIC   0xB1EED5AB
#define LARGE_MAGIC  0xB1EEDFA7

static const size_t SIZE_CLASSES[] = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define NUM_CLASSES  (sizeof(SIZE_CLASSES) / sizeof(SIZE_CLASSES[0]))

// MINIMUM alignment
#define KALLOC_ALIGN  8


typedef struct slab_hdr {
    uint32_t         magic;
    uint32_t         class_idx;
    uint32_t         free_count;
    uint32_t         total_count;
    void            *freelist;
    struct slab_hdr *prev;
    struct slab_hdr *next;
} __attribute__((aligned(16))) slab_hdr_t;

typedef struct {
    uint32_t magic;
    uint32_t _pad;
    size_t   pages;
} large_hdr_t;

typedef struct {
    slab_hdr_t *partial;
    slab_hdr_t *empty;
    spinlock_t  lock;
} slab_class_t;

static slab_class_t slab_classes[NUM_CLASSES];
static int          kalloc_init_done = 0;
static spinlock_t   large_lock;

static void kalloc_init(void) {
    if (kalloc_init_done)
        return;
    for (size_t i = 0; i < NUM_CLASSES; i++) {
        slab_classes[i].partial = NULL;
        slab_classes[i].empty   = NULL;
        spinlock_init(&slab_classes[i].lock);
    }
    spinlock_init(&large_lock);
    kalloc_init_done = 1;
}

/*
 * return the index of the smallest size class >= bytes,
 * or NUM_CLASSES if bytes > largest class
 */
static inline size_t class_for(size_t bytes) {
    if (bytes < KALLOC_ALIGN) bytes = KALLOC_ALIGN;

    for (size_t i = 0; i < NUM_CLASSES; i++) {
        if (SIZE_CLASSES[i] >= bytes)
            return i;
    }
    return NUM_CLASSES;
}

static slab_hdr_t *slab_init_page(void *page, size_t ci) {
    slab_hdr_t *hdr = (slab_hdr_t *)page;
    memset(hdr, 0, sizeof(*hdr));

    hdr->magic     = SLAB_MAGIC;
    hdr->class_idx = (uint32_t)ci;
    hdr->prev      = NULL;
    hdr->next      = NULL;

    size_t obj_size    = SIZE_CLASSES[ci];
    size_t hdr_bytes   = (sizeof(slab_hdr_t) + KALLOC_ALIGN - 1) & ~(KALLOC_ALIGN - 1);
    size_t usable      = PAGE_SIZE - hdr_bytes;
    size_t total       = usable / obj_size;

    hdr->total_count = (uint32_t)total;
    hdr->free_count  = (uint32_t)total;

    // build the freelist
    uint8_t *base = (uint8_t *)page + hdr_bytes;
    for (size_t i = 0; i < total; i++) {
        void **obj  = (void **)(base + i * obj_size);
        *obj        = (i + 1 < total) ? (base + (i + 1) * obj_size) : NULL;
    }
    hdr->freelist = (void *)base;

    return hdr;
}

static void slab_push(slab_hdr_t **list, slab_hdr_t *s) {
    s->prev = NULL;
    s->next = *list;
    if (*list) (*list)->prev = s;
    *list = s;
}

static void slab_remove(slab_hdr_t **list, slab_hdr_t *s) {
    if (s->prev) s->prev->next = s->next;
    else         *list = s->next;
    if (s->next) s->next->prev = s->prev;
    s->prev = s->next = NULL;
}

void *kmalloc(size_t bytes) {
    if (!bytes) return NULL;

    if (!kalloc_init_done) kalloc_init();

    size_t ci = class_for(bytes);

    // large alloc
    if (ci == NUM_CLASSES) {
        size_t total_bytes = bytes + sizeof(large_hdr_t);
        size_t pages = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

        unsigned long irq = irq_push();
        spinlock_acquire(&large_lock);
        void *mem = vmm_alloc_pages(pages);
        spinlock_release(&large_lock);
        irq_restore(irq);

        if (!mem) return NULL;

        large_hdr_t *lhdr = (large_hdr_t *)mem;
        lhdr->magic = LARGE_MAGIC;
        lhdr->pages = pages;
        return (uint8_t *)mem + sizeof(large_hdr_t);
    }

    slab_class_t *sc = &slab_classes[ci];

    unsigned long irq = irq_push();
    spinlock_acquire(&sc->lock);

    // partial slabs can be used it gets tried here
    slab_hdr_t *slab = sc->partial;

    if (!slab) {
        // try empty cache
        if (sc->empty) {
            slab = sc->empty;
            sc->empty = NULL;
            slab_push(&sc->partial, slab);
        } else {
            spinlock_release(&sc->lock);
            irq_restore(irq);

            void *page = vmm_alloc_pages(1);
            if (!page) return NULL;

            irq = irq_push();
            spinlock_acquire(&sc->lock);

            slab = slab_init_page(page, ci);
            slab_push(&sc->partial, slab);
        }
    }

    // pop freelist
    void *obj       = slab->freelist;
    slab->freelist  = *(void **)obj;
    slab->free_count--;

    if (slab->free_count == 0)
        slab_remove(&sc->partial, slab);

    spinlock_release(&sc->lock);
    irq_restore(irq);

    return obj;
}

void kfree(void *ptr) {
    if (!ptr) return;

    uintptr_t page_base = (uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1);
    uint32_t  magic     = *(uint32_t *)page_base;

    if (magic == LARGE_MAGIC) {
        large_hdr_t *lhdr = (large_hdr_t *)page_base;
        size_t pages = lhdr->pages;

        unsigned long irq = irq_push();
        spinlock_acquire(&large_lock);
        vmm_free_pages((void *)page_base, pages);
        spinlock_release(&large_lock);
        irq_restore(irq);
        return;
    }

    if (magic == SLAB_MAGIC) {
        slab_hdr_t   *slab = (slab_hdr_t *)page_base;
        size_t        ci   = slab->class_idx;
        slab_class_t *sc   = &slab_classes[ci];

        unsigned long irq = irq_push();
        spinlock_acquire(&sc->lock);

        // back to the free list ready to be used again, thank you for your service now get back to work
        *(void **)ptr  = slab->freelist;
        slab->freelist = ptr;
        slab->free_count++;

        int was_full = (slab->free_count == 1);
        int is_empty = (slab->free_count == slab->total_count);

        if (was_full) {
            slab_push(&sc->partial, slab);
        } else if (is_empty) {
            slab_remove(&sc->partial, slab);
            if (!sc->empty) {
                sc->empty = slab;
            } else {
                spinlock_release(&sc->lock);
                irq_restore(irq);
                vmm_free_pages((void *)page_base, 1);
                return;
            }
        }

        spinlock_release(&sc->lock);
        irq_restore(irq);
        return;
    }

    serial_printf(LOG_WARN "kfree: ptr %p has unknown magic 0x%08x  double free or corruption?\n",
                  ptr, (unsigned)magic);
}
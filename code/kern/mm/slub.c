#include <assert.h>
#include <memlayout.h>
#include <pmm.h>
#include <slub.h>
#include <stdio.h>
#include <string.h>

#define SLUB_MAX_SLABS 16
#define SLUB_ALIGN 8
struct slub_slab { struct Page *page; size_t obj_size, nobj, free_count; void *free_head; uint64_t used[64]; };
static struct slub_slab slabs[SLUB_MAX_SLABS];
static size_t align_up(size_t n) { return (n + SLUB_ALIGN - 1) & ~(SLUB_ALIGN - 1); }
static void *page_memory(struct Page *p) { return (void *)(page2pa(p) + va_pa_offset); }
static void set_used(struct slub_slab *s, size_t i) { s->used[i >> 6] |= 1ULL << (i & 63); }
static void clear_used(struct slub_slab *s, size_t i) { s->used[i >> 6] &= ~(1ULL << (i & 63)); }
static int is_used(struct slub_slab *s, size_t i) { return (s->used[i >> 6] >> (i & 63)) & 1; }
static struct slub_slab *new_slab(size_t size) {
    for (size_t i = 0; i < SLUB_MAX_SLABS; i++) if (!slabs[i].page) {
        struct Page *p = alloc_page(); if (!p) return NULL;
        struct slub_slab *s = &slabs[i]; s->page = p; s->obj_size = size;
        s->nobj = PGSIZE / size; s->free_count = s->nobj; s->free_head = page_memory(p);
        memset(s->used, 0, sizeof(s->used));
        for (size_t j = 0; j < s->nobj; j++) { void *o = (char *)page_memory(p) + j * size; *(void **)o = j + 1 < s->nobj ? (char *)o + size : NULL; }
        return s;
    }
    return NULL;
}
void slub_init(void) { memset(slabs, 0, sizeof(slabs)); }
void *slub_alloc(size_t size) {
    if (!size)
        return NULL;
    size = align_up(size < sizeof(void *) ? sizeof(void *) : size);
    if (size > PGSIZE)
        return NULL;
    struct slub_slab *s = NULL;
    for (size_t i = 0; i < SLUB_MAX_SLABS; i++) if (slabs[i].page && slabs[i].obj_size == size && slabs[i].free_count) { s = &slabs[i]; break; }
    if (!s)
        s = new_slab(size);
    if (!s || !s->free_head)
        return NULL;
    void *o = s->free_head; size_t n = ((uintptr_t)o - (uintptr_t)page_memory(s->page)) / size;
    s->free_head = *(void **)o; s->free_count--; set_used(s, n); return o;
}
void slub_free(void *obj) {
    if (!obj) return;
    for (size_t i = 0; i < SLUB_MAX_SLABS; i++) { struct slub_slab *s = &slabs[i];
        if (s->page && (uintptr_t)obj >= (uintptr_t)page_memory(s->page) && (uintptr_t)obj < (uintptr_t)page_memory(s->page) + PGSIZE && ((uintptr_t)obj - (uintptr_t)page_memory(s->page)) % s->obj_size == 0) {
            size_t n = ((uintptr_t)obj - (uintptr_t)page_memory(s->page)) / s->obj_size; assert(n < s->nobj && is_used(s, n)); clear_used(s, n); *(void **)obj = s->free_head; s->free_head = obj; s->free_count++; return;
        }
    }
    panic("slub_free: invalid object");
}
void slub_check(void) { slub_init(); void *a = slub_alloc(24), *b = slub_alloc(24); assert(a && b && a != b); slub_free(a); assert(slub_alloc(24) == a); slub_free(b); cprintf("slub_test: allocation, reuse and free passed\n"); }

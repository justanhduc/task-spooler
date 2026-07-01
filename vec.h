#ifndef VEC_H
#define VEC_H

#include <stddef.h>

typedef struct {
    void **data;
    size_t size;
    size_t capacity;
} vec_t;

// vec_create() - create a new empty vector, returns NULL on failure
vec_t *vec_create(void);

// vec_init(v) - initialize a stack-allocated vector, returns 0 on success
int vec_init(vec_t *v);

// vec_push(v, item) - append item to the end, returns 0 on success
int vec_push(vec_t *v, void *item);

// vec_pop(v) - remove and return the last item, returns NULL if empty
void *vec_pop(vec_t *v);

// vec_get(v, index) - get item at index without bounds checking
void *vec_get(const vec_t *v, size_t index);

// vec_set(v, index, item) - set item at index, returns old item or NULL
void *vec_set(vec_t *v, size_t index, void *item);

// vec_insert(v, index, item) - insert item at index, shifting right, returns 0 on success
int vec_insert(vec_t *v, size_t index, void *item);

// vec_remove(v, index) - remove item at index, shifting left, returns removed item
void *vec_remove(vec_t *v, size_t index);

// vec_size(v) - return the number of elements
size_t vec_size(const vec_t *v);

// vec_capacity(v) - return the current allocated capacity
size_t vec_capacity(const vec_t *v);

// vec_reserve(v, cap) - ensure capacity at least 'cap', returns 0 on success
int vec_reserve(vec_t *v, size_t cap);

// vec_clear(v) - remove all elements, keeps allocated memory
void vec_clear(vec_t *v);

// vec_free(v) - free the vector and its internal data (use for heap-allocated vectors)
void vec_free(vec_t *v);

// vec_destroy(v) - free only internal data (use for stack-allocated vectors)
void vec_destroy(vec_t *v);

#endif // VEC_H
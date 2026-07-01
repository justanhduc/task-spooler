#include "vec.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VEC_INITIAL_CAPACITY 8

vec_t *vec_create(void)
{
    vec_t *v = (vec_t *)malloc(sizeof(vec_t));
    if (!v) return NULL;
    if (vec_init(v) != 0) {
        free(v);
        return NULL;
    }
    return v;
}

int vec_init(vec_t *v)
{
    if (!v) return -1;
    v->data = (void **)malloc(VEC_INITIAL_CAPACITY * sizeof(void *));
    if (!v->data) return -1;
    v->size = 0;
    v->capacity = VEC_INITIAL_CAPACITY;
    return 0;
}

int vec_push(vec_t *v, void *item)
{
    if (!v || !v->data) return -1;
    if (v->size >= v->capacity) {
        if (v->capacity > SIZE_MAX / 2) return -1;
        if (vec_reserve(v, v->capacity * 2) != 0) return -1;
    }
    v->data[v->size++] = item;
    return 0;
}

void *vec_pop(vec_t *v)
{
    if (!v || !v->data || v->size == 0) return NULL;
    return v->data[--v->size];
}

void *vec_get(const vec_t *v, size_t index)
{
    if (!v || !v->data || index >= v->size) return NULL;
    return v->data[index];
}

void *vec_set(vec_t *v, size_t index, void *item)
{
    if (!v || !v->data || index >= v->size) return NULL;
    void *old = v->data[index];
    v->data[index] = item;
    return old;
}

int vec_insert(vec_t *v, size_t index, void *item)
{
    if (!v || !v->data || index > v->size) return -1;
    if (v->size >= v->capacity) {
        if (v->capacity > SIZE_MAX / 2) return -1;
        if (vec_reserve(v, v->capacity * 2) != 0) return -1;
    }
    memmove(&v->data[index + 1], &v->data[index], (v->size - index) * sizeof(void *));
    v->data[index] = item;
    v->size++;
    return 0;
}

void *vec_remove(vec_t *v, size_t index)
{
    if (!v || !v->data || index >= v->size) return NULL;
    void *removed = v->data[index];
    memmove(&v->data[index], &v->data[index + 1], (v->size - index - 1) * sizeof(void *));
    v->size--;
    return removed;
}

size_t vec_size(const vec_t *v)
{
    return v ? v->size : 0;
}

size_t vec_capacity(const vec_t *v)
{
    return v ? v->capacity : 0;
}

int vec_reserve(vec_t *v, size_t cap)
{
    if (!v || !v->data) return -1;
    if (cap <= v->capacity) return 0;
    void **new_data = (void **)realloc(v->data, cap * sizeof(void *));
    if (!new_data) return -1;
    v->data = new_data;
    v->capacity = cap;
    return 0;
}

void vec_clear(vec_t *v)
{
    if (v && v->data) {
        v->size = 0;
    }
}

void vec_free(vec_t *v)
{
    if (v) {
        free(v->data);
        free(v);
    }
}

void vec_destroy(vec_t *v)
{
    if (v) {
        free(v->data);
        v->data = NULL;
        v->size = 0;
        v->capacity = 0;
    }
}
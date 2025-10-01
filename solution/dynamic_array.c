#include "dynamic_array.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

DynamicArray* da_create(size_t init_capacity) {
    DynamicArray *da = malloc(sizeof(DynamicArray));
    if (!da) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    da->data = malloc(init_capacity * sizeof(char*));
    if (!da->data) {
        perror("malloc");
        free(da);
        exit(EXIT_FAILURE);
    }
    da->size = 0;
    da->capacity = init_capacity;
    return da;
}

void da_put(DynamicArray *da, const char* val) {
    if (!da || !val) return;

    //Resize if the array is full
    if (da->size >= da->capacity) {
        size_t new_capacity = da->capacity == 0 ? 1 : da->capacity * 2;
        char **new_data = realloc(da->data, new_capacity * sizeof(char*));
        if (!new_data) {
            perror("realloc");
            da_free(da);
            exit(EXIT_FAILURE);
        }
        da->data = new_data;
        da->capacity = new_capacity;
    }

    da->data[da->size] = strdup(val);
    if (!da->data[da->size]) {
        perror("strdup");
        da_free(da);
        exit(EXIT_FAILURE);
    }
    da->size++;
}

char *da_get(DynamicArray *da, const size_t ind) {
    if (!da || ind >= da->size) {
        return NULL;
    }
    return da->data[ind];
}

void da_delete(DynamicArray *da, const size_t ind) {
    if (!da || ind >= da->size) {
        return;
    }
    free(da->data[ind]); //Free the string itself
    //Shift elements to the left to fill the gap
    for (size_t i = ind; i < da->size - 1; i++) {
        da->data[i] = da->data[i + 1];
    }
    da->size--;
}

void da_print(DynamicArray *da) {
    if (!da) return;
    for (size_t i = 0; i < da->size; i++) {
        printf("%s\n", da->data[i]);
    }
}

void da_free(DynamicArray *da) {
    if (!da) return;
    for (size_t i = 0; i < da->size; i++) {
        free(da->data[i]);
    }
    free(da->data);
    free(da);
}
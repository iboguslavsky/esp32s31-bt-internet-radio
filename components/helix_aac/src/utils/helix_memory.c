#include "utils/helix_memory.h"
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

void *helix_malloc(int size)
{
    if (size <= 0) {
        size = 1;
    }
    void *ptr = NULL;
#ifdef ESP_PLATFORM
    ptr = heap_caps_calloc(1, (size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_calloc(1, (size_t)size, MALLOC_CAP_8BIT);
    }
#else
    ptr = calloc(1, (size_t)size);
#endif
    return ptr;
}

void helix_free(void *ptr)
{
    if (ptr) {
        free(ptr);
    }
}

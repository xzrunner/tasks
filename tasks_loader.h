#ifdef __cplusplus
extern "C"
{
#endif

#ifndef tasks_loader_h
#define tasks_loader_h

#include <stdbool.h>

struct tasks_loader;

struct tasks_loader* tasks_loader_create(void (*load)(const char* filepath, void (*unpack)(const void* data, size_t size, void* ud), void* ud), int thread_num);
void tasks_loader_release(struct tasks_loader*);

void tasks_loader_clear(struct tasks_loader*);

void tasks_load_file(struct tasks_loader*, const char* filepath, void (*parser)(const void* data, size_t size, void* ud), void* ud, const char* desc);

void tasks_loader_update(struct tasks_loader*);

bool tasks_loader_empty(struct tasks_loader*);

#endif // tasks_loader_h

#ifdef __cplusplus
}
#endif
#ifndef MODERNOS_EVENTFD_H
#define MODERNOS_EVENTFD_H
#include <fs/vfs.h>

struct event_counter *eventfd_allocate(uint32_t initial, bool semaphore);
void eventfd_release(struct event_counter *counter);
long eventfd_transfer(struct file *file, void *value, size_t size, bool write);
unsigned eventfd_poll(struct file *file, unsigned events);
#endif

#ifndef MODERNOS_NETWORK_H
#define MODERNOS_NETWORK_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct file;
struct net_socket;
void network_probe(void *pci_header);
void network_start(void);
long network_syscall(void *task, void *registers);
long network_file_syscall(void *task, void *registers, struct file *);
long network_read(struct file *, void *, size_t, unsigned flags);
long network_write(struct file *, const void *, size_t, unsigned flags);
unsigned network_poll(struct file *, unsigned events);
void network_close(struct file *);
long network_ioctl(struct file *, unsigned long request, void *value);
size_t network_info(char *, size_t);
#endif

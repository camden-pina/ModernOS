/*
 * Lightweight AML Interpreter
 * Copyright (C) 2018-2023 The lai authors
 */

#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lai_nsnode;
typedef struct lai_nsnode lai_nsnode_t;

struct lai_variable_t;
typedef struct lai_variable_t lai_variable_t;

#define LAI_DEBUG_LOG 1
#define LAI_WARN_LOG 2

struct lai_sync_state {
    // Used internally by LAI. Read-only for the host.
    unsigned int val;

    // Freely available to the host.
    // Intended to implement a mutex that protects p.
    unsigned int s;

    // Freely available to the host.
    // Intended to hold a pointer to a wait queue.
    void *p;
};

// OS-specific functions.
void *laihost_malloc(size_t sz);
void *laihost_realloc(void *, size_t, size_t);
void laihost_free(void *ptr, size_t sz);

void laihost_log(int level, const char *message);
void laihost_panic(const char *message) __attribute__((noreturn));

void *laihost_scan(char *signature, size_t index);
void *laihost_map(size_t address, size_t size);
void laihost_unmap(void *, size_t);
void laihost_outb(uint16_t port, uint8_t value);
void laihost_outw(uint16_t port, uint16_t value);
void laihost_outd(uint16_t port, uint32_t value);

uint8_t laihost_inb(uint16_t port);
uint16_t laihost_inw(uint16_t port);
uint32_t laihost_ind(uint16_t port);

void laihost_pci_writeb(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset, uint8_t value);
uint8_t laihost_pci_readb(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset);

void laihost_pci_writew(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset, uint16_t value);
uint16_t laihost_pci_readw(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset);

void laihost_pci_writed(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset, uint32_t value);
uint32_t laihost_pci_readd(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset);

void laihost_sleep(uint64_t duration);
uint64_t laihost_timer(void);

int laihost_sync_wait(struct lai_sync_state *, unsigned int val, int64_t deadline);
void laihost_sync_wake(struct lai_sync_state *);


void laihost_handle_amldebug(lai_variable_t *variable);
void laihost_handle_global_notify(lai_nsnode_t *, int);

#ifndef COAST_DISPLAY_H
#define COAST_DISPLAY_H
#include <uapi/abi.h>
#include <input/mouse.h>
#include <uapi/keyboard.h>

struct task;
long display_call(struct task *task, unsigned operation, uint64_t pointer, uint64_t size);
long display_input(struct task *task, uint64_t pointer, uint64_t capacity);
long display_input_v2(struct task *, uint64_t pointer, uint64_t capacity);
void display_drop(unsigned pid);
bool display_owned(unsigned pid);
void display_key(uint8_t key);
bool display_hid(uint8_t usage, uint8_t ascii, uint8_t modifiers, bool pressed);
void display_pointer(const mouse_event_t *event);
void display_keyboard_sync(const struct os_keyboard_snapshot *);
#endif

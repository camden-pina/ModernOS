#include <input/mouse.h>
#include <string.h>

#define MOUSE_QUEUE_SIZE 256u

static struct {
    mouse_event_t events[MOUSE_QUEUE_SIZE];
    mouse_state_t states[MOUSE_MAX_SOURCES];
    bool known[MOUSE_MAX_SOURCES];
    uint16_t head;
    uint16_t tail;
    uint64_t sequence;
    uint64_t dropped;
    bool overrun;
} mouse;

static void enqueue(uint16_t source_id, uint16_t flags, uint32_t buttons, uint32_t changed,
                    int32_t dx, int32_t dy, int32_t wheel, int32_t hwheel) {
    uint16_t next = (uint16_t)((mouse.head + 1u) % MOUSE_QUEUE_SIZE);
    if (next == mouse.tail) {
        mouse.tail = (uint16_t)((mouse.tail + 1u) % MOUSE_QUEUE_SIZE);
        ++mouse.dropped;
        mouse.overrun = true;
    }
    mouse.events[mouse.head] = (mouse_event_t){.sequence = ++mouse.sequence,
                                               .source_id = source_id,
                                               .flags = flags,
                                               .buttons = buttons,
                                               .changed = changed,
                                               .dx = dx,
                                               .dy = dy,
                                               .wheel = wheel,
                                               .hwheel = hwheel};
    mouse.head = next;
}

void mouse_init(void) {
    memset(&mouse, 0, sizeof(mouse));
}

bool mouse_attach(uint16_t source_id) {
    if (!source_id || source_id >= MOUSE_MAX_SOURCES || mouse.states[source_id].connected)
        return false;
    mouse.known[source_id] = true;
    mouse.states[source_id] = (mouse_state_t){.connected = true};
    enqueue(source_id, MOUSE_EVENT_CONNECTED, 0, 0, 0, 0, 0, 0);
    return true;
}

void mouse_detach(uint16_t source_id) {
    if (!source_id || source_id >= MOUSE_MAX_SOURCES || !mouse.states[source_id].connected)
        return;
    uint32_t changed = mouse.states[source_id].buttons;
    mouse.states[source_id] = (mouse_state_t){0};
    enqueue(source_id, MOUSE_EVENT_DISCONNECTED, 0, changed, 0, 0, 0, 0);
}

void mouse_publish(uint16_t source_id, int32_t dx, int32_t dy, int32_t wheel, int32_t hwheel,
                   uint32_t buttons) {
    if (!source_id || source_id >= MOUSE_MAX_SOURCES || !mouse.states[source_id].connected)
        return;
    uint32_t changed = mouse.states[source_id].buttons ^ buttons;
    mouse.states[source_id].buttons = buttons;
    if (dx || dy || wheel || hwheel || changed)
        enqueue(source_id, 0, buttons, changed, dx, dy, wheel, hwheel);
}

bool mouse_read_event(mouse_event_t *out) {
    if (!out || mouse.head == mouse.tail)
        return false;
    *out = mouse.events[mouse.tail];
    mouse.tail = (uint16_t)((mouse.tail + 1u) % MOUSE_QUEUE_SIZE);
    if (mouse.overrun) {
        out->flags |= MOUSE_EVENT_OVERRUN;
        mouse.overrun = false;
    }
    return true;
}

size_t mouse_read_events(mouse_event_t *out, size_t capacity) {
    size_t count = 0;
    if (out)
        while (count < capacity && mouse_read_event(out + count))
            ++count;
    return count;
}

bool mouse_get_state(uint16_t source_id, mouse_state_t *out) {
    if (!out || !source_id || source_id >= MOUSE_MAX_SOURCES || !mouse.known[source_id])
        return false;
    *out = mouse.states[source_id];
    return true;
}

uint64_t mouse_dropped_events(void) {
    return mouse.dropped;
}

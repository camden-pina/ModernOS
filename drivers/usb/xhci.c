#include <drivers/usb/xhci.h>
#include <drivers/usb/xhci_structs.h>
#include <drivers/usb/usb_platform.h>
#include <printk.h>
#include <string.h>

#define XHCI_PAGE_SIZE USB_DMA_PAGE_SIZE
#define XHCI_RING_TRBS 256u
#define XHCI_LINK_INDEX 255u
#define XHCI_MAX_SLOTS USB_MAX_DEVICES
#define XHCI_TIMEOUT_MS 2000u
#define XHCI_SPIN_LIMIT 50000000u
#define XHCI_MAX_MMIO 0x1000000u

#define USBCMD_RUN (1u << 0)
#define USBCMD_RESET (1u << 1)
#define USBCMD_INTE (1u << 2)
#define USBSTS_HALTED (1u << 0)
#define USBSTS_HSE (1u << 2)
#define USBSTS_EINT (1u << 3)
#define USBSTS_CNR (1u << 11)
#define USBSTS_HCE (1u << 12)

#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_RESET (1u << 4)
#define PORT_POWER (1u << 9)
#define PORT_CSC (1u << 17)
#define PORT_PRC (1u << 21)
#define PORT_CHANGES (0x7fu << 17)

/* Preserve writable persistent fields, excluding PED (RW1C), PR, WPR,
 * LWS, and every change bit. Do not blindly write a PORTSC snapshot back.
 */
#define PORT_PRESERVE ((15u << 10) | PORT_CCS | (1u << 3) | \
                       (1u << 30) | PORT_POWER | (3u << 14) | (7u << 25))

typedef struct {
    xhci_trb_t *trbs;
    uint64_t phys;
    uint16_t enqueue;
    uint8_t cycle;
} xhci_ring_t;

typedef struct {
    bool configured;
    bool pending;
    bool failed;
    usb_endpoint_t descriptor;
    uint8_t dci;
    uint8_t interval;
    uint8_t *buffer;
    uint64_t buffer_phys;
    uint64_t pending_trb;
    usb_interrupt_callback_t callback;
    void *context;
    xhci_ring_t ring;
} xhci_interrupt_pipe_t;

typedef struct {
    bool allocated;
    bool connected;
    uint8_t slot_id;
    uint8_t port;
    uint8_t speed;
    uint16_t ep0_mps;
    uint8_t *input;
    uint64_t input_phys;
    uint8_t *output;
    uint8_t *buffer;
    uint64_t buffer_phys;
    usb_device_t usb;
    xhci_ring_t ep0;
    /* IN endpoint numbers 1..15 each have their own ring and DMA buffer. */
    xhci_interrupt_pipe_t interrupt[16];
} xhci_device_t;

typedef struct {
    bool pending;
    bool done;
    uint64_t trb_phys;
    uint8_t code;
    uint8_t slot;
} xhci_command_wait_t;

typedef struct {
    bool pending;
    bool done;
    uint8_t slot;
    uint8_t code;
    uint16_t requested;
    uint16_t actual;
    uint64_t setup_trb;
    uint64_t data_trb;
    uint64_t status_trb;
} xhci_control_wait_t;

static struct {
    bool initialized;
    bool running;
    bool failed;
    bool ac64;
    uint8_t context_stride;
    uint8_t slots;
    uint8_t ports;
    bool usb2_port[256];
    uint8_t slot_type[256];
    uint64_t mmio_phys;
    volatile uint8_t *mmio;
    size_t mmio_size;
    volatile xhci_op_regs_t *op;
    volatile xhci_interrupter_regs_t *intr;
    volatile uint32_t *doorbells;
    uint64_t *dcbaa;
    xhci_ring_t command;
    volatile xhci_trb_t *events;
    uint64_t events_phys;
    uint16_t event_dequeue;
    uint8_t event_cycle;
    xhci_command_wait_t command_wait;
    xhci_control_wait_t control_wait;
    xhci_device_t devices[XHCI_MAX_SLOTS + 1];
} hc;

typedef struct {
    bool clock;
    uint64_t start;
    uint32_t spins;
    uint32_t ms;
} xhci_deadline_t;

static xhci_deadline_t deadline_start(uint32_t ms) {
    xhci_deadline_t d = {.spins = XHCI_SPIN_LIMIT, .ms = ms};
    d.clock = usb_platform_time_ms(&d.start);
    return d;
}

static bool deadline_expired(xhci_deadline_t *d) {
    uint64_t now;
    if (d->clock) {
        if (usb_platform_time_ms(&now))
            return now - d->start >= d->ms;
        /* If a previously available clock fails, use the bounded fallback. */
        d->clock = false;
    }
    if (d->spins == 0)
        return true;
    --d->spins;
    return false;
}

static bool wait_bits(volatile uint32_t *reg, uint32_t mask, uint32_t wanted, uint32_t ms) {
    xhci_deadline_t d = deadline_start(ms);
    while ((*reg & mask) != wanted) {
        if (deadline_expired(&d))
            return false;
        usb_platform_relax();
    }
    return true;
}

static inline void dma_release(void) {
    __asm__ volatile("mfence" ::: "memory");
}

static inline void dma_acquire(void) {
    __asm__ volatile("lfence" ::: "memory");
}

static void notify_disconnect(xhci_device_t *dev);

static void fail_controller(const char *reason) {
    if (!hc.failed)
        kprintf("xHCI stopped: %s\n\r", reason);
    hc.failed = true;
    hc.running = false;
    if (hc.op)
        hc.op->usbcmd &= ~(USBCMD_RUN | USBCMD_INTE);
    for (unsigned i = 1; i <= hc.slots; ++i)
        if (hc.devices[i].allocated && hc.devices[i].connected)
            notify_disconnect(&hc.devices[i]);
    /* Keep every DMA allocation alive. In particular, a timed-out command
     * must never leave a ring/context being recycled under live hardware.
     */
}

static uint64_t alloc_dma(size_t pages, void **out) {
    return usb_platform_alloc_dma(pages, hc.ac64, out);
}

static bool ring_init(xhci_ring_t *ring) {
    void *memory;
    ring->phys = alloc_dma(1, &memory);
    if (!ring->phys)
        return false;
    ring->trbs = memory;
    ring->enqueue = 0;
    ring->cycle = 1;
    ring->trbs[XHCI_LINK_INDEX].param = ring->phys;
    /* Link stays unpublished until the producer reaches it. */
    ring->trbs[XHCI_LINK_INDEX].control = XHCI_TRB_TYPE(TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE;
    return true;
}

static uint64_t ring_push(xhci_ring_t *ring, uint64_t param, uint32_t status, uint32_t control) {
    uint16_t index = ring->enqueue;
    volatile xhci_trb_t *trb = &ring->trbs[index];
    trb->param = param;
    trb->status = status;
    dma_release();
    trb->control = (control & ~XHCI_TRB_CYCLE) | ring->cycle;
    if (++ring->enqueue == XHCI_LINK_INDEX) {
        dma_release();
        ring->trbs[XHCI_LINK_INDEX].control =
            XHCI_TRB_TYPE(TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1u;
    }
    return ring->phys + (uint64_t)index * sizeof(xhci_trb_t);
}

static void ring_doorbell(uint8_t slot, uint8_t target) {
    dma_release();
    hc.doorbells[slot] = target;
    (void)hc.doorbells[slot]; /* Flush posted MMIO write. */
}

static xhci_input_control_context_t *input_control(xhci_device_t *dev) {
    return (xhci_input_control_context_t *)dev->input;
}

static xhci_slot_context_t *input_slot(xhci_device_t *dev) {
    return (xhci_slot_context_t *)(dev->input + hc.context_stride);
}

static xhci_endpoint_context_t *input_endpoint(xhci_device_t *dev, uint8_t dci) {
    return (xhci_endpoint_context_t *)(dev->input + (size_t)(dci + 1u) * hc.context_stride);
}

static volatile uint32_t *port_reg(uint8_t port) {
    return (volatile uint32_t *)((volatile uint8_t *)hc.op + 0x400u + (size_t)(port - 1u) * 0x10u);
}

static void acknowledge_port(uint8_t port) {
    volatile uint32_t *reg = port_reg(port);
    uint32_t value = *reg;
    *reg = (value & PORT_PRESERVE) | (value & PORT_CHANGES);
}

/* The host driver routes bytes only. HID interpretation belongs to classes. */
static void stop_interrupt_pipe(xhci_interrupt_pipe_t *pipe) {
    pipe->failed = true;
    pipe->pending = false;
    pipe->callback = NULL;
    pipe->context = NULL;
}

static void notify_disconnect(xhci_device_t *dev) {
    if (!dev->connected)
        return;
    dev->connected = false;
    dev->usb.connected = false;
    for (unsigned i = 1; i < 16; ++i)
        stop_interrupt_pipe(&dev->interrupt[i]);
    if (dev->usb.on_disconnect)
        dev->usb.on_disconnect(&dev->usb);
}

static bool backend_submit_interrupt_in(usb_device_t *device, uint8_t endpoint_address,
                                        usb_interrupt_callback_t callback, void *context) {
    xhci_device_t *dev = device ? device->host_data : NULL;
    if (!dev || !callback || !dev->connected || !device->connected || !hc.running || hc.failed ||
        !(endpoint_address & 0x80u) || !(endpoint_address & 15u) || (endpoint_address & 0x70u))
        return false;
    xhci_interrupt_pipe_t *pipe = &dev->interrupt[endpoint_address & 15u];
    if (!pipe->configured || pipe->failed || pipe->pending ||
        pipe->descriptor.address != endpoint_address)
        return false;
    memset(pipe->buffer, 0, pipe->descriptor.max_packet_size);
    pipe->callback = callback;
    pipe->context = context;
    pipe->pending_trb = ring_push(&pipe->ring, pipe->buffer_phys, pipe->descriptor.max_packet_size,
                                  XHCI_TRB_TYPE(TRB_NORMAL) | XHCI_TRB_IOC | XHCI_TRB_ISP);
    pipe->pending = true;
    ring_doorbell(dev->slot_id, pipe->dci);
    return true;
}

static void dispatch_event(const xhci_trb_t *event) {
    uint8_t type = (uint8_t)((event->control >> 10) & 0x3fu);
    uint8_t code = (uint8_t)(event->status >> 24);
    uint8_t slot = (uint8_t)(event->control >> 24);
    if (type == TRB_COMMAND_COMPLETION_EVENT) {
        if (hc.command_wait.pending && event->param == hc.command_wait.trb_phys) {
            hc.command_wait.code = code;
            hc.command_wait.slot = slot;
            hc.command_wait.done = true;
        }
        return;
    }
    if (type == TRB_PORT_STATUS_CHANGE_EVENT) {
        uint8_t port = (uint8_t)(event->param >> 24);
        if (port == 0 || port > hc.ports)
            return;
        uint32_t value = *port_reg(port);
        acknowledge_port(port);
        for (unsigned i = 1; i <= hc.slots; ++i) {
            xhci_device_t *dev = &hc.devices[i];
            if (dev->allocated && dev->port == port &&
                (!(value & PORT_CCS) || !(value & PORT_PED) || (value & PORT_CSC))) {
                notify_disconnect(dev);
            }
        }
        return; /* New devices are enumerated at init only. */
    }
    if (type == TRB_HOST_CONTROLLER_EVENT) {
        fail_controller("Host Controller Event");
        return;
    }
    if (type != TRB_TRANSFER_EVENT || slot == 0 || slot > hc.slots)
        return;
    /* This driver never queues Event Data TRBs: ED=1 is not a TRB pointer. */
    if (event->control & (1u << 2))
        return;
    uint8_t dci = (uint8_t)((event->control >> 16) & 0x1fu);
    uint64_t pointer = event->param & ~0xfull;
    uint32_t residual = event->status & 0xffffffu;
    xhci_control_wait_t *wait = &hc.control_wait;
    if (dci == 1 && wait->pending && wait->slot == slot &&
        (pointer == wait->setup_trb || pointer == wait->data_trb || pointer == wait->status_trb)) {
        if (code == XHCI_CC_SHORT_PACKET && pointer == wait->data_trb) {
            if (residual > wait->requested) {
                wait->code = 0;
                wait->done = true;
            } else {
                wait->actual = (uint16_t)(wait->requested - residual);
            }
            /* Do not finish until the Status Stage completes. */
        } else if (code != XHCI_CC_SUCCESS || pointer == wait->status_trb) {
            wait->code = code;
            wait->done = true;
        }
        return;
    }
    xhci_device_t *dev = &hc.devices[slot];
    if (!dev->connected || dci < 3 || !(dci & 1u))
        return;
    xhci_interrupt_pipe_t *pipe = &dev->interrupt[dci / 2u];
    if (!pipe->configured || !pipe->pending || pipe->dci != dci || pointer != pipe->pending_trb)
        return;
    usb_interrupt_callback_t callback = pipe->callback;
    void *context = pipe->context;
    pipe->pending = false;
    pipe->callback = NULL;
    pipe->context = NULL;
    if ((code != XHCI_CC_SUCCESS && code != XHCI_CC_SHORT_PACKET) ||
        residual > pipe->descriptor.max_packet_size) {
        kprintf("xHCI device %u endpoint %u: transfer error %u\n\r", slot, pipe->descriptor.address,
                code);
        stop_interrupt_pipe(pipe);
        if (callback)
            callback(&dev->usb, USB_TRANSFER_ERROR, NULL, 0, context);
        return;
    }
    dma_acquire();
    size_t length = pipe->descriptor.max_packet_size - residual;
    usb_transfer_status_t status =
        code == XHCI_CC_SHORT_PACKET ? USB_TRANSFER_SHORT_PACKET : USB_TRANSFER_SUCCESS;
    if (callback)
        callback(&dev->usb, status, pipe->buffer, length, context);
}

static bool consume_one_event(void) {
    volatile xhci_trb_t *source = &hc.events[hc.event_dequeue];
    uint32_t control = source->control;
    if ((control & XHCI_TRB_CYCLE) != hc.event_cycle)
        return false;
    dma_acquire();
    xhci_trb_t event = {.param = source->param, .status = source->status, .control = control};
    if (++hc.event_dequeue == XHCI_RING_TRBS) {
        hc.event_dequeue = 0;
        hc.event_cycle ^= 1u;
    }
    /* Release this event before any handler queues commands/transfers. */
    dma_release();
    hc.intr->erdp = hc.events_phys + (uint64_t)hc.event_dequeue * 16u + (1u << 3);
    hc.intr->iman = 1u;          /* IP RW1C; IE stays disabled for polling. */
    hc.op->usbsts = USBSTS_EINT; /* EINT RW1C, no blanket status writeback. */
    dispatch_event(&event);
    return true;
}

static void poll_events(void) {
    if (hc.failed || !hc.events)
        return;
    if (hc.op->usbsts & (USBSTS_HSE | USBSTS_HCE)) {
        fail_controller("host controller/system error");
        return;
    }
    /* Bounded work per call even if the hardware continually produces events. */
    for (unsigned i = 0; i < XHCI_RING_TRBS && !hc.failed; ++i)
        if (!consume_one_event())
            break;
}

static bool command(uint32_t type, uint64_t param, uint32_t control, uint8_t *result_slot) {
    if (hc.failed || hc.command_wait.pending)
        return false;
    hc.command_wait = (xhci_command_wait_t){.pending = true};
    hc.command_wait.trb_phys = ring_push(&hc.command, param, 0, XHCI_TRB_TYPE(type) | control);
    ring_doorbell(0, 0);
    xhci_deadline_t d = deadline_start(XHCI_TIMEOUT_MS);
    while (!hc.command_wait.done && !hc.failed) {
        poll_events();
        if (hc.command_wait.done || hc.failed)
            break;
        if (deadline_expired(&d)) {
            fail_controller("command timeout");
            break;
        }
        usb_platform_relax();
    }
    bool ok = !hc.failed && hc.command_wait.done && hc.command_wait.code == XHCI_CC_SUCCESS;
    if (result_slot)
        *result_slot = hc.command_wait.slot;
    if (!ok && !hc.failed)
        kprintf("xHCI command %u failed, completion code %u\n\r", type, hc.command_wait.code);
    hc.command_wait.pending = false;
    return ok;
}

static bool control_transfer(xhci_device_t *dev, uint8_t request_type, uint8_t request,
                             uint16_t value, uint16_t index, uint16_t length, uint16_t *actual) {
    if (hc.failed || hc.control_wait.pending || !dev->connected || length > XHCI_PAGE_SIZE)
        return false;
    bool in = (request_type & 0x80u) != 0;
    if (length && in)
        memset(dev->buffer, 0, length);
    uint64_t setup = (uint64_t)request_type | ((uint64_t)request << 8) | ((uint64_t)value << 16) |
                     ((uint64_t)index << 32) | ((uint64_t)length << 48);
    uint32_t trt = length ? (in ? 3u : 2u) : 0u;
    xhci_control_wait_t *wait = &hc.control_wait;
    *wait = (xhci_control_wait_t){
        .pending = true, .slot = dev->slot_id, .requested = length, .actual = length};
    wait->setup_trb =
        ring_push(&dev->ep0, setup, 8, XHCI_TRB_TYPE(TRB_SETUP_STAGE) | XHCI_TRB_IDT | (trt << 16));
    if (length)
        wait->data_trb =
            ring_push(&dev->ep0, dev->buffer_phys, length,
                      XHCI_TRB_TYPE(TRB_DATA_STAGE) | XHCI_TRB_ISP | (in ? XHCI_TRB_DIR_IN : 0));
    /* Opposite the data direction; no-data requests always have IN status. */
    wait->status_trb = ring_push(&dev->ep0, 0, 0,
                                 XHCI_TRB_TYPE(TRB_STATUS_STAGE) | XHCI_TRB_IOC |
                                     ((!length || !in) ? XHCI_TRB_DIR_IN : 0));
    ring_doorbell(dev->slot_id, 1); /* EP0's Device Context Index is 1. */
    xhci_deadline_t d = deadline_start(XHCI_TIMEOUT_MS);
    while (!wait->done && !hc.failed && dev->connected) {
        poll_events();
        if (wait->done || hc.failed || !dev->connected)
            break;
        if (deadline_expired(&d)) {
            fail_controller("control transfer timeout");
            break;
        }
        usb_platform_relax();
    }
    bool ok = !hc.failed && dev->connected && wait->done && wait->code == XHCI_CC_SUCCESS;
    if (actual)
        *actual = ok ? wait->actual : 0;
    if (!ok && !hc.failed)
        kprintf("xHCI slot %u request %u failed, completion code %u\n\r", dev->slot_id, request,
                wait->code);
    wait->pending = false;
    /* An unsupported class request can STALL EP0 without breaking other
     * interfaces. xHCI requires Reset Endpoint, then skipping all remaining
     * stages of the failed request. There is only one EP0 request in flight.
     * Do not send USB CLEAR_FEATURE to a control endpoint.
     */
    if (!ok && !hc.failed && dev->connected && wait->done && wait->code == XHCI_CC_STALL_ERROR) {
        uint32_t target = ((uint32_t)dev->slot_id << 24) | (1u << 16);
        uint64_t next = dev->ep0.phys + (uint64_t)dev->ep0.enqueue * 16u;
        if (!command(TRB_RESET_ENDPOINT, 0, target, NULL) ||
            !command(TRB_SET_TR_DEQUEUE_POINTER, next | dev->ep0.cycle, target, NULL))
            fail_controller("EP0 stall recovery failed");
    }
    dma_acquire();
    return ok;
}

static bool backend_control(usb_device_t *device, const usb_setup_packet_t *setup, void *data,
                            uint16_t *actual) {
    xhci_device_t *dev = device ? device->host_data : NULL;
    if (actual)
        *actual = 0;
    if (!dev || !setup || setup->length > XHCI_PAGE_SIZE || (setup->length && !data))
        return false;
    bool in = (setup->request_type & 0x80u) != 0;
    if (setup->length && !in)
        memcpy(dev->buffer, data, setup->length);
    uint16_t received = 0;
    bool ok = control_transfer(dev, setup->request_type, setup->request, setup->value, setup->index,
                               setup->length, &received);
    if (ok && in && received)
        memcpy(data, dev->buffer, received);
    if (actual)
        *actual = ok ? received : 0;
    return ok;
}

static uint8_t interrupt_interval(uint8_t speed, uint8_t b_interval) {
    if (speed == 3)
        return (uint8_t)(b_interval - 1u);
    unsigned microframes = (unsigned)b_interval * 8u;
    uint8_t exponent = 0;
    while (microframes > 1u) {
        microframes >>= 1;
        ++exponent;
    }
    return exponent; /* FS/LS: floor(log2(bInterval * 8)), range 3..10. */
}

static bool backend_set_ep0_packet_size(usb_device_t *device, uint16_t mps) {
    xhci_device_t *dev = device ? device->host_data : NULL;
    if (!dev || !dev->connected || hc.failed)
        return false;
    if (mps == dev->ep0_mps)
        return true;
    memset(dev->input, 0, XHCI_PAGE_SIZE);
    input_control(dev)->add_flags = 1u << 1;
    xhci_endpoint_context_t *ep = input_endpoint(dev, 1);
    dma_acquire();
    memcpy(ep, dev->output + hc.context_stride, sizeof(*ep));
    ep->info2 = (ep->info2 & 0xffffu) | ((uint32_t)mps << 16);
    if (!command(TRB_EVALUATE_CONTEXT, dev->input_phys, (uint32_t)dev->slot_id << 24, NULL))
        return false;
    dev->ep0_mps = mps;
    device->ep0_max_packet_size = mps;
    return true;
}

static bool backend_configure_interrupt_in(usb_device_t *device, const usb_endpoint_t *endpoint) {
    xhci_device_t *dev = device ? device->host_data : NULL;
    if (!dev || !endpoint || !dev->connected || hc.failed || !(endpoint->address & 0x80u) ||
        !(endpoint->address & 15u) || (endpoint->address & 0x70u) ||
        (endpoint->attributes & 3u) != 3u || !endpoint->max_packet_size || !endpoint->interval ||
        (dev->speed == USB_SPEED_LOW && endpoint->max_packet_size > 8) ||
        (dev->speed == USB_SPEED_FULL && endpoint->max_packet_size > 64) ||
        (dev->speed == USB_SPEED_HIGH &&
         (endpoint->max_packet_size > 1024 || endpoint->interval > 16)))
        return false;
    xhci_interrupt_pipe_t *pipe = &dev->interrupt[endpoint->address & 15u];
    if (pipe->configured) {
        return !pipe->failed && pipe->descriptor.address == endpoint->address &&
               pipe->descriptor.attributes == endpoint->attributes &&
               pipe->descriptor.max_packet_size == endpoint->max_packet_size &&
               pipe->descriptor.interval == endpoint->interval;
    }
    if (!ring_init(&pipe->ring))
        return false;
    void *memory;
    pipe->buffer_phys = alloc_dma(1, &memory);
    if (!pipe->buffer_phys)
        return false;
    pipe->buffer = memory;
    pipe->descriptor = *endpoint;
    pipe->dci = (uint8_t)((endpoint->address & 15u) * 2u + 1u);
    pipe->interval = interrupt_interval(dev->speed, endpoint->interval);

    memset(dev->input, 0, XHCI_PAGE_SIZE);
    input_control(dev)->add_flags = 1u | (1u << pipe->dci);
    xhci_slot_context_t *slot = input_slot(dev);
    dma_acquire();
    memcpy(slot, dev->output, sizeof(*slot));
    uint8_t entries = (uint8_t)((slot->info1 >> 27) & 31u);
    if (entries < pipe->dci)
        entries = pipe->dci;
    slot->info1 = (slot->info1 & ~(31u << 27)) | ((uint32_t)entries << 27);
    xhci_endpoint_context_t *ep = input_endpoint(dev, pipe->dci);
    ep->info1 = (uint32_t)pipe->interval << 16;
    ep->info2 = (3u << 1) | (7u << 3) | ((uint32_t)endpoint->max_packet_size << 16);
    ep->tr_dequeue_ptr = pipe->ring.phys | 1u;
    ep->tx_info = (uint32_t)endpoint->max_packet_size | ((uint32_t)endpoint->max_packet_size << 16);
    if (!command(TRB_CONFIGURE_ENDPOINT, dev->input_phys, (uint32_t)dev->slot_id << 24, NULL))
        return false;
    /* Commands poll events; the port may have disconnected while waiting. */
    if (!dev->connected)
        return false;
    pipe->configured = true;
    return true;
}

static void backend_close(usb_device_t *device) {
    xhci_device_t *dev = device ? device->host_data : NULL;
    if (!dev)
        return;
    notify_disconnect(dev);
    if (!hc.failed && command(TRB_DISABLE_SLOT, 0, (uint32_t)dev->slot_id << 24, NULL)) {
        hc.dcbaa[dev->slot_id] = 0;
        dma_release();
        dev->allocated = false;
    }
    /* The existing early PMM supplies no free operation; retain all pages. */
}

static const usb_host_ops_t host_ops = {.control = backend_control,
                                        .set_ep0_packet_size = backend_set_ep0_packet_size,
                                        .configure_interrupt_in = backend_configure_interrupt_in,
                                        .submit_interrupt_in = backend_submit_interrupt_in,
                                        .close = backend_close};

static bool read_extended_capabilities(uint32_t hccparams1) {
    uint32_t offset = (hccparams1 >> 16) * 4u;
    unsigned traversed = 0;
    bool saw_protocol = false;
    while (offset) {
        if (++traversed > 256 || offset > XHCI_MAX_MMIO - 16u) {
            kprintf("xHCI: invalid extended capability chain\n\r");
            return false;
        }
        if ((size_t)offset + 16u > hc.mmio_size) {
            size_t needed =
                ((size_t)offset + 16u + XHCI_PAGE_SIZE - 1u) & ~(size_t)(XHCI_PAGE_SIZE - 1u);
            void *expanded = usb_platform_map_mmio(hc.mmio_phys, needed);
            if (!expanded)
                return false;
            hc.mmio = expanded;
            hc.mmio_size = needed;
        }
        volatile uint32_t *ext = (volatile uint32_t *)(hc.mmio + offset);
        uint32_t header = ext[0];
        uint8_t id = (uint8_t)header;
        uint8_t next = (uint8_t)(header >> 8);
        if (id == 1) {
            /* Byte write leaves the adjacent BIOS ownership byte untouched. */
            hc.mmio[offset + 3] |= 1u;
            if (!wait_bits(ext, (1u << 16) | (1u << 24), 1u << 24, XHCI_TIMEOUT_MS)) {
                kprintf("xHCI: BIOS ownership handoff timed out\n\r");
                return false;
            }
            uint32_t legacy = ext[1];
            uint32_t preserved = legacy & ((7u << 1) | (0xffu << 5));
            ext[1] = preserved | (7u << 29); /* Disable SMIs; clear RW1C status. */
        } else if (id == 2) {
            saw_protocol = true;
            uint8_t first = (uint8_t)ext[2];
            uint8_t count = (uint8_t)(ext[2] >> 8);
            uint8_t psi_count = (uint8_t)((ext[2] >> 28) & 15u);
            bool usb2 = (header >> 24) == 2 && ext[1] == 0x20425355u;
            /* Custom Protocol Speed IDs need translation, left for later. */
            if (usb2 && psi_count != 0) {
                kprintf("xHCI: custom USB2 Protocol Speed IDs unsupported\n\r");
                usb2 = false;
            }
            if (first == 0 || (unsigned)first + count > (unsigned)hc.ports + 1u)
                return false;
            for (unsigned p = first; p < (unsigned)first + count; ++p) {
                hc.usb2_port[p] = usb2;
                hc.slot_type[p] = (uint8_t)(ext[3] & 31u);
            }
        }
        if (!next)
            break;
        offset += (uint32_t)next * 4u;
    }
    if (!saw_protocol) {
        kprintf("xHCI: no Supported Protocol capability\n\r");
        return false;
    }
    return true;
}

static bool reset_usb2_port(uint8_t port, uint8_t *speed) {
    volatile uint32_t *reg = port_reg(port);
    if (!(*reg & PORT_POWER)) {
        *reg = (*reg & PORT_PRESERVE) | PORT_POWER;
        if (!usb_platform_delay_ms(20))
            return false;
    }
    if (!(*reg & PORT_CCS))
        return false;
    if (!usb_platform_delay_ms(100)) /* Connection debounce. */
        return false;
    if (!(*reg & PORT_CCS))
        return false;
    acknowledge_port(port);
    *reg = (*reg & PORT_PRESERVE) | PORT_RESET;
    if (!wait_bits(reg, PORT_RESET, 0, XHCI_TIMEOUT_MS) ||
        !wait_bits(reg, PORT_PRC, PORT_PRC, XHCI_TIMEOUT_MS)) {
        kprintf("xHCI port %u: reset timeout\n\r", port);
        return false;
    }
    acknowledge_port(port);
    if (!usb_platform_delay_ms(10)) /* USB2 reset recovery before requests. */
        return false;
    uint32_t value = *reg;
    if ((value & (PORT_CCS | PORT_PED)) != (PORT_CCS | PORT_PED))
        return false;
    *speed = (uint8_t)((value >> 10) & 15u);
    if (*speed < 1 || *speed > 3) {
        kprintf("xHCI port %u: unsupported speed ID %u\n\r", port, *speed);
        return false;
    }
    /* Drain reset/connect events before binding this port to a new slot. */
    poll_events();
    return !hc.failed;
}

bool xhci_init_controller(pci_device_header_t *pci_hdr) {
    if (hc.initialized) {
        kprintf("xHCI: initialization already attempted; one controller supported\n\r");
        return false;
    }
    hc.initialized = true;
    if (!pci_hdr || !usb_platform_dma_ready()) {
        kprintf("xHCI: missing PCI header or shared kernel HHDM response\n\r");
        return false;
    }
    if (!usb_platform_delay_ms(0)) {
        kprintf("xHCI: ACPI PM timer unavailable or not advancing\n\r");
        return false;
    }
    if (!usb_platform_prepare_pci(pci_hdr)) {
        kprintf("xHCI: implement usb_platform_prepare_pci() first\n\r");
        return false;
    }
    uint32_t bar0 = pci_hdr->bar[0];
    uint32_t bar_type = (bar0 >> 1) & 3u;
    if ((bar0 & 1u) || (bar_type != 0 && bar_type != 2)) {
        kprintf("xHCI: BAR0 is not a supported memory BAR\n\r");
        return false;
    }
    uint64_t physical = bar0 & ~0xfull;
    if (bar_type == 2)
        physical |= (uint64_t)pci_hdr->bar[1] << 32;
    if (!physical)
        return false;

    volatile xhci_cap_regs_t *cap = usb_platform_map_mmio(physical, XHCI_PAGE_SIZE);
    if (!cap) {
        kprintf("xHCI: implement usb_platform_map_mmio() first\n\r");
        return false;
    }

    uint8_t caplength = cap->caplength;
    uint32_t hcs1 = cap->hcsparams1;
    uint32_t hcs2 = cap->hcsparams2;
    uint32_t hcc1 = cap->hccparams1;
    uint32_t dboff = cap->dboff & ~3u;
    uint32_t rtsoff = cap->rtsoff & ~31u;
    uint32_t max_slots = hcs1 & 0xffu;
    hc.ports = (uint8_t)(hcs1 >> 24);
    hc.slots = (uint8_t)(max_slots < XHCI_MAX_SLOTS ? max_slots : XHCI_MAX_SLOTS);
    hc.ac64 = (hcc1 & 1u) != 0;
    hc.context_stride = (hcc1 & (1u << 2)) ? 64 : 32;
    if (caplength < 0x20 || (caplength & 7u) || !hc.slots || !hc.ports ||
        ((hcs1 >> 8) & 0x7ffu) == 0 || dboff < 0x20 || rtsoff < 0x20 ||
        dboff > XHCI_MAX_MMIO - 0x1000u || rtsoff > XHCI_MAX_MMIO - 0x1000u) {
        kprintf("xHCI: invalid controller capabilities\n\r");
        return false;
    }

    /* Map only the required register prefix, not an assumed 64/256KiB BAR.
     * Extend the mapping while walking extended capabilities if needed.
     */
    size_t bytes = XHCI_PAGE_SIZE;
    size_t end = (size_t)caplength + 0x400u + (size_t)hc.ports * 16u;
    if (end > bytes)
        bytes = end;
    end = (size_t)dboff + ((size_t)hc.slots + 1u) * 4u;
    if (end > bytes)
        bytes = end;
    end = (size_t)rtsoff + 0x40u;
    if (end > bytes)
        bytes = end;
    bytes = (bytes + XHCI_PAGE_SIZE - 1u) & ~(size_t)(XHCI_PAGE_SIZE - 1u);
    hc.mmio = usb_platform_map_mmio(physical, bytes);
    if (!hc.mmio) {
        kprintf("xHCI: MMIO mapping failed\n\r");
        return false;
    }
    hc.mmio_size = bytes;
    hc.mmio_phys = physical;
    if (!read_extended_capabilities(hcc1))
        return false;

    hc.op = (volatile xhci_op_regs_t *)(hc.mmio + caplength);
    hc.intr = (volatile xhci_interrupter_regs_t *)(hc.mmio + rtsoff + 0x20u);
    hc.doorbells = (volatile uint32_t *)(hc.mmio + dboff);
    if (!wait_bits(&hc.op->usbsts, USBSTS_CNR, 0, XHCI_TIMEOUT_MS)) {
        fail_controller("controller not ready before reset");
        return false;
    }

    hc.op->usbcmd &= ~(USBCMD_RUN | USBCMD_INTE);
    if (!wait_bits(&hc.op->usbsts, USBSTS_HALTED, USBSTS_HALTED, XHCI_TIMEOUT_MS)) {
        fail_controller("halt timeout");
        return false;
    }
    hc.op->usbcmd |= USBCMD_RESET;
    if (!wait_bits(&hc.op->usbcmd, USBCMD_RESET, 0, XHCI_TIMEOUT_MS) ||
        !wait_bits(&hc.op->usbsts, USBSTS_CNR, 0, XHCI_TIMEOUT_MS)) {
        fail_controller("reset timeout");
        return false;
    }

    if (!(hc.op->pagesize & 1u)) {
        fail_controller("controller does not support 4KiB pages");
        return false;
    }

    void *memory;
    uint64_t dcbaa_phys = alloc_dma(1, &memory);
    hc.dcbaa = memory;
    if (!dcbaa_phys)
        goto allocation_failed;

    uint32_t scratchpads = (((hcs2 >> 21) & 31u) << 5) | ((hcs2 >> 27) & 31u);
    if (scratchpads) {
        size_t pages = ((size_t)scratchpads * 8u + XHCI_PAGE_SIZE - 1u) / XHCI_PAGE_SIZE;
        uint64_t array_phys = alloc_dma(pages, &memory);
        uint64_t *array = memory;
        if (!array_phys)
            goto allocation_failed;
        for (unsigned i = 0; i < scratchpads; ++i) {
            uint64_t scratch_phys = alloc_dma(1, &memory);
            if (!scratch_phys)
                goto allocation_failed;
            array[i] = scratch_phys;
        }
        hc.dcbaa[0] = array_phys;
    }

    if (!ring_init(&hc.command))
        goto allocation_failed;
    hc.events_phys = alloc_dma(1, &memory);
    hc.events = memory;
    if (!hc.events_phys)
        goto allocation_failed;

    uint64_t erst_phys = alloc_dma(1, &memory);
    xhci_erst_entry_t *erst = memory;
    if (!erst_phys)
        goto allocation_failed;
    erst[0].ring_segment_base_address = hc.events_phys;
    erst[0].ring_segment_size = XHCI_RING_TRBS;
    hc.event_cycle = 1;
    dma_release();

    hc.op->config = (hc.op->config & ~0xffu) | hc.slots;
    hc.op->dcbaap = dcbaa_phys;
    hc.op->crcr = hc.command.phys | 1u;
    hc.intr->iman = 1u; /* Acknowledge IP and keep IE=0. */
    hc.intr->imod = 0;
    hc.intr->erstsz = 1;
    hc.intr->erstba = erst_phys;
    hc.intr->erdp = hc.events_phys | (1u << 3);

    hc.op->usbcmd = USBCMD_RUN; /* Polling: controller interrupt enable stays 0. */
    if (!wait_bits(&hc.op->usbsts, USBSTS_HALTED, 0, XHCI_TIMEOUT_MS)) {
        fail_controller("start timeout");
        return false;
    }

    hc.running = true;
    kprintf("xHCI running: %u ports, context stride %u bytes\n\r", hc.ports, hc.context_stride);
    return true;

allocation_failed:
    fail_controller("DMA allocation failed");
    return false;
}

usb_device_t *xhci_open_root_device(uint8_t port) {
    if (!xhci_is_running() || !port || port > hc.ports || !hc.usb2_port[port])
        return NULL;
    uint8_t speed;
    if (!reset_usb2_port(port, &speed))
        return NULL;
    uint8_t slot_id = 0;
    if (!command(TRB_ENABLE_SLOT, 0, (uint32_t)hc.slot_type[port] << 16, &slot_id))
        return NULL;
    if (!slot_id || slot_id > hc.slots) {
        fail_controller("Enable Slot returned an invalid slot ID");
        return NULL;
    }
    xhci_device_t *dev = &hc.devices[slot_id];
    memset(dev, 0, sizeof(*dev));
    dev->allocated = true;
    dev->connected = true;
    dev->slot_id = slot_id;
    dev->port = port;
    dev->speed = speed;
    dev->ep0_mps = speed == 3 ? 64 : 8;
    dev->usb = (usb_device_t){.id = slot_id,
                              .root_port = port,
                              .speed = speed,
                              .ep0_max_packet_size = dev->ep0_mps,
                              .connected = true,
                              .host_ops = &host_ops,
                              .host_data = dev};

    void *memory;
    dev->input_phys = alloc_dma(1, &memory);
    dev->input = memory;
    if (!dev->input_phys)
        goto failed;
    uint64_t output_phys = alloc_dma(1, &memory);
    dev->output = memory;
    if (!output_phys)
        goto failed;
    dev->buffer_phys = alloc_dma(1, &memory);
    dev->buffer = memory;
    if (!dev->buffer_phys || !ring_init(&dev->ep0))
        goto failed;

    input_control(dev)->add_flags = 3;
    input_slot(dev)->info1 = ((uint32_t)speed << 20) | (1u << 27);
    input_slot(dev)->info2 = (uint32_t)port << 16;
    xhci_endpoint_context_t *ep0 = input_endpoint(dev, 1);
    ep0->info2 = (3u << 1) | (4u << 3) | ((uint32_t)dev->ep0_mps << 16);
    ep0->tr_dequeue_ptr = dev->ep0.phys | 1u;
    ep0->tx_info = 8; /* Average TRB Length is the LOW 16 bits. */
    hc.dcbaa[slot_id] = output_phys;
    if (!command(TRB_ADDRESS_DEVICE, dev->input_phys, (uint32_t)slot_id << 24, NULL))
        goto failed;               /* BSR=0: xHC sends SET_ADDRESS. Never send it on EP0. */
    if (!usb_platform_delay_ms(2)) /* SET_ADDRESS recovery. */
        goto failed;

    if (!dev->connected)
        goto failed;
    return &dev->usb;
failed:
    backend_close(&dev->usb);
    return NULL;
}

void xhci_poll(void) {
    if (hc.running && !hc.failed)
        poll_events();
}

bool xhci_is_running(void) {
    return hc.running && !hc.failed;
}

uint8_t xhci_root_port_count(void) {
    return hc.ports;
}

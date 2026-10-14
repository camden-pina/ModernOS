#ifndef XHCI_STRUCTS_H
#define XHCI_STRUCTS_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t caplength;
    uint8_t reserved;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hccparams2;
} xhci_cap_regs_t;

typedef struct {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint8_t reserved1[8];
    uint32_t dnctrl;
    uint64_t crcr;
    uint8_t reserved2[16];
    uint64_t dcbaap;
    uint32_t config;
} xhci_op_regs_t;

typedef struct {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t reserved;
    uint64_t erstba;
    uint64_t erdp;
} xhci_interrupter_regs_t;

_Static_assert(offsetof(xhci_op_regs_t, crcr) == 0x18, "CRCR offset");
_Static_assert(offsetof(xhci_op_regs_t, dcbaap) == 0x30, "DCBAAP offset");
_Static_assert(offsetof(xhci_op_regs_t, config) == 0x38, "CONFIG offset");
_Static_assert(sizeof(xhci_interrupter_regs_t) == 0x20, "Interrupter size");

#define TRB_NORMAL 1u
#define TRB_SETUP_STAGE 2u
#define TRB_DATA_STAGE 3u
#define TRB_STATUS_STAGE 4u
#define TRB_LINK 6u
#define TRB_ENABLE_SLOT 9u
#define TRB_DISABLE_SLOT 10u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_CONFIGURE_ENDPOINT 12u
#define TRB_EVALUATE_CONTEXT 13u
#define TRB_RESET_ENDPOINT 14u
#define TRB_SET_TR_DEQUEUE_POINTER 16u
#define TRB_TRANSFER_EVENT 32u
#define TRB_COMMAND_COMPLETION_EVENT 33u
#define TRB_PORT_STATUS_CHANGE_EVENT 34u
#define TRB_HOST_CONTROLLER_EVENT 37u

#define XHCI_CC_SUCCESS 1u
#define XHCI_CC_STALL_ERROR 6u
#define XHCI_CC_SHORT_PACKET 13u
#define XHCI_TRB_CYCLE (1u << 0)
#define XHCI_TRB_TOGGLE_CYCLE (1u << 1)
#define XHCI_TRB_ISP (1u << 2)
#define XHCI_TRB_IOC (1u << 5)
#define XHCI_TRB_IDT (1u << 6)
#define XHCI_TRB_DIR_IN (1u << 16)
#define XHCI_TRB_TYPE(t) ((uint32_t)(t) << 10)

/* Natural alignment gives the exact hardware layout on x86-64. */
typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct {
    uint64_t ring_segment_base_address;
    uint32_t ring_segment_size; /* Bits 15:0 are size; bits 31:16 reserved. */
    uint32_t reserved;
} xhci_erst_entry_t;

/* These are the first 32 bytes of a context. When CSZ=1, stride is 64. */
typedef struct {
    uint32_t info1;
    uint32_t info2;
    uint32_t tt_info;
    uint32_t state;
    uint32_t reserved[4];
} xhci_slot_context_t;

typedef struct {
    uint32_t info1; /* DWORD 0: state, Mult, Interval, Max ESIT Payload Hi. */
    uint32_t info2; /* DWORD 1: CErr, EP Type, Max Burst, Max Packet Size. */
    uint64_t tr_dequeue_ptr;
    uint32_t tx_info; /* Average TRB Length low 16; Max ESIT Payload low high 16. */
    uint32_t reserved[3];
} xhci_endpoint_context_t;

typedef struct {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} xhci_input_control_context_t;

/* No fixed aggregate Input Context: use the controller's 32/64-byte stride. */
_Static_assert(sizeof(xhci_trb_t) == 16, "TRB size");
_Static_assert(sizeof(xhci_erst_entry_t) == 16, "ERST size");
_Static_assert(sizeof(xhci_slot_context_t) == 32, "Slot Context size");
_Static_assert(sizeof(xhci_endpoint_context_t) == 32, "Endpoint Context size");
_Static_assert(offsetof(xhci_endpoint_context_t, tr_dequeue_ptr) == 8,
               "Endpoint dequeue pointer offset");
_Static_assert(sizeof(xhci_input_control_context_t) == 32, "Input Control size");

#endif

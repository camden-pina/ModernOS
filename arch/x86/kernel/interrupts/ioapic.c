#define MODULE "ioapic"

#include <interrupts/ioapic.h>
#include <interrupts/lapic.h>
#include <acpi/tables.h>
#include <printk.h>
#include <panic.h>

#define IOAPICID 0x00
#define IOAPICVER 0x01
#define IOAPICARB 0x02
#define IOREDTBL 0x10

typedef struct acpi_madt_entry_t {
    unsigned char type;
    unsigned char length;
} __attribute__((packed)) acpi_madt_entry_t;

typedef struct apic_local_t {
    acpi_madt_entry_t hdr;
    unsigned char processorID;
    unsigned char apicID;
    unsigned int flags;
} __attribute__((packed)) apic_local_t;

typedef struct apic_io_t {
    acpi_madt_entry_t hdr;
    unsigned char ID;
    unsigned char reserved;
    unsigned int address;
    unsigned int globalSystemInterruptBase;
} __attribute__((packed)) apic_io_t;

typedef struct apic_interrupt_service_override_t {
    acpi_madt_entry_t hdr;
    unsigned char busSource;
    unsigned char irqSource;
    unsigned int globalSystemInterrupt;
    unsigned short flags;
} __attribute__((packed)) apic_interrupt_service_override_t;

typedef struct apic_non_maskable_interrupt_t {
    acpi_madt_entry_t hdr;
    unsigned char processorID;
    unsigned short flags;
    unsigned char lint_num;
} __attribute__((packed)) apic_non_maskable_interrupt_t;

typedef struct apic_local_address_override_t {
    acpi_madt_entry_t hdr;
    unsigned short reserved;
    unsigned long long address;
} __attribute__((packed)) apic_local_address_override_t;

// These get set from the ACPI table
static unsigned int *IOREGSEL = (void *)0;
static unsigned int *IOREGWIN = (void *)0;

static void ioapic_write(unsigned int index, unsigned int value) {
    *IOREGSEL = index;
    *IOREGWIN = value;
}

static unsigned int ioapic_read(unsigned int index) {
    *IOREGSEL = index;
    return *IOREGWIN;
}

#define IOAPIC_DELIVERY_MODE_FIXED 0x00
#define IOAPIC_DELIVERY_MODE_LOWEST_PRIOTIRY 0x01
#define IOAPIC_DELIVERY_MODE_SMI 0x02
#define IOAPIC_DELIVERY_MODE_NMI 0x04
#define IOAPIC_DELIVERY_MODE_INIT 0x05
#define IOAPIC_DELIVERY_MODE_STARTUP 0x06

#define IOAPIC_DESTINATION_PHYSICAL 0x00
#define IOAPIC_DESTINATION_LOGICAL 0x01

#define IOAPIC_DELIVERY_STATUS_IDLE 0x00
#define IOAPIC_DELIVERY_STATUS_SEND_PENDING 0x01

#define IOAPIC_LEVEL_DEASSERT 0x00
#define IOAPIC_LEVEL_ASSERT 0x01

#define IOAPIC_TRIGGER_EDGE 0x00
#define IOAPIC_TRIGGER_LEVEL 0x01

#define IOAPIC_DESTINATION_SHORTHAND_NO_SHORTHAND 0x00
#define IOAPIC_DESTINATION_SHORTHAND_SELF 0x01
#define IOAPIC_DESTINATION_SHORTHAND_ALL_INCLUDING_SELF 0x02
#define IOAPIC_DESTINATION_SHORTHAND_ALL_EXCLUDING_SELF 0x03

typedef struct ICR {
    unsigned char vector;
    unsigned char delivery_mode : 3;
    unsigned char destination_mode : 1;
    unsigned char deleivery_status : 1;
    unsigned char polarity : 1; //was reserved
    unsigned char level : 1;
    unsigned char trigger_mode : 1;
    unsigned char mask : 1; // was reserved
    unsigned char reserved1 : 1;
    unsigned char destination_shorthand : 2;
    unsigned long long reserved2 : 36;
    unsigned char destination_field;
} __attribute__((packed)) ICR;

void ioapic_map(unsigned char irq_index, unsigned char idt_index) {
    const unsigned int low_index = IOREDTBL + irq_index * 2;
    const unsigned int high_index = IOREDTBL + irq_index * 2 + 1;

    unsigned int high = ioapic_read(high_index);

    // Set APIC ID
    high &= ~0xff000000;

    high |= apic_read(0x02) << 24; // Local APIC id

    ioapic_write(high_index, high);

    unsigned int low = ioapic_read(low_index);

    // unmask the IRQ
    low &= ~(1 << 16);

    // set to physical delivery mode
    low &= ~(1 << 11);

    // set to fixed delivery mode
    low &= ~0x700;

    // set delivery vector
    low &= ~0xff;
    low |= idt_index;

    ioapic_write(low_index, low);
}

// Locates the I/O APIC with an IRQ Base of 0x00 and sets IOREGSEL and IOREGWIN values accordingly
_Bool load_ioapic_address(void) {
    // Get MADT Table
    const acpi_madt_t *madt = (acpi_madt_t *)acpi_locate_table("APIC");

    unsigned long long cpu_count = 0;
    char found = 0;

    for (unsigned long long i = sizeof(acpi_madt_t); i < madt->hdr.len;) {
        acpi_madt_entry_t *madt_entry = (acpi_madt_entry_t *)((char *)madt + i);

        switch (madt_entry->type) {
        case 0x00: // Local APIC
        {
            apic_local_t *cpu = (apic_local_t *)madt_entry;

            kprintf("CPU local APIC ID 0x%02x ACPI ID 0x%02x flags 0x%08x\n", cpu->apicID,
                    cpu->processorID, cpu->flags);
            if (cpu->flags & 0x01)
                cpu_count++;

            break;
        }
        case 0x01: // I/O APIC
        {
            found = 1;
            apic_io_t *apic_io = (apic_io_t *)((char *)madt_entry);

            if (apic_io->globalSystemInterruptBase == 0x00) {
                IOREGSEL = (unsigned int *)(long long)apic_io->address;
                IOREGWIN = (unsigned int *)(long long)(apic_io->address + 0x10);
            }
            break;
        }
        case 0x02: // Interrupt Source Override
        {
            break;
        }
        case 0x03: // NMI Source
        {
            break;
        }
        case 0x04: // Local APIC NMI
        {
            break;
        }
        case 0x05: // Local APIC Address Override
        {
            //apic_local_address_override_t* base = (apic_local_address_override_t*)((char*)madt_entry);
            break;
        }
        case 0x06: // I/O SAPIC
        {
            break;
        }
        case 0x07: // Local SAPIC
        {
            break;
        }
        case 0x08: // Platform Interrupt Sources
        {
            break;
        }
        case 0x09: // Processor Local x2APIC
        {
            break;
        }
        case 0x0A: // Local x2APIC NMI
        {
            break;
        }
        case 0x0B: // GIC
        {
            break;
        }
        case 0x0C: // GICD
        {
            break;
        }
        default: {
            kprintf("Unrecognized Entry in MADT : type 0%x\n\r", madt_entry->type);
            break;
        }
        }
        i += madt_entry->length;
    }

    kprintf("CPU Count: %llu\n\r", cpu_count);
    return found;
}

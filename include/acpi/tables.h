#ifndef _ACPI_TABLES_H
#define _ACPI_TABLES_H 1

#include <acpi/acpi.h>

// RSDP Structure
typedef struct _acpi_rsdp_t {
    unsigned char signature[8];
    unsigned char checksum;
    char OEM[6];
    unsigned char rev;
    unsigned int rsdt_ptr;
    unsigned int length;
    unsigned long long xsdt_ptr;
    unsigned char exChecksum;
    char reserved[3];
} __attribute__((packed)) _acpi_rsdp_t;

// ACPI System Description Table Header
typedef struct _acpi_sdt_hdr_t {
    char signature[4];
    unsigned int len;
    unsigned char rev;
    unsigned char checksum;
    char oem[6];
    char oem_table_id[8];
    unsigned int oem_rev;
    unsigned int creator_id;
    unsigned int creator_rev;
} __attribute__((packed)) _acpi_sdt_hdr_t;

// Extended System Description Table (XSDT)
typedef struct _acpi_xsdt_t {
    _acpi_sdt_hdr_t hdr;
    unsigned long long tables[]; // Array of 64-bit pointers
} __attribute__((packed)) _acpi_xsdt_t;

typedef struct {
    _acpi_sdt_hdr_t hdr;
    unsigned int local_interrupt_controller_addr;
    unsigned int flags;
} __attribute__((packed)) acpi_madt_t;

// Generic Address Structure (GAS)
typedef struct _acpi_gas_t {
    unsigned char addr_space;
    unsigned char bit_width;
    unsigned char bit_offset;
    unsigned char access_size;
    unsigned long long addr;
} __attribute__((packed)) _acpi_gas_t;

// Fixed ACPI Description Table (FADT)
typedef struct _acpi_fadt_t {
    _acpi_sdt_hdr_t hdr;
    unsigned int firmware_ctl;
    unsigned int dsdt;
    unsigned char reserved0;
    unsigned char pref_pm_profile;
    unsigned short sci_int;
    unsigned int smi_cmd;
    unsigned char acpi_enable;
    unsigned char acpi_disable;
    unsigned char s4bios_req;
    unsigned char pstate_cnt;
    unsigned int pm1a_evt_block;
    unsigned int pm1b_evt_blk;
    unsigned int pm1a_cnt_blk;
    unsigned int pm1b_cnt_blk;
    unsigned int pm2_cnt_blk;
    unsigned int pm_tmr_blk;
    unsigned int gpe0_blk;
    unsigned int gpe1_blk;
    unsigned char pm1_evt_len;
    unsigned char pm1_cnt_len;
    unsigned char pm2_cnt_len;
    unsigned char pm_tme_len;
    unsigned char gpe0_blk_len;
    unsigned char gpe1_blk_len;
    unsigned char gpe1_base;
    unsigned char cst_cnt;
    unsigned short p_lvl2_lat;
    unsigned short p_lvl3_lat;
    unsigned short flush_size;
    unsigned short flush_stride;
    unsigned char duty_offset;
    unsigned char duty_width;
    unsigned char day_alrm;
    unsigned char mon_alrm;
    unsigned char century;
    unsigned short iapc_boot_arch;
    unsigned char reserved1;
    unsigned int flags;
    _acpi_gas_t reset_reg;
    unsigned char reset_value;
    unsigned short arm_boot_arch;
    unsigned char fadt_minor_version;
    unsigned long long x_firmware_ctrl;
    unsigned long long x_dsdt;
    _acpi_gas_t x_pm1a_evt_blk;
    _acpi_gas_t x_pm1b_evt_blk;
    _acpi_gas_t x_pm1a_cnt_blk;
    _acpi_gas_t x_pm1b_cnt_blk;
    _acpi_gas_t x_pm2_cnt_blk;
    _acpi_gas_t x_pm_tmr_blk;
    _acpi_gas_t x_gpe0_blk;
    _acpi_gas_t x_gpe1_blk;
    _acpi_gas_t sleep_control_reg;
    _acpi_gas_t sleep_status_reg;
    unsigned long long hypervisor_vendor_identity;
} __attribute__((packed)) _acpi_fadt_t;

// Function declarations for table lookup.
acpi_status_t acpi_table_find(void **table, const char *signature, size_t idx);
void *acpi_scan(const char *name, size_t idx);

#endif

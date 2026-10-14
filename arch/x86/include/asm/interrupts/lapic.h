#ifndef _LAPIC_H
#define _LAPIC_H 1

void apic_init(void);
void apic_init_cpu(void);
void apic_timer_start(void);
void apic_send_eoi_if_necessary(unsigned char interrupt_vector);

void apic_write(int index, unsigned int value);
unsigned int apic_read(int index);

#endif // LAPIC_H

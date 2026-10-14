#ifndef MODERNOS_FILE_TIME_H
#define MODERNOS_FILE_TIME_H
#include <stdint.h>
#include <stdbool.h>

int64_t fs_time_now(void);
int64_t fs_time_from_fat(uint16_t date, uint16_t time);
bool fs_time_to_fat(int64_t epoch, uint16_t *date, uint16_t *time);
#endif

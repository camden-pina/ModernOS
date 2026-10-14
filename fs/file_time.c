#include <fs/file_time.h>
#include <tsc.h>

int64_t fs_time_now(void) {
    extern int64_t kernel_boot_epoch;
    return kernel_boot_epoch ? kernel_boot_epoch + (int64_t)(tsc_uptime_us() / 1000000) : 0;
}

static bool leap(unsigned year) {
    return !(year % 4) && ((year % 100) || !(year % 400));
}

static unsigned month_days(unsigned year, unsigned month) {
    static const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return days[month - 1] + (month == 2 && leap(year));
}

int64_t fs_time_from_fat(uint16_t date, uint16_t time) {
    unsigned year = 1980 + (date >> 9), month = (date >> 5) & 15, day = date & 31;
    unsigned hour = time >> 11, minute = (time >> 5) & 63, second = (time & 31) * 2;
    if (!month || month > 12 || !day || day > month_days(year, month) || hour > 23 || minute > 59 ||
        second > 59)
        return 0;
    int64_t days = 0;
    for (unsigned y = 1970; y < year; y++)
        days += 365 + leap(y);
    for (unsigned m = 1; m < month; m++)
        days += month_days(year, m);
    return (days + day - 1) * 86400 + hour * 3600 + minute * 60 + second;
}

bool fs_time_to_fat(int64_t epoch, uint16_t *date, uint16_t *time) {
    if (epoch < 315532800 || epoch >= 4354819200LL)
        return false; /* 1980..2107 */
    unsigned year = 1970, month = 1;
    int64_t days = epoch / 86400;
    while (days >= 365 + leap(year)) {
        days -= 365 + leap(year);
        year++;
    }
    while (days >= month_days(year, month)) {
        days -= month_days(year, month);
        month++;
    }
    unsigned seconds = epoch % 86400;
    *date = ((year - 1980) << 9) | (month << 5) | (days + 1);
    *time = ((seconds / 3600) << 11) | (((seconds / 60) % 60) << 5) | ((seconds % 60) / 2);
    return true;
}

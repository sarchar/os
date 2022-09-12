#ifndef __CMOS_H__
#define __CMOS_H__


// see map at http://www.bioscentral.com/misc/cmosmap.htm
//
enum CMOS_REGISTERS {
    CMOS_REGISTER_RTC_SECONDS     = 0x00,
    CMOS_REGISTER_RTC_MINUTES     = 0x02,
    CMOS_REGISTER_RTC_HOURS       = 0x04,
    CMOS_REGISTER_RTC_DAY_OF_WEEK = 0x06,
    CMOS_REGISTER_RTC_DATE_DAY    = 0x07,
    CMOS_REGISTER_RTC_DATE_MONTH  = 0x08,
    CMOS_REGISTER_RTC_DATE_YEAR   = 0x09,
    CMOS_REGISTER_STATUS_A        = 0x0A,
    CMOS_REGISTER_STATUS_B        = 0x0B,
    CMOS_REGISTER_STATUS_C        = 0x0C,
    CMOS_REGISTER_STATUS_D        = 0x0D
};

enum CMOS_STATUS_A_FLAGS {
    CMOS_STATUS_A_FLAG_RTC_UPDATE_IN_PROGRESS = 0x80,
};

enum CMOS_STATUS_B_FLAGS {
    CMOS_STATUS_B_FLAG_RTC_DST                       = 1 << 0, // daylight savings
    CMOS_STATUS_B_FLAG_RTC_24HOUR                    = 1 << 1,
    CMOS_STATUS_B_FLAG_RTC_NOT_BCD                   = 1 << 2,
    CMOS_STATUS_B_FLAG_UPDATE_ENDED_INTERRUPT_ENABLE = 1 << 4,
    CMOS_STATUS_B_FLAG_ALARM_INTERRUPT_ENABLE        = 1 << 5,
    CMOS_STATUS_B_FLAG_PERIODIC_INTERRUPT_ENABLE     = 1 << 6,
};

// Even though these flags are passed into cmos_time, the
// actual values in the cmos_time structure are all converted to 24-hours, non-bcd encoded ints
enum CMOS_TIME_FLAGS {
    CMOS_TIME_24HOUR = 1 << 0, // CMOS time was 24 hours
    CMOS_TIME_DST    = 1 << 1, // daylight savings was enabled in CMOS
    CMOS_TIME_BCD    = 1 << 2, // COMS time was BCD encoded
};

struct cmos_time {
    u8 seconds;
    u8 minutes;
    u8 hours;
    u8 day;
    u8 month;
    u8 year;
    u8 century;
    u8 flags;
} __packed;

void cmos_init();
void cmos_read_rtc(struct cmos_time*);

#endif

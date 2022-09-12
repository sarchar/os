#include "common.h"

#include "acpi.h"
#include "cmos.h"
#include "cpu.h"
#include "smp.h"
#include "stdio.h"
#include "string.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

#define NMI_ENABLE   0x00
#define NMI_DISABLE  0x80

#define SPIN_WAIT_COUNT 3

#define RTC_UPDATE_IN_PROGRESS() ((_read_cmos(CMOS_REGISTER_STATUS_A) & CMOS_STATUS_A_FLAG_RTC_UPDATE_IN_PROGRESS) != 0)

struct {
    u8 nmi_flag;
    u8 century_register;
    u8 rtc_flags;
} global_cmos = {
    .nmi_flag         = NMI_DISABLE,
    .century_register = 0,
    .rtc_flags        = 0,
};

declare_spinlock(cmos_lock);

static __always_inline u8 _read_cmos(u8 reg)
{
    __outb(CMOS_ADDRESS, (reg & 0x7F) | global_cmos.nmi_flag);
    for(u32 i = 0; i < SPIN_WAIT_COUNT; i++) { __pause(); }
    return __inb(CMOS_DATA);
}

static __always_inline void _write_cmos(u8 reg, u8 val)
{
    __outb(CMOS_ADDRESS, (reg & 0x7F) | global_cmos.nmi_flag);
    for(u32 i = 0; i < SPIN_WAIT_COUNT; i++) { __pause(); }
    __outb(CMOS_DATA, val);
}

void cmos_init()
{
    // setup the century register
    struct acpi_fadt* fadt = acpi_find_table("FACP", 0);
    if(fadt != null) {
        global_cmos.century_register = fadt->century;
        fprintf(stderr, "cmos: century register = 0x%02X\n", global_cmos.century_register);
    }

    // cache rtc flags here
    u8 cmos_rtc_status = _read_cmos(CMOS_REGISTER_STATUS_B);
    if((cmos_rtc_status & CMOS_STATUS_B_FLAG_RTC_DST) != 0)     global_cmos.rtc_flags |= CMOS_TIME_DST;
    if((cmos_rtc_status & CMOS_STATUS_B_FLAG_RTC_24HOUR) != 0)  global_cmos.rtc_flags |= CMOS_TIME_24HOUR;
    if((cmos_rtc_status & CMOS_STATUS_B_FLAG_RTC_NOT_BCD) == 0) global_cmos.rtc_flags |= CMOS_TIME_BCD;

    // display current time during boot
    struct cmos_time t;
    cmos_read_rtc(&t);
    fprintf(stderr, "cmos: initialized. current date is %d:%02d:%02d %02d%02d-%02d-%02d\n",
            t.hours, t.minutes, t.seconds, t.century, t.year, t.month, t.day);
}

static void _cmos_read_rtc(struct cmos_time* time)
{
    // wait for any update in progress to complete
    while(RTC_UPDATE_IN_PROGRESS()) __pause();

    // read the clock
    time->seconds     = _read_cmos(CMOS_REGISTER_RTC_SECONDS);
    time->minutes     = _read_cmos(CMOS_REGISTER_RTC_MINUTES);
    time->hours       = _read_cmos(CMOS_REGISTER_RTC_HOURS);
    time->day         = _read_cmos(CMOS_REGISTER_RTC_DATE_DAY);
    time->month       = _read_cmos(CMOS_REGISTER_RTC_DATE_MONTH);
    time->year        = _read_cmos(CMOS_REGISTER_RTC_DATE_YEAR);
    time->century     = _read_cmos(global_cmos.century_register);
    time->flags       = global_cmos.rtc_flags;
}

#define BCD_TO_INT(x) (((x) & 0x0F) + (((x) >> 4) * 10))
void _fix_time(struct cmos_time* time)
{
    if((time->flags & CMOS_TIME_BCD) != 0) {
        time->seconds = BCD_TO_INT(time->seconds);
        time->minutes = BCD_TO_INT(time->minutes);
        time->hours   = BCD_TO_INT(time->hours);
        time->day     = BCD_TO_INT(time->day);
        time->month   = BCD_TO_INT(time->month);
        time->year    = BCD_TO_INT(time->year);
        time->century = BCD_TO_INT(time->century);
    }

    if((time->flags & CMOS_TIME_24HOUR) == 0) {
        // 12 hour clock uses high bit for AM/PM
        if(time->hours & 0x80) {
            time->hours = ((time->hours & 0x7F) + 12) % 24;
        }
    }
}

void cmos_read_rtc(struct cmos_time* time)
{
    acquire_lock(cmos_lock);
    u64 cpu_flags = __cli_saveflags();

    struct cmos_time tmp;
    _cmos_read_rtc(&tmp);

    while(true) {
        // read a second time
        _cmos_read_rtc(time);

        // if the two reads are equal, we should have a valid times
        if(memcmp(&tmp, time, sizeof(struct cmos_time)) == 0) break;

        // the times were different, try again
        memcpy(&tmp, time, sizeof(struct cmos_time));
    }

    release_lock(cmos_lock);
    __restoreflags(cpu_flags);

    _fix_time(time);
}

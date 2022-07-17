// Very basic module that allows sending data over the PC serial port
//
// Documentation
// https://wiki.osdev.org/Serial_Ports
// http://synfare.com/JO59hw/hwdocs/serial/serial04.html
//
#include "common.h"

#include "cpu.h"
#include "kernel.h"
#include "serial.h"
#include "stdio.h"

#define SERIAL_DATA_REG(port)    ((port) + 0x00)
#define SERIAL_IRQEN_REG(port)   ((port) + 0x01)
#define SERIAL_DIVL_REG(port)    ((port) + 0x00) // only valid with DLAB=1
#define SERIAL_DIVH_REG(port)    ((port) + 0x01) // only valid with DLAB=1
#define SERIAL_IDFCNTL_REG(port) ((port) + 0x02)
#define SERIAL_LINCNTL_REG(port) ((port) + 0x03)
#define SERIAL_MDMCNTL_REG(port) ((port) + 0x04)
#define SERIAL_LINSTAT_REG(port) ((port) + 0x05)
#define SERIAL_MDMSTAT_REG(port) ((port) + 0x06)
#define SERIAL_SCRATCH_REG(port) ((port) + 0x07)

#define DLAB_ENABLE_BIT    (1 << 7)

// character size
#define CHARACTER_LENGTH_5 (0 << 0)
#define CHARACTER_LENGTH_6 (1 << 0)
#define CHARACTER_LENGTH_7 (2 << 0)
#define CHARACTER_LENGTH_8 (3 << 0)

// stop bits
#define STOP_BITS_1        (0 << 2)
#define STOP_BITS_1p5_2    (1 << 2)

// parity
#define PARITY_NONE        (0 << 3)
#define PARITY_ODD         (1 << 3)
#define PARITY_EVEN        (3 << 3)
#define PARITY_MARK        (5 << 3)
#define PARITY_SPACE       (7 << 3)

// IRQ enable bits
#define IRQEN_DATA_AVAILABLE (1 << 0)
#define IRQEN_XMIT_EMPTY     (1 << 1)
#define IRQEN_BREAKERROR     (1 << 2)
#define IRQEN_STATUS_CHANGE  (1 << 3)

#define ENABLE_CLEAR_RXTX    (1 << 0)
#define CLEAR_RX             (1 << 1)
#define CLEAR_TX             (1 << 2)

#define RDY_MODE_1           (1 << 3)

#define FIFO_LENGTH_1        (0 << 6)
#define FIFO_LENGTH_4        (1 << 6)
#define FIFO_LENGTH_8        (2 << 6)
#define FIFO_LENGTH_14       (3 << 6)

#define DTR_MARK             (0 << 0)
#define DTR_SPACE            (1 << 0)
#define RTS_MARK             (0 << 1)
#define RTS_SPACE            (1 << 1)

#define IRQ_MASTER_ENABLE    (1 << 3)

#define LOOPBACK_ENABLE      (1 << 4)

#define RX_READY             (1 << 0)
#define OVERRUN_ERROR        (1 << 1)
#define PARITY_ERROR         (1 << 2)
#define FRAMING_ERROR        (1 << 3)
#define BREAK_CONDITION      (1 << 4)
#define TX_EMPTY             (1 << 5)
#define XMIT_IDLE            (1 << 6) // this and the next one are a little sketchy
#define IMPENDING_ERROR      (1 << 7)

static struct {
    u16  port;
    u8   divisor;
    bool valid;
} serial;

static inline bool _is_transmit_ready() 
{
    return (__inb(SERIAL_LINSTAT_REG(serial.port)) & TX_EMPTY) != 0;
}

static void _write(char a) 
{
    if(!serial.valid) return;

    while (!_is_transmit_ready()) ;

    __outb(SERIAL_DATA_REG(serial.port), a);
}

static bool _check_serial(u16 port)
{
    // Disable all interrupts
    __outb(SERIAL_IRQEN_REG(port), 0x00);

    // Enable DLAB (set baud rate divisor), so that we can set the divisor
    __outb(SERIAL_LINCNTL_REG(port), DLAB_ENABLE_BIT);

    // Set divisor to 3 (115200/3=) 38400 baud 
    __outb(SERIAL_DIVL_REG(port), 0x03); // high byte
    __outb(SERIAL_DIVH_REG(port), 0x00); // low byte

    // 8 bits, no parity, one stop bit
    __outb(SERIAL_LINCNTL_REG(port), STOP_BITS_1 | PARITY_NONE | CHARACTER_LENGTH_8); // clears DLAB

    // Enable 14-byte FIFO and clear rx/tx buffers
    __outb(SERIAL_IDFCNTL_REG(port), FIFO_LENGTH_14 | ENABLE_CLEAR_RXTX | CLEAR_RX | CLEAR_TX);

    // IRQs enabled, RTS/DSR set
    __outb(SERIAL_MDMCNTL_REG(port), IRQ_MASTER_ENABLE | RTS_SPACE | DTR_SPACE);

    // Set in loopback mode, test the serial chip
    __outb(SERIAL_MDMCNTL_REG(port), __inb(SERIAL_MDMCNTL_REG(port)) | LOOPBACK_ENABLE);

    // Test serial chip (send a byte and check if serial returns same byte)
    u8 test_byte = 0xAE;
    __outb(SERIAL_DATA_REG(port), test_byte);

    // Check if serial is faulty (i.e: not same byte as sent)
    if(__inb(SERIAL_DATA_REG(port)) != test_byte) return false;

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    __outb(SERIAL_MDMCNTL_REG(port), __inb(SERIAL_MDMCNTL_REG(port)) & ~LOOPBACK_ENABLE);    // Set in loopback mode, test the serial chip

    return true;
}

void serial_init()
{
    serial.port  = 0x3F8;
    serial.valid = _check_serial(serial.port);

    if(!serial.valid) fprintf(stderr, "serial: port 0x3F8 not valid\n");
    else              fprintf(stderr, "serial: initialized port 0x%x\n", serial.port);
}

void serial_write_buffer(char* buf, u16 len)
{
    while(len-- > 0) _write(*buf++);
}

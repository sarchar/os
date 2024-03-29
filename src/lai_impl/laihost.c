#include "lai/core.h"

#include "common.h"

#include "acpi.h"
#include "cpu.h"
#include "kernel.h"
#include "pci.h"
#include "stdio.h"
#include "stdlib.h"

// Logs a message. level can either be LAI_DEBUG_LOG for debugging info, or LAI_WARN_LOG for warnings 
void laihost_log(int level, const char *msg)
{
    unused(level);
    unused(msg);
//    fprintf(stderr, "acpi: lai(%d): %s\n", level, msg);
}

// Reports a fatal error, and halts.
__noreturn void laihost_panic(const char *msg)
{
    fprintf(stderr, "laihost_panic: %s\n", msg);
    PANIC(COLOR(32, 32, 32));
}

void* laihost_malloc(size_t sz)
{
    void* ptr = malloc(sz);
//    fprintf(stderr, "laihost_malloc(sz=%d)=0x%lX\n", sz, ptr);
    return ptr;
}

void* laihost_realloc(void* ptr, size_t newsz, size_t oldsz)
{
    unused(oldsz);
    void* newptr = realloc(ptr, newsz);
//    fprintf(stderr, "laihost_realloc(ptr=0x%lX, newsz=%d, oldsz=%d)=0x%lX\n", ptr, newsz, oldsz, newptr);
    return newptr;
}

void laihost_free(void* ptr, size_t sz)
{
    unused(sz);
//    fprintf(stderr, "laihost_free(ptr=0x%lX, sz=%d)\n", ptr, sz);
    free(ptr);
}

// Returns the (virtual) address of the n-th table that has the given signature, or NULL when no such table was found.
void* laihost_scan(char const* sig, size_t index)
{
    void* table = acpi_find_table(sig, index);
//    fprintf(stderr, "laihost_scan(sig=%s, index=%d)=0x%lX\n", sig, index, table);
    return table;
}

void laihost_outb(uint16_t port, uint8_t val)
{
    __outb(port, val);
}

void laihost_outw(uint16_t port, uint16_t val)
{
    __outw(port, val);
}

void laihost_outd(uint16_t port, uint32_t val)
{
    __outl(port, val);
}

uint8_t laihost_inb(uint16_t port)
{
    return __inb(port);
}

uint16_t laihost_inw(uint16_t port)
{
    return __inw(port);
}

uint32_t laihost_ind(uint16_t port)
{
    return __inl(port);
}

// Read a byte/word/dword from the given device's PCI configuration space at the given offset. 
uint8_t laihost_pci_readb(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset)
{
//    fprintf(stderr, "laihost_pci_readb(seg=%d, bus=%d, slot=%d, fun=%d, offset=%d)\n",
//            seg, bus, slot, fun, offset);
    assert(seg == 0, "for now seg must be 0");
    return pci_read_configuration_u8(bus, slot, fun, offset, null);
}

uint16_t laihost_pci_readw(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset)
{
//    fprintf(stderr, "laihost_pci_readw(seg=%d, bus=%d, slot=%d, fun=%d, offset=%d)\n",
//            seg, bus, slot, fun, offset);
    assert(seg == 0, "for now seg must be 0");
    return pci_read_configuration_u16(bus, slot, fun, offset, null);
}

uint32_t laihost_pci_readd(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun, uint16_t offset)
{
//    fprintf(stderr, "laihost_pci_readd(seg=%d, bus=%d, slot=%d, fun=%d, offset=%d)\n",
//            seg, bus, slot, fun, offset);
    assert(seg == 0, "for now seg must be 0");
    return pci_read_configuration_u32(bus, slot, fun, offset, null);
}

// Sleeps for the given amount of milliseconds. Can be stubbed on emulators 
void laihost_sleep(uint64_t ms)
{
//    fprintf(stderr, "laihost_sleep(ms=%ull)\n", ms);
//    for(u64 i = 0; i < ms*13241; i++) asm volatile("pause");
    usleep(ms*1000);
}

// Maps count bytes from the given physical address and returns a virtual address that can be used to access the memory.
void* laihost_map(size_t address, size_t count)
{
    unused(count);
//    fprintf(stderr, "laihost_map(address=0x%lX, count=%d)\n", address, count);
    return (void*)address;
}


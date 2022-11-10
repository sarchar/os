// Based on code from https://wiki.osdev.org/Bare_Bones
#include "lai/helpers/pm.h"

#include "common.h"

#include "acpi.h"
#include "apic.h"
#include "bootmem.h"
#include "buffer.h"
#include "cmos.h"
#include "cpu.h"
#include "drivers/ahci.h"
#include "drivers/e1000.h"
#include "drivers/ps2keyboard.h"
#include "efifb.h"
#include "fs/ext2/ext2.h"
#include "gdt.h"
#include "hpet.h"
#include "interrupts.h"
#include "kalloc.h"
#include "kernel.h"
#include "multiboot2.h"
#include "net/arp.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "net/icmp.h"
#include "net/ipv4.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/udp.h"
#include "paging.h"
#include "palloc.h"
#include "pci.h"
#include "serial.h"
#include "smp.h"
#include "stdio.h"
#include "task.h"
#include "terminal.h"
#include "time.h"
#include "vmem.h"

char const* assert_error_message = null;

extern void _gdt_fixup(intp vma_base);
void kernel_main(struct multiboot_info*);

static s64 echo_server(struct task*);
static s64 get_www(struct task*);
static bool volatile exit_shell = false;

__noreturn void kernel_panic(u32 error)
{
    // disable all context switches
    __cli();
    smp_all_stop();

    // attempt to set the screen to all red
    //efifb_clear(COLOR(255,0,0));
    for(u32 y = 540; y < 620; y++) {
        for(u32 x = 840; x < 920; x++) {
            efifb_putpixel(x, y, (color)error);
        }
    }

    // loop forever
    while(1) { asm("hlt"); }
}

static void initialize_kernel(struct multiboot_info* multiboot_info)
{
    // create a terminal before any print calls are made -- they won't
    // show up on screen until a framebuffer is enabled, but they are buffered in memory until then
    // for now this is safe to call immediately, since no memory allocation happens in terminal_init()
    terminal_init();

    // initialize serial port 
    serial_init();

    fprintf(stderr, "Boot..kernel_main at 0x%lX\n", (intp)kernel_main);

    // parse multiboot right away
    multiboot2_parse(multiboot_info);

    // create bootmem storage
    bootmem_init();

    // create the frame buffer so we can actually show the user stuff
    efifb_init();

    // parse the ACPI tables. we need it to enable interrupts
    acpi_init();

    // init CMOS/NMI and RTC
    cmos_init();

    // immediately setup and enable interrupts
    interrupts_init();

    // take over from the bootmem allocator
    palloc_init();
    kalloc_init();

    // take over from the page table initialized at boot
    __cli();           // disable interrupts before changing pages

    // with palloc and acpi ready, we can switch GDTs to dynamically allocated memory
    gdt_init();
    
    // gdt has to be fixed up to use _kernel_vma_base before switching the 
    // page table and interrupts over to highmem
    _gdt_fixup((intp)&_kernel_vma_base);

    // initialize paging
    efifb_disable();   // disable the framebuffer until it gets remapped. we can't have screen drawing during paging update
    paging_init();     // unmaps a large portion of lowmem

    // a few modules have to map new memory
    efifb_map();        // the EFI framebuffer needs virtual mapping, also re-enables the screen
    terminal_redraw(0); // remapping efifb may have missed some putpixel calls
    apic_map();         // the APIC needs memory mapping

    // add high memory blocks to palloc 
    palloc_init_highmem();

    // initialize the virtual memory manager
    vmem_init();

    // with palloc, paging and vmem initialized, we can now have a working malloc()... 
    free(malloc(1*1024*1024)); // initialize malloc with 1MiB of storage

    // safe to enable interrupts now
    __sti();

    // enable the kernel timer
    hpet_init();

    // map PCI into virtual memory
    pci_init();

    // finish ACPI initialization
    acpi_init_lai();

    // startup smp, multithreading and tasks
    smp_init();

    // initialize networking
    net_init();
}

static void load_drivers()
{
    // TODO "register" all the drivers first, then call pci_enumerate_devices() ?
    // or just load all the builtin modules and let them look for devices as they do now

    // enumerate system devices
    // in the future, this could happen after all drivers are "loaded",
    // and then as devices are discovered they can be mapped into their respective drivers
    // right now, drivers search for devices they're interested in
    pci_enumerate_devices();

    ps2keyboard_load();
    ahci_load();
    e1000_load();

}

static char current_directory[256] = "/";

static s64 open_directory(char* path, struct inode** inode)
{
    struct inode* parent = null;
    if(path[0] != '/') {
        if(open_directory(current_directory, &parent) < 0) return -1;
    } else {
        if(ext2_read_inode(2, &parent) < 0) return -1;
        while(*path == '/') path++;
    }

    while(*path != '\0') {
        if(!EXT2_ISDIR(parent)) {
            fprintf(stderr, "kernel: %s is not a valid directory\n", path);
            ext2_free_inode(parent);
            return -1;
        }

        // copy the next path part
        char pathpart[256] = { 0, };
        u32 partlen = 0;
        while(partlen < 255 && *path != '/' && *path != '\0') pathpart[partlen++] = *path++;
        pathpart[partlen] = 0;

        // skip all repeated /s
        while(*path == '/') path++;

        // loop over the directory entries looking for a match
        struct ext2_dirent_iter iter = EXT2_DIRENT_ITER_INIT(parent);
        struct ext2_dirent* dirent;
        struct inode* child = null;

        while((dirent = ext2_dirent_iter_next(&iter)) != null) {
            // check if the name matches in length first, then the name itself
            if(dirent->name_len != partlen) continue;
            if(memcmp(dirent->name, pathpart, dirent->name_len) != 0) continue;

            // name matches, so we can break out and use this inode
            if(ext2_read_inode(dirent->inode_number, &child) < 0) {
                ext2_dirent_iter_done(&iter);
                return -1;
            }

            break;
        }

        ext2_dirent_iter_done(&iter);

        // move onto the child directory
        ext2_free_inode(parent);
        parent = child;
    }

    *inode = parent;
    return 0;
}

static void run_command(char* cmdbuffer)
{
    char* end = cmdbuffer + strlen(cmdbuffer);
    char* cmdptr = strchr(cmdbuffer, ' ');
    if(cmdptr != null) {
        *cmdptr++ = '\0';
    } else {
        cmdptr = end;
    }

    if(strcmp(cmdbuffer, "pf") == 0) {
        *(u64 *)0x00007ffc00000000 = 1;    // page fault
    } else if(strcmp(cmdbuffer, "div0") == 0) {
        __asm__ volatile("div %0" : : "c"(0));  // division by 0 error
    } else if(strcmp(cmdbuffer, "gpf") == 0) {
        *(u32 *)0xf0fffefe00000000 = 1;  // gpf because address isn't canonical
    } else if(strcmp(cmdbuffer, "reboot") == 0) {
        fprintf(stderr, "calling lai_acpi_reset()\n");
        lai_api_error_t err = lai_acpi_reset();
        // we shouldn't see this, but sometimes ACPI reset isn't supported
        fprintf(stderr, "error = %s\n", lai_api_error_to_string(err));
        acpi_reset();
    } else if(strcmp(cmdbuffer, "sleep") == 0) {
        fprintf(stderr, "calling lai_acpi_sleep(5)\n");
        lai_enter_sleep(5);
    } else if(strcmp(cmdbuffer, "pci") == 0) {
        pci_dump_device_list();
    } else if(strcmp(cmdbuffer, "ahci") == 0) {
        ahci_dump_registers();
    } else if(strcmp(cmdbuffer, "exit") == 0) {
        exit_shell = true;
    } else if(strcmp(cmdbuffer, "date") == 0) {
        struct cmos_time cmostime;
        cmos_read_rtc(&cmostime);
        fprintf(stderr, "%02d:%02d:%02d %02d%02d:%02d:%02d (flags=0x%02X)\n",
                cmostime.hours, cmostime.minutes, cmostime.seconds, cmostime.century, cmostime.year, cmostime.month, cmostime.day, cmostime.flags);
        fprintf(stderr, "time(NULL) = %d\n", time(null));
    } else if(strcmp(cmdbuffer, "cd") == 0) {
        struct inode* dir;

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we found a path, open it instead
        if(*cmdptr != 0) {
            char* start = cmdptr;
            cmdptr = strchr(cmdptr, ' ');
            if(cmdptr != null) {
                *cmdptr++ = '\0';
            } else {
                cmdptr = end;
            }

            if(open_directory(start, &dir) < 0) return;

            if(*start == '/') {
                strcpy(current_directory, start);
            } else {
                char buf[256];
                strcpy(buf, current_directory);
                if(strcmp(buf, "/") == 0) {
                    sprintf(current_directory, "/%s", start);
                } else {
                    sprintf(current_directory, "%s/%s", buf, start);
                }
            }
        } else {
            // no path specified, reset current_directory to "/"
            strcpy(current_directory, "/");
            if(open_directory(current_directory, &dir) < 0) return;
        }

        if(dir != null) ext2_free_inode(dir);

    } else if(strcmp(cmdbuffer, "ls") == 0) {
        struct inode* dir;

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we found a path, open it instead
        if(*cmdptr != 0) {
            char* start = cmdptr;
            cmdptr = strchr(cmdptr, ' ');
            if(cmdptr != null) {
                *cmdptr++ = '\0';
            } else {
                cmdptr = end;
            }

            if(open_directory(start, &dir) < 0) {
                return;
            }
        } else {
            if(open_directory(current_directory, &dir) < 0) {
                return;
            }
        }

        if(dir != null) {
            struct ext2_dirent_iter iter = EXT2_DIRENT_ITER_INIT(dir);
            struct ext2_dirent* dirent;

            while((dirent = ext2_dirent_iter_next(&iter)) != null) {
                struct inode* entry;

                if(ext2_read_inode(dirent->inode_number, &entry) < 0) {
                    fprintf(stderr, "error reading inode %d\n", dirent->inode_number);
                    continue;
                }

                char buf[256];
                memcpy(buf, dirent->name, dirent->name_len);
                buf[dirent->name_len] = 0;
                fprintf(stderr, "%-32s mode=0x%04X size=%-12d inode=%-4d i_blocks=%-3d\n", buf, entry->ext2_inode->i_mode, entry->ext2_inode->i_size, entry->inode_number, entry->ext2_inode->i_blocks);

                ext2_free_inode(entry);
            }

            ext2_dirent_iter_done(&iter);
            ext2_free_inode(dir);
        }
    } else if(strcmp(cmdbuffer, "cat") == 0) {
        struct inode* dir;
        if(open_directory(current_directory, &dir) < 0) {
            return;
        }

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we have a parameter, look for it
        if(*cmdptr != 0) {
            char* start = cmdptr;
            cmdptr = strchr(cmdptr, ' ');
            if(cmdptr != null) {
                *cmdptr++ = '\0';
            } else {
                cmdptr = end;
            }
            u32 filename_len = strlen(start);

            // look for the file within the current directory
            struct ext2_dirent_iter iter = EXT2_DIRENT_ITER_INIT(dir);
            struct ext2_dirent* dirent;
            while((dirent = ext2_dirent_iter_next(&iter)) != null) {
                if(dirent->name_len != filename_len) continue;
                if(memcmp(dirent->name, start, dirent->name_len) != 0) continue;

                // file matches
                struct inode* file_inode;
                if(ext2_read_inode(dirent->inode_number, &file_inode) < 0) {
                    ext2_free_inode(dir);
                    return;
                }

                u64 offs = 0;
                u64 block_index = 0;
                while(offs < file_inode->ext2_inode->i_size) {
                    intp data;
                    if(ext2_read_inode_block(file_inode, block_index, &data) < 0) {
                        // failed to read data
                        ext2_free_inode(file_inode);
                        break;
                    }

                    u64 left = min(ext2_block_size(), file_inode->ext2_inode->i_size - offs);
                    for(u64 i = 0; i < left; i++) {
                        fprintf(stderr, "%c", *(u8*)(data + i));
                    }

                    palloc_abandon(data, 0);
                    offs += left;
                    block_index += 1;
                }
            }
        } 

        ext2_free_inode(dir);
    } else if(strcmp(cmdbuffer, "createfile") == 0) {
        struct inode* dir;
        if(open_directory(current_directory, &dir) < 0) {
            fprintf(stderr, "no current directory: %s\n", current_directory);
            return;
        }

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we have a parameter, look for it
        if(*cmdptr == 0) {
            fprintf(stderr, "no filename specified\n");
            return;
        }

        char* filename = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        // create file
        struct inode* newfile;
        ext2_create_file(dir, filename, &newfile);

        // write data to it
        ext2_write_inode_data(newfile, 0, (u8 const*)"hello, world!\n", 14);

        // increment file size, TODO move to ext2 module
        newfile->ext2_inode->i_size += 14;
        ext2_write_inode(newfile);
    } else if(strcmp(cmdbuffer, "mkdir") == 0) {
        struct inode* dir;
        if(open_directory(current_directory, &dir) < 0) {
            fprintf(stderr, "no current directory: %s\n", current_directory);
            return;
        }

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we have a parameter, look for it
        if(*cmdptr == 0) {
            fprintf(stderr, "no name specified\n");
            return;
        }

        char* newdirname = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        // create file
        struct inode* newdir;
        ext2_create_directory(dir, newdirname, &newdir);
    } else if(strcmp(cmdbuffer, "newtask") == 0) {
        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we have a parameter, look for it
        if(*cmdptr == 0) {
            fprintf(stderr, "no name specified\n");
            return;
        }

        char* stargetcpu = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        u32 targetcpu = atoi(stargetcpu);
        if(targetcpu >= apic_num_local_apics()) {
            fprintf(stderr, "cpu %d is out of range\n", targetcpu);
            return;
        }

        extern s64 userland_task_main(struct task*);
        struct task* newtask = task_create(userland_task_main, (intp)null, true);
        task_enqueue_for(targetcpu, newtask);
    } else if(strcmp(cmdbuffer, "pt") == 0) {
        paging_debug_table(get_cpu()->current_task->page_table);
    } else if(strcmp(cmdbuffer, "arp") == 0) {
        struct net_device* ndev = net_device_by_index(0); // grab the first network adapter
        if(ndev == null) return;

        struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the first IPv4 interface
        if(iface == null) return;

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we have a parameter, look for it
        if(*cmdptr == 0) {
            fprintf(stderr, "no name specified\n");
            return;
        }

        char* addr = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        // create an ARP request for the specified IP address
        struct net_address lookup_address;
        ipv4_parse_address_string(&lookup_address, addr);
        arp_send_request(iface, &lookup_address);

    } else if(strcmp(cmdbuffer, "ping") == 0) {
        struct net_device* ndev = net_device_by_index(0); // grab the first network adapter
        if(ndev == null) return;

        struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the first IPv4 interface
        if(iface == null) return;

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we have a parameter, look for it
        if(*cmdptr == 0) {
            fprintf(stderr, "no name specified\n");
            return;
        }

        char* addr = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        // send 10 ping packets to the destination
        struct net_address lookup_address;
        ipv4_parse_address_string(&lookup_address, addr);

        for(u16 seq = 0; seq < 10; seq++) {
            icmp_send_echo(iface, &lookup_address, seq);
            usleep(1000000);
        }
    } else if(strcmp(cmdbuffer, "udp") == 0) {
        struct net_device* ndev = net_device_by_index(0); // grab the first network adapter
        if(ndev == null) return;

        struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the first IPv4 interface
        if(iface == null) return;

        // skip whitespace or until end of string
        while((*cmdptr != 0) && (*cmdptr == ' ' || *cmdptr == '\t')) cmdptr++;

        // if we have a parameter, look for it
        if(*cmdptr == 0) {
            fprintf(stderr, "no name specified\n");
            return;
        }

        char* addr = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        // TODO convert to use net_socket_create/send/destroy. no more direct interface to udp_send_packet
        //! // send a udp packet
        //! struct net_address dest_address;
        //! ipv4_parse_address_string(&dest_address, addr);

        //! char* portstr = cmdptr;
        //! cmdptr = strchr(cmdptr, ' ');
        //! if(cmdptr != null) {
        //!     *cmdptr++ = '\0';
        //! } else {
        //!     cmdptr = end;
        //! }

        //! u16 port = atoi(portstr);
        //! if(port == 0) port = 8080;

        //! char payload[] = "hello!";
        //! udp_send_packet(iface, &dest_address, 10000, port, (u8*)payload, strlen(payload) + 1);
    } else if(strcmp(cmdbuffer, "listen") == 0) {
        // determine port to listen on
        char* portstr = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        u16 port = atoi(portstr);
        if(port == 0) port = 8000;

        // start the echo server
        struct task* echo_server_task = task_create(echo_server, (intp)port, false);
        struct cpu* cpu = get_cpu();
        task_enqueue(&cpu->current_task, echo_server_task);

    } else if(strcmp(cmdbuffer, "www") == 0) {
        // get IP or hostname to fetch from
        char* server = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        // start the fetch thread
        u32 slen = strlen(server);
        if(slen > 0) {
            struct buffer* buf = buffer_create(slen);
            buffer_write(buf, (u8*)server, slen);

            struct task* get_www_task = task_create(get_www, (intp)buf, false);
            struct cpu* cpu = get_cpu();
            task_enqueue(&cpu->current_task, get_www_task);
        }
    } else if(strcmp(cmdbuffer, "host") == 0) {
        // get hostname parameter
        char* hoststr = cmdptr;
        cmdptr = strchr(cmdptr, ' ');
        if(cmdptr != null) {
            *cmdptr++ = '\0';
        } else {
            cmdptr = end;
        }

        struct dns_result* dns_result = dns_lookup(hoststr);
        if(dns_result == null) {
            fprintf(stderr, "failed to lookup host %s\n", hoststr);
            return;
        }

        struct dns_record* rec = &dns_result->records[0];
        for(u32 i = 0; i < dns_result->num_records; i++, rec++) {
            char s[16];
            fprintf(stderr, "[%d] %s: ", i, rec->name);

            switch(rec->type) {
            case DNS_RECORD_TYPE_ADDRESS:
                switch(rec->address.protocol) {
                case NET_PROTOCOL_IPv4:
                    ipv4_format_address(s, rec->address.ipv4);
                    fprintf(stderr, "IPv4 address %s", s);
                    break;

                case NET_PROTOCOL_IPv6:
                    fprintf(stderr, "IPv6 address TODO");
                    break;
                }
                break;

            case DNS_RECORD_TYPE_NAMESERVER:
                fprintf(stderr, "nameserver %s", rec->ptr);
                break;

            default:
                fprintf(stderr, "unknown type %d", rec->internal_type);
                break;
            }

            fprintf(stderr, " (ttl = %d)\n", rec->ttl);
        }
    
        dns_result_destroy(dns_result);
    }
}

static void handle_keypress(char c, void* userdata)
{
    static char cmdbuffer[512];
    static u32 cmdlen = 0;

    unused(userdata);

    if(c == '\t') return;
    else if(c == '\n') {
        //terminal_putc((u16)c);
        fprintf(stderr, "%c", c);

        cmdbuffer[cmdlen] = 0;
        run_command(cmdbuffer);
        cmdlen = 0;

        fprintf(stderr, "%s:> ", current_directory);
    } else if(c == '\b') {
        // TODO
    } else {
        // save the final byte for \0
        if(cmdlen < 511) {
            cmdbuffer[cmdlen++] = c;
        }

        //terminal_putc((u16)c);
        fprintf(stderr, "%c", c);
    }
}

bool write_root_device_sector(struct filesystem_callbacks* fscbs, u64 start_sector, u64 write_count, intp src)
{
    return ahci_write_device_sectors((u8)((intp)fscbs->userdata & 0xFF), start_sector, write_count, src);
}

bool read_root_device_sector(struct filesystem_callbacks* fscbs, u64 start_sector, u64 read_count, intp dest)
{
    return ahci_read_device_sectors((u8)((intp)fscbs->userdata & 0xFF), start_sector, read_count, dest);
}

static s64 shell(struct task* task)
{
    s64 res = 0;

    fprintf(stderr, "shell started with task id = %d\n", task->task_id);

    // initialize the libc RNG
    struct cmos_time cmostime;
    cmos_read_rtc(&cmostime);
    u32 seed = 60 * (60 * (24 * (30 * (12 * cmostime.year + cmostime.month) + cmostime.day) + cmostime.hours) + cmostime.minutes) + cmostime.seconds;
    srand(seed);

    ps2keyboard_hook_ascii(&handle_keypress, null);

    u32 root_device = ahci_get_first_nonpacket_device_port();
    struct filesystem_callbacks ext2_fs = {
        .read_sectors       = &read_root_device_sector,
        .write_sectors      = &write_root_device_sector,
        .device_sector_size = ahci_get_device_sector_size(root_device),
        .userdata           = (void*)(intp)root_device,
    };

    if(ext2_open(&ext2_fs) < 0) {
        fprintf(stderr, "root device <#device:ahci #ahci:%d> is not an ext2 filesystem\n", root_device);
    } else {
        fprintf(stderr, "root device <#device:ahci #ahci:%d> found\n", root_device);
    }

    // configure the first IPv4 interface using dhcp
    fprintf(stderr, "configuring network...\n");
    struct net_device* ndev = net_device_by_index(0); // grab the first network adapter
    assert(ndev != null, "missing network device");
    struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the first IPv4 interface
    assert(iface != null, "missing network interface");
    if((res = dhcp_configure_network(iface, true)) < 0) {
        fprintf(stderr, "failed to configure network (err = %d)\n", res);
    }

    fprintf(stderr, "kernel shell ready...\n\n");
    fprintf(stderr, "%s:> ", current_directory);

    // update drivers forever (they should just use kernel tasks in the future)
    exit_shell = false;

    while(!exit_shell) {
        ps2keyboard_update();
        task_yield(TASK_YIELD_VOLUNTARY);
    }

    fprintf(stderr, "\n...exiting kernel shell...\n");
    return 0;
}

static s64 get_www(struct task* task)
{
    struct net_socket* socket = null;
    s64 res;

    // read the server address from the command line
    struct buffer* buf = (struct buffer*)task->userdata;
    u32 slen = buffer_remaining_read(buf);
    char* server = (char*)__builtin_alloca(slen);
    buffer_read(buf, (u8*)server, slen);
    buffer_destroy(buf);

    // lookup the address via DNS
    struct dns_result* dns_result = dns_lookup(server);
    if(dns_result == null) {
        fprintf(stderr, "failed to lookup host %s\n", server);
        goto done;
    }

    // grab the first A record
    struct dns_record* rec = &dns_result->records[0];
    for(u32 i = 0; i < dns_result->num_records; i++, rec++) {
        if(rec->type == DNS_RECORD_TYPE_ADDRESS && rec->address.protocol == NET_PROTOCOL_IPv4) break;
    }

    // if no records were found, error
    if(dns_result->num_records == 0 || rec->type != DNS_RECORD_TYPE_ADDRESS) {
        fprintf(stderr, "no A record found for server %s\n", server);
        goto done;
    }

    dns_result_destroy(dns_result);

    // create a sockinfo for our connection: destination is port 80 (HTTP)
    struct net_socket_info sockinfo;
    sockinfo.protocol     = NET_PROTOCOL_TCP;
    sockinfo.dest_address = rec->address;
    sockinfo.dest_port    = 80;

    // use the local interface as source address with a random source port
    struct net_device* ndev = net_device_by_index(0); // grab the first network adapter
    assert(ndev != null, "missing network device");
    struct net_interface* iface = net_device_get_interface_by_index(ndev, NET_PROTOCOL_IPv4, 0); // grab the first IPv4 interface
    assert(iface != null, "missing network interface");
    sockinfo.source_address = iface->address;
    sockinfo.source_port = 10000 + rand() % 40000;

    // create the socket
    socket = net_socket_create(iface, &sockinfo);
    if(socket == null) {
        fprintf(stderr, "failed to create socket\n");
        goto done;
    }

    // try connecting
    char serverip[16];
    ipv4_format_address(serverip, rec->address.ipv4);
    fprintf(stderr, "Connecting to %s [%s]...", server, serverip);
    if((res = net_socket_connect(socket)) < 0) goto done;
    fprintf(stderr, "connected!\n");

    // send HTTP request
    buf = buffer_create(512);
    buffer_puts(buf, "GET / HTTP/1.1\r\n");
    buffer_puts(buf, "Host: ");
    buffer_puts(buf, server);
    buffer_puts(buf, "\r\nAccept: text/html\r\n");
    buffer_puts(buf, "Connection: close\r\n");
    buffer_puts(buf, "\r\n");
    if((res = net_socket_send(socket, buf)) < 0) goto done;

    // read and print until the socket closes
    while(true) {
        struct buffer* response = buffer_create(512);
        if((res = net_socket_receive(socket, response, buffer_remaining_write(response))) <= 0) {
            buffer_destroy(response);
            break;
        }

        char s[512];
        u32 c = buffer_read(response, (u8*)s, buffer_remaining_read(response));
        s[c] = 0;
        fprintf(stderr, "%s\n", s);

        buffer_destroy(response);
    }

done:
    if(socket != null) net_socket_destroy(socket); 
    return res;
}

static s64 echo_server_per_socket(struct task* task)
{
    struct net_socket* socket = (struct net_socket*)task->userdata;

    char ip[16];
    ipv4_format_address(ip, socket->socket_info.source_address.ipv4);

    struct buffer* greeting = buffer_create(128);
    buffer_puts(greeting, "Welcome to my echo server, ");
    buffer_puts(greeting, ip);
    buffer_puts(greeting, "\n");
    net_socket_send(socket, greeting);

    while(true) {
        struct buffer* reply = buffer_create(512);
        buffer_write(reply, (u8*)"Echo: ", 6);
        if(net_socket_receive(socket, reply, buffer_remaining_write(reply)) <= 0) {
            // error/closed socket
            buffer_destroy(reply);
            break;
        }

        // reply is freed after it is sent
        if(net_socket_send(socket, reply) < 0) {
            buffer_destroy(reply);
            task_exit(-1);
        }
    }

    net_socket_destroy(socket); // close the socket if open, and free memory associated with the socket
    return 0;
}

static s64 echo_server(struct task* task)
{
    // create a listening socket on port 23 (telnet)
    struct net_socket_info sockinfo;
    zero(&sockinfo);
    sockinfo.protocol                = NET_PROTOCOL_TCP;
    sockinfo.dest_address.protocol   = NET_PROTOCOL_IPv4;
    sockinfo.source_address.protocol = NET_PROTOCOL_IPv4;
    sockinfo.source_address.ipv4     = 0; // listen on 0.0.0.0
    sockinfo.source_port             = (u16)task->userdata;

    struct net_socket* socket = net_socket_create(null, &sockinfo);
    if(socket == null) {
        fprintf(stderr, "could not create socket\n");
        return -1;
    }

    // start listening on said socket
    if(net_socket_listen(socket, 10) < 0) {
        fprintf(stderr, "could not listen on socket\n");
        net_socket_destroy(socket);
        return -1;
    }

    fprintf(stderr, "socket listening 0x%lX\n", net_socket_lookup(&sockinfo));

    // okay, listening socket is ready, wait for a socket
    while(true) {
        struct net_socket* peersocket = net_socket_accept(socket);

        char buf[16];
        ipv4_format_address(buf, peersocket->socket_info.source_address.ipv4);
        fprintf(stderr, "starting echo connection with %s:%d\n", buf, peersocket->socket_info.source_port);

        struct task* peer_echo_task = task_create(echo_server_per_socket, (intp)peersocket, false);
        task_enqueue(&get_cpu()->current_task, peer_echo_task);
    }
}

void kernel_main(struct multiboot_info* multiboot_info) 
{
    initialize_kernel(multiboot_info);
    load_drivers();

    // start the shell and exit
    struct task* shell_task = task_create(shell, (intp)null, false);
    struct cpu* cpu = get_cpu();
    task_enqueue(&cpu->current_task, shell_task);

    // never exit
    kernel_do_work();
}

__noreturn void kernel_do_work()
{
    task_set_preemtable(get_cpu()->current_task, false);

    // the workhorse of the kernel is here. we process high priority tasks
    // network tx/rx, disk and other i/o requests, and user task switching
    // the checks below are ordered such that they continue to run while there
    // is work to do
    while(true) {
        // clean up any exited tasks
        task_clean();

        //if(!priority_queue.empty()) { work = priority_queue.pop(); do_work(work); continue; }

        if(net_do_work()) continue;

        //if(io_do_work()) continue;

        // if there's no more work to do, yield to any running tasks
        //fprintf(stderr, "cpu%d: done with work\n", get_cpu()->cpu_index);
        task_yield(TASK_YIELD_VOLUNTARY); // faster than __hlt since it doesn't wait for preempt
    }
}


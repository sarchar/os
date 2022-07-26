
add_custom_command(OUTPUT os.iso
                   DEPENDS ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/os.bin 
                   COMMAND mkdir -p iso/boot/grub
		   COMMAND cp ${CMAKE_CURRENT_SOURCE_DIR}/grub.cfg iso/boot/grub/
		   COMMAND cp ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/os.bin iso/boot/
		   COMMAND grub-mkrescue -o os.iso iso/
)

add_custom_target(os_iso ALL 
                  DEPENDS os.iso
		  COMMAND mv os.iso ${CMAKE_BINARY_DIR})

add_custom_target(run
                  DEPENDS os_iso 
                  COMMAND qemu-system-x86_64 -machine q35
                                             -m 4G
                                             -bios /usr/share/ovmf/OVMF.fd
                                             -boot d
                                             -vga cirrus
                                             -monitor stdio
                                             -smp 4
                                             -serial file:${CMAKE_BINARY_DIR}/output.log
                                             -drive file=${CMAKE_BINARY_DIR}/os.iso,if=none,media=cdrom,format=raw,id=boot
                                             -drive file=${CMAKE_BINARY_DIR}/fs.qcow2,if=none,media=disk,format=qcow2,id=extra
                                             -device ahci,id=ahci
                                             -device ide-cd,drive=boot,bus=ahci.0
                                             -device ide-hd,drive=extra,bus=ahci.1
                                             -usbdevice mouse
                                             #-d int -D int.log
)

add_custom_target(gdb
                  DEPENDS os_iso 
                  COMMAND qemu-system-x86_64 -machine q35
                                             -m 4G
                                             -bios /usr/share/ovmf/OVMF.fd
                                             -boot d
                                             -vga cirrus
                                             -monitor stdio
                                             -smp 4
                                             -serial file:${CMAKE_BINARY_DIR}/output.log
                                             -drive file=${CMAKE_BINARY_DIR}/fs.qcow2,if=virtio,media=disk,format=qcow2
                                             -drive file=${CMAKE_BINARY_DIR}/os.iso,if=virtio,media=cdrom,format=raw
                                             -usbdevice mouse
                                             -s -S
)

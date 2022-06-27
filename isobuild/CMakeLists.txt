
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
                                             -cdrom ${CMAKE_BINARY_DIR}/os.iso
                                             -m 4G
                                             -bios /usr/share/ovmf/OVMF.fd
                                             -boot d
                                             -vga std
                                             -monitor stdio
)

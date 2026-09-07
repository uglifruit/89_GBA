@ crt0.s — Minimal GBA multiboot startup + ROM header.
@
@ The first 0xC0 bytes are the GBA header the BIOS requires (it compares the Nintendo logo
@ against its own copy and checks the header checksum, locking up if either is wrong — so
@ these are filled in post-link by gbafix). For MULTIBOOT the BIOS jumps to offset 0xC0
@ (0x020000C0), NOT the branch at 0x00, so our real entry point _start must sit exactly at
@ 0xC0. The image runs from EWRAM (0x02000000).
@
@ Header field map (0x00..0xBF):
@   0x00 : b _start_cart        (cartridge-boot branch; unused by multiboot)
@   0x04 : 156-byte Nintendo logo   (gbafix)
@   0xA0 : 12-byte title
@   0xAC : 4-byte game code
@   0xB0 : 2-byte maker code
@   0xB2 : 0x96 fixed
@   0xB3..0xBC : unit/device/reserved/version (0)
@   0xBD : header checksum          (gbafix)
@   0xBE : 2 reserved bytes (0)

    .section .crt0, "ax"
    .arm
    .global _headerstart
_headerstart:
    b _start_cart                   @ 0x00

    .space 156, 0x00                @ 0x04: Nintendo logo (gbafix fills this)

    .byte 'G','B','A','L','I','N','K',' ',' ',' ',' ',' '   @ 0xA0 title (12)
    .byte 'C','G','B','A'                                   @ 0xAC game code (4)
    .byte '0','0'                                           @ 0xB0 maker code (2)
    .byte 0x96                                              @ 0xB2 fixed
    .byte 0x00                                              @ 0xB3 main unit
    .byte 0x00                                              @ 0xB4 device type
    .space 7, 0x00                                          @ 0xB5 reserved
    .byte 0x00                                              @ 0xBC version
    .byte 0x00                                              @ 0xBD header checksum (gbafix)
    .space 2, 0x00                                          @ 0xBE reserved

    @ ---- 0xC0: MULTIBOOT ENTRY POINT ----
    @ The .space above brings us to exactly 0xC0 (0x04+156=0xA0, +32 header bytes = 0xC0).
    .arm
    .global _start
_start:
_start_cart:
    @ IWRAM (system) stack. NOT the conventional 0x03007F00: the BIOS hands our serial IRQ
    @ handler the IRQ stack at 0x03007FA0, and at 0x03007F00 that leaves it only 160 bytes
    @ before it collides with this one. link_pump() calls two levels deeper than a typical
    @ handler, so we move ours down and give the IRQ stack 416 bytes instead. Nothing else
    @ uses IWRAM at all — the whole image is linked for EWRAM — so this costs nothing.
    ldr sp, =0x03007E00

    @ Zero .bss. The section is NOLOAD, so it is NOT part of the multiboot image — whatever
    @ EWRAM happened to contain is still sitting there. C guarantees zero-initialised statics,
    @ so without this any static/global in the payload starts as garbage. Cheap insurance.
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    mov r2, #0
1:  cmp r0, r1
    strlo r2, [r0], #4
    blo 1b

    ldr r0, =main
    bx  r0                          @ enter main() (ARM)

.pool

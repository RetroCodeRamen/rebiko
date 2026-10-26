#ifndef CYBIKO_MACHINE_H
#define CYBIKO_MACHINE_H

/*
 * Compile-time machine selection. Build with -DCYBIKO_MACHINE_V1 for the
 * Cybiko Classic; the default is the Xtreme. This is a build switch, not a
 * runtime struct, because the bus fast paths in h8s_cpu.c and address_bus.c
 * compare against these as constants and the ESP32 speed depends on that.
 *
 * XT values are the ones the core always used. V1 values follow
 * MachineConfig.java (MIT, Copyright 2026 Dan Berkowitz). The Classic
 * radio is SCI0 and CyOS drives it. Status is in PLAN.md.
 */

#if defined(CYBIKO_MACHINE_V1) && defined(CYBIKO_MACHINE_XT)
#error "Define only one of CYBIKO_MACHINE_V1 / CYBIKO_MACHINE_XT"
#endif
#if !defined(CYBIKO_MACHINE_V1) && !defined(CYBIKO_MACHINE_XT)
#define CYBIKO_MACHINE_XT 1
#endif

#if defined(CYBIKO_MACHINE_XT)

#define CYBIKO_MACHINE_NAME        "Cybiko Xtreme"
#define CYBIKO_CLOCK_HZ            18432000

#define CYBIKO_BOOT_ROM_SIZE       0x8000
#define CYBIKO_BOOT_ROM_MIRROR_END 0x03FFFF   /* 32 KB mirrored to 256 KB */

#define CYBIKO_EXT_RAM_BASE        0x400000
#define CYBIKO_EXT_RAM_SIZE        0x200000   /* 2 MB */

#define CYBIKO_LCD_BASE            0x100000

#define CYBIKO_HAS_FLASH_MAP       1
#define CYBIKO_FLASH_BASE          0x600000
#define CYBIKO_FLASH_END           0x7FFFFF   /* 512 KB mirrored */
#define CYBIKO_FLASH_SIZE          0x80000

#define CYBIKO_HAS_USB             1          /* USBN9604 stub at 0x200000 */
#define CYBIKO_HAS_DMA             1
#define CYBIKO_HAS_SPI_FLASH       0

#define CYBIKO_ON_CHIP_BASE        0xFFDC00
#define CYBIKO_ON_CHIP_SIZE        0x2400

#define CYBIKO_TIMER16_CHANNELS    6

#define CYBIKO_KEYBOARD_SCAN_COLS  10

/* Port A reads back power on + battery charged */
#define CYBIKO_POWER_PORT_IN       0xFFFF59
#define CYBIKO_POWER_PORT_DR       0xFFFF69
#define CYBIKO_POWER_VALUE         0xC0

/* PCF8593 SDA on Port F bit 6, written inverted through the DR */
#define CYBIKO_RTC_SDA_BIT         0x40

#elif defined(CYBIKO_MACHINE_V1)

#define CYBIKO_MACHINE_NAME        "Cybiko Classic"
#define CYBIKO_CLOCK_HZ            11059200

#define CYBIKO_BOOT_ROM_SIZE       0x8000
#define CYBIKO_BOOT_ROM_MIRROR_END 0x007FFF   /* no mirror */

#define CYBIKO_EXT_RAM_BASE        0x200000
#define CYBIKO_EXT_RAM_SIZE        0x80000    /* 512 KB */

#define CYBIKO_LCD_BASE            0x600000

#define CYBIKO_HAS_FLASH_MAP       0          /* CyOS is on AT45DB041 SPI flash */
#define CYBIKO_HAS_USB             0
#define CYBIKO_HAS_DMA             0
/* AT45DB041 on SCI1, CS = Port 3 bit 4, DTC block at 0xFFFBD0 */
#define CYBIKO_HAS_SPI_FLASH       1

#define CYBIKO_ON_CHIP_BASE        0xFFEC00
#define CYBIKO_ON_CHIP_SIZE        0x1400

#define CYBIKO_TIMER16_CHANNELS    3

#define CYBIKO_KEYBOARD_SCAN_COLS  9

/* Port 1 input bit 3 = power on. V1 has no Port A power readback. */
#define CYBIKO_POWER_PORT_IN       0xFFFF50
#define CYBIKO_POWER_VALUE         0x08

/* PCF8593 SDA on Port F bit 0, driven open-drain through the Port F DDR
 * (bug #10 in the Java notes): DDR bit set pulls SDA low. */
#define CYBIKO_RTC_SDA_BIT         0x01
#define CYBIKO_RTC_SDA_VIA_DDR     1

/* Key press raises IRQ2 (vector 18); CyOS scans the matrix in its ISR */
#define CYBIKO_KEY_IRQ_VECTOR      18

/* ADC battery channels, 10-bit. CyOS 1.2.46 needs ch1 - ch2 > 15 to pass
 * its battery check (bug #14); these are the Java emulator's values. */
#define CYBIKO_ADC_DEFAULT_CH1     0x0300
#define CYBIKO_ADC_DEFAULT_CH2     0x0100
#define CYBIKO_ADC_DEFAULT_OTHER   0x0100

#endif

#define CYBIKO_EXT_RAM_END   (CYBIKO_EXT_RAM_BASE + CYBIKO_EXT_RAM_SIZE - 1)
#define CYBIKO_LCD_END       (CYBIKO_LCD_BASE + 1)
/* I/O registers start here on every variant; below it is plain on-chip RAM */
#define CYBIKO_ON_CHIP_IO    0xFFFE00

#endif

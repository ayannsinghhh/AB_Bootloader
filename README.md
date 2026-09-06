# STM32F4 A/B Custom Bootloader

This project implements a custom A/B bootloader for the STM32F411 microcontroller.

The bootloader supports firmware updates over UART (USART2) using a custom
frame-based communication protocol. It maintains two firmware slots and
provides basic firmware verification, boot management, and rollback support.

A Python-based companion GUI is included to communicate with the bootloader
and upload firmware.

---

## Features

* **A/B Firmware Slots**
  * **Primary Slot:** 256 KB (Sectors 5 + 6)
  * **Primary Address:** `0x08020000`
  * **Secondary Slot:** 128 KB (Sector 7)
  * **Secondary Address:** `0x08060000`

* **Bootloader Menu**
  * Display firmware information for each slot.
  * Select the primary slot as the upload target.
  * Select the secondary slot as the upload target.

* **Automatic Boot**
  * The bootloader provides a menu before starting the application.
  * If no option is selected, the bootloader automatically starts the
    currently selected application.

* **Firmware Update**
  * Firmware is transferred over USART2.
  * Firmware is divided into fixed-size data frames.
  * Each frame contains a CRC32 value for error detection.

* **Firmware Verification**
  * Firmware headers contain information such as firmware size and version.
  * The STM32 CRC peripheral is used to verify the uploaded image before
    booting it.

* **A/B Boot and Rollback**
  * A newly uploaded firmware is marked as pending.
  * The application confirms successful startup using `boot_confirm()`.
  * If the new firmware repeatedly fails to confirm, the bootloader can
    roll back to the previously active slot.

* **Safe Application Jump**
  * The bootloader checks the application's initial Stack Pointer.
  * The Reset Handler address is checked before jumping to the application.
  * The application's vector table is loaded before execution.

* **Python Companion GUI**
  * Serial communication with the bootloader.
  * Firmware `.bin` file selection.
  * Firmware version configuration.
  * Firmware upload and progress monitoring.
  * Bootloader menu interaction.

---

## Hardware & Software Requirements

### Hardware

* STM32F411 microcontroller
* USB-to-UART adapter
* ST-Link debugger

USART2 is used for communication:

```text
STM32F411        USB-to-UART

PA2 (TX)   --->  RX
PA3 (RX)   <---  TX
GND        --->  GND

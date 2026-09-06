# Testing the Bootloader with a Simple LED Blink User Application

This guide explains how to test the bootloader by uploading a simple user
application that blinks an LED.

This setup assumes:

- Target MCU: STM32F411CEU6
- STM32CubeIDE or another STM32 toolchain is installed
- Tera Term is installed on the PC
- Python 3 and pyserial are installed for the firmware uploader
- The bootloader is already programmed into the STM32
- USART2 is used for communication with the bootloader

## 1. Create the User Application

Create a new STM32CubeIDE project for the STM32F411CEU6.

The user application should be configured to run from the application address
defined by the bootloader.

For testing, the application simply blinks an LED continuously.

The application also contains the `boot_confirm()` function. This function
informs the bootloader that the newly uploaded firmware has started
successfully.

The main application flow is:

```text
Bootloader
    |
    v
User Application
    |
    v
boot_confirm()
    |
    v
Initialize LED
    |
    v
Blink LED continuously

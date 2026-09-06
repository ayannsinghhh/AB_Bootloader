#ifndef _BOOTLOADER
#define _BOOTLOADER
#include "main.h"
#include "usart.h"
#include "gpio.h"
#include "stdarg.h"
#include "stdio.h"
#include "string.h"
#include "frames.h"
#include "stdint.h"
#include "stdbool.h"
void bootloader_main(void);
void bootloader_USART2_callback(uint8_t data);

extern uint8_t bytes_buff[sizeof(frame_format_t)];
extern volatile uint8_t bytes_received_count;

#endif

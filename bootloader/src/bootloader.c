#include "bootloader.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* Flash layout
 *
 * Sector 0 - Bootloader
 * Sector 1 - Bootloader
 * Sector 2 - Reserved
 * Sector 3 - Reserved
 * Sector 4 - Boot configuration
 * Sector 5 - Primary application
 * Sector 6 - Primary application
 * Sector 7 - Secondary application
 */

#define PRIMARY_APP_ADDRESS   0x08020000u
#define PRIMARY_SLOT_SIZE     (256u * 1024u)

#define SECONDARY_APP_ADDRESS 0x08060000u
#define SECONDARY_SLOT_SIZE   (128u * 1024u)

#define CONFIG_FLASH_SECTOR   FLASH_SECTOR_4
#define CONFIG_FLASH_ADDRESS  0x08010000u

#define USER_APPLICATION      (PRIMARY_APP_ADDRESS + 4u)


uint32_t start_address = PRIMARY_APP_ADDRESS;
uint32_t app_base_address = PRIMARY_APP_ADDRESS;

uint8_t bytes_buff[sizeof(frame_format_t)] = {0};
volatile uint8_t bytes_received_count = 0;

frame_format_t receivedFrame;
volatile static bool parse = false;

const size_t FRAME_SIZE = sizeof(frame_format_t);

bootloader_state bootloader_current_state = STATE_IDLE;

frame_format_t ackFrame;
frame_format_t nackFrame;

frame_format_t (*bootloader_state_functions[3])(void);

fw_header_t fw_header;

/* Set when the user selects a slot for firmware upload. */
volatile bool upload_mode = false;


/* Function prototypes */

static void print(char *msg, ...);
static void jump_to_user_app(void);

void save_bootloader_config(bootloader_config_t *cfg);
bootloader_config_t *load_bootloader_config(void);

static void uart_send_data(uint8_t *data, uint16_t len);
static bool parse_frame(void);

void bootloaderInit(void);
frame_format_t idle_state_func(void);
frame_format_t updating_state_func(void);

static void reset_received_frame(void);
static void set_bl_state(bootloader_state state);
static void sendFrame(frame_format_t *frame);

void erase_sectors_for_user_app(uint32_t app_start_address,
                                uint32_t app_size);

uint32_t get_sector(uint32_t address);

static void write_payload(void);
void erase_sector(void);

static boot_state_t load_boot_state(void);
static void save_boot_state(boot_state_t *bs);
static bool verify_image_crc(uint32_t slot_addr, fw_header_t *hdr);

static void show_boot_menu(uint32_t timeout_ms);
static void display_slot_info(uint8_t slot);
static bool read_fw_header_from_flash(uint32_t addr, fw_header_t *out);


/* -------------------------------------------------------------------------- */
/* Bootloader main                                                            */
/* -------------------------------------------------------------------------- */

void bootloader_main(void)
{
    bootloaderInit();

    boot_state_t bs = load_boot_state();

    /*
     * If an update is pending, try that slot first.
     * If it keeps failing, go back to the confirmed slot.
     */
    if (bs.pending_slot != 0xFF)
    {
        if (bs.boot_attempt >= MAX_BOOT_ATTEMPTS)
        {
            print("[BL] Rollback: pending slot failed after %d attempts, "
                  "reverting to slot %d\r\n",
                  MAX_BOOT_ATTEMPTS,
                  bs.active_slot);

            bs.pending_slot = 0xFF;
            bs.boot_attempt = 0;
            bs.image_confirmed = 0;

            save_boot_state(&bs);
        }
        else
        {
            bs.boot_attempt++;
            save_boot_state(&bs);

            app_base_address = (bs.pending_slot == 0) ?
                               PRIMARY_APP_ADDRESS :
                               SECONDARY_APP_ADDRESS;

            print("[BL] Booting pending slot %d (attempt %d/%d)\r\n",
                  bs.pending_slot,
                  bs.boot_attempt,
                  MAX_BOOT_ATTEMPTS);

            jump_to_user_app();
        }
    }
    else
    {
        app_base_address = (bs.active_slot == 0) ?
                           PRIMARY_APP_ADDRESS :
                           SECONDARY_APP_ADDRESS;
    }

    /*
     * Give the user some time to select an option from the menu.
     */
    show_boot_menu(20000);

    /*
     * Upload mode is entered when the user selects a target slot.
     * In this mode the bootloader stays here until the update finishes.
     */
    if (upload_mode)
    {
        USART2->CR1 |= USART_CR1_RXNEIE;

        bootloader_state_functions[STATE_IDLE] = idle_state_func;
        bootloader_state_functions[STATE_UPDATING] = updating_state_func;

        while (upload_mode)
        {
            (*bootloader_state_functions[bootloader_current_state])();
            HAL_Delay(10);
        }
    }

    USART2->CR1 |= USART_CR1_RXNEIE;

    uint32_t curr_time = HAL_GetTick();

    bootloader_state_functions[STATE_IDLE] = idle_state_func;
    bootloader_state_functions[STATE_UPDATING] = updating_state_func;

    while (1)
    {
        (*bootloader_state_functions[bootloader_current_state])();

        /*
         * Automatically start the selected application after 5 seconds
         * if the bootloader is still idle.
         */
        if (((HAL_GetTick() - curr_time) > 5000) &&
            (bootloader_current_state == STATE_IDLE) &&
            (!upload_mode))
        {
            print("Launching user app...\r\n");
            jump_to_user_app();
        }
    }
}


/* -------------------------------------------------------------------------- */
/* Bootloader initialization                                                  */
/* -------------------------------------------------------------------------- */

void bootloaderInit(void)
{
    ackFrame.start_of_frame = BL_START_OF_FRAME;
    ackFrame.frame_id = BL_ACK_FRAME;
    ackFrame.payload_len = PAYLOAD_LEN;
    ackFrame.end_of_frame = BL_END_OF_FRAME;

    for (int i = 0; i < PAYLOAD_LEN; i++)
    {
        ackFrame.payload[i] = i;
    }


    nackFrame.start_of_frame = BL_START_OF_FRAME;
    nackFrame.frame_id = BL_NACK_FRAME;
    nackFrame.payload_len = PAYLOAD_LEN;
    nackFrame.end_of_frame = BL_END_OF_FRAME;

    for (int i = 0; i < PAYLOAD_LEN; i++)
    {
        nackFrame.payload[i] = i;
    }
}


/* -------------------------------------------------------------------------- */
/* Boot state                                                                  */
/* -------------------------------------------------------------------------- */

static boot_state_t load_boot_state(void)
{
    boot_state_t *state =
        (boot_state_t *)CONFIG_FLASH_ADDRESS;

    if (state->magic != BOOT_STATE_MAGIC)
    {
        boot_state_t default_state;

        default_state.magic = BOOT_STATE_MAGIC;
        default_state.active_slot = 0;
        default_state.pending_slot = 0xFF;
        default_state.boot_attempt = 0;
        default_state.image_confirmed = 0;
        default_state.reserved = 0;

        return default_state;
    }

    return *state;
}


static void save_boot_state(boot_state_t *bs)
{
    bs->magic = BOOT_STATE_MAGIC;

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = CONFIG_FLASH_SECTOR;
    erase.NbSectors = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASHEx_Erase(&erase, &sector_error);

    uint32_t *data = (uint32_t *)bs;
    uint32_t address = CONFIG_FLASH_ADDRESS;

    for (uint32_t i = 0; i < sizeof(boot_state_t) / 4; i++)
    {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          address,
                          data[i]);

        address += 4;
    }

    HAL_FLASH_Lock();
}


/* -------------------------------------------------------------------------- */
/* Firmware verification                                                      */
/* -------------------------------------------------------------------------- */

static bool verify_image_crc(uint32_t slot_addr, fw_header_t *hdr)
{
    if (hdr == NULL)
    {
        return false;
    }

    if (hdr->magic != FW_HEADER_MAGIC)
    {
        return false;
    }

    if (hdr->fw_size == 0 ||
        hdr->fw_size > PRIMARY_SLOT_SIZE)
    {
        return false;
    }

    uint32_t image_start = slot_addr + sizeof(fw_header_t);
    uint32_t word_count = hdr->fw_size / 4;

    __HAL_CRC_DR_RESET(&hcrc);

    uint32_t calculated_crc =
        HAL_CRC_Calculate(&hcrc,
                          (uint32_t *)image_start,
                          word_count);

    return calculated_crc == hdr->crc32;
}


/* -------------------------------------------------------------------------- */
/* Bootloader states                                                          */
/* -------------------------------------------------------------------------- */

frame_format_t idle_state_func(void)
{
    if (parse_frame())
    {
        switch (receivedFrame.frame_id)
        {
            case BL_START_UPDATE:

                set_bl_state(STATE_UPDATING);
                reset_received_frame();
                sendFrame(&ackFrame);

                break;

            default:

                set_bl_state(STATE_IDLE);
                reset_received_frame();

                break;
        }
    }

    return ackFrame;
}


frame_format_t updating_state_func(void)
{
    static bool erased = false;

    if (parse_frame())
    {
        switch (receivedFrame.frame_id)
        {
            case BL_PAYLOAD:
            {
                if (!erased)
                {
                    uint32_t app_size = fw_header.fw_size;

                    uint32_t slot_max =
                        (start_address == PRIMARY_APP_ADDRESS) ?
                        PRIMARY_SLOT_SIZE :
                        SECONDARY_SLOT_SIZE;

                    if (app_size == 0 ||
                        app_size == 0xFFFFFFFFu ||
                        app_size > slot_max)
                    {
                        app_size = slot_max;
                    }

                    erase_sectors_for_user_app(start_address, app_size);

                    erased = true;
                }

                write_payload();
                sendFrame(&ackFrame);

                break;
            }


            case BL_UPDATE_DONE:
            {
                fw_header_t *stored_hdr =
                    (fw_header_t *)app_base_address;

                if (stored_hdr->magic != FW_HEADER_MAGIC)
                {
                    print("[BL] ERROR: No valid firmware header found "
                          "after write.\r\n");

                    sendFrame(&nackFrame);
                    erased = false;

                    break;
                }

                if (!verify_image_crc(app_base_address, stored_hdr))
                {
                    print("[BL] ERROR: Image CRC32 mismatch! "
                          "Firmware rejected.\r\n");

                    sendFrame(&nackFrame);
                    erased = false;

                    break;
                }

                boot_state_t bs = load_boot_state();

                uint8_t new_slot =
                    (app_base_address == PRIMARY_APP_ADDRESS) ? 0 : 1;

                bs.pending_slot = new_slot;
                bs.boot_attempt = 0;
                bs.image_confirmed = 0;

                save_boot_state(&bs);

                print("[BL] Image verified OK. Pending slot %d set. "
                      "Booting...\r\n",
                      new_slot);

                sendFrame(&ackFrame);

                upload_mode = false;
                erased = false;

                jump_to_user_app();

                break;
            }


            case BL_HEADER:

                memcpy(&fw_header,
                       receivedFrame.payload,
                       sizeof(fw_header));

                sendFrame(&ackFrame);

                break;


            case BL_STATUS_CHECK:

                sendFrame(&ackFrame);

                break;


            default:

                sendFrame(&nackFrame);

                break;
        }

        reset_received_frame();
    }

    return ackFrame;
}


/* -------------------------------------------------------------------------- */
/* Flash write                                                                */
/* -------------------------------------------------------------------------- */

static void write_payload(void)
{
    HAL_FLASH_Unlock();

    for (int i = 0; i < 16; i += 4)
    {
        uint32_t *value =
            (uint32_t *)&receivedFrame.payload[i];

        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          start_address,
                          *value);

        if (*(uint32_t *)start_address != *value)
        {
            print("Flash write verification failed at "
                  "0x%08lx\r\n",
                  start_address);

            sendFrame(&nackFrame);
            break;
        }

        start_address += 4;
    }

    HAL_FLASH_Lock();

    reset_received_frame();
}


/* -------------------------------------------------------------------------- */
/* Frame handling                                                             */
/* -------------------------------------------------------------------------- */

static void reset_received_frame(void)
{
    receivedFrame.start_of_frame = 0;
    receivedFrame.frame_id = 0;
    receivedFrame.payload_len = 0;
    receivedFrame.crc32 = 0;
    receivedFrame.end_of_frame = 0;

    for (int i = 0; i < 16; i++)
    {
        receivedFrame.payload[i] = 0;
    }
}


static void set_bl_state(bootloader_state state)
{
    bootloader_current_state = state;
}


static bool parse_frame(void)
{
    if (!parse)
    {
        return false;
    }

    parse = false;

    memcpy(&receivedFrame,
           bytes_buff,
           sizeof(frame_format_t));

    memset(bytes_buff, 0, sizeof(frame_format_t));

    if (receivedFrame.start_of_frame != BL_START_OF_FRAME ||
        receivedFrame.end_of_frame != BL_END_OF_FRAME)
    {
        return false;
    }

    if (receivedFrame.payload_len > PAYLOAD_LEN)
    {
        sendFrame(&nackFrame);
        return false;
    }

    __HAL_CRC_DR_RESET(&hcrc);

    uint32_t calculated_crc =
        HAL_CRC_Calculate(&hcrc,
                          (uint32_t *)receivedFrame.payload,
                          receivedFrame.payload_len / 4);

    if (calculated_crc != receivedFrame.crc32)
    {
        sendFrame(&nackFrame);
        return false;
    }

    return true;
}


void bootloader_USART2_callback(uint8_t data)
{
    if (bytes_received_count < FRAME_SIZE)
    {
        bytes_buff[bytes_received_count++] = data;

        if (bytes_received_count == FRAME_SIZE)
        {
            bytes_received_count = 0;
            parse = true;
        }
    }
}


/* -------------------------------------------------------------------------- */
/* Application jump                                                           */
/* -------------------------------------------------------------------------- */

static void jump_to_user_app(void)
{
    uint32_t app_addr = app_base_address;

    uint32_t appStack =
        *((uint32_t *)app_addr);

    uint32_t appResetHandler =
        *((uint32_t *)(app_addr + 4));

    print("Read from 0x%08lX: MSP=0x%08lX, "
          "ResetHandler=0x%08lX\r\n",
          app_addr,
          appStack,
          appResetHandler);


    /* Check that the application stack pointer is inside SRAM. */
    if (appStack < 0x20000000 ||
        appStack > 0x20020000)
    {
        print("ERROR: Invalid Stack Pointer read from app! "
              "Halting.\r\n");

        return;
    }


    uint32_t slot_start = app_addr;

    uint32_t slot_size =
        (app_addr == PRIMARY_APP_ADDRESS) ?
        PRIMARY_SLOT_SIZE :
        SECONDARY_SLOT_SIZE;


    /* Make sure the reset handler points inside the selected slot. */
    if (appResetHandler < slot_start ||
        appResetHandler >= (slot_start + slot_size))
    {
        print("ERROR: Reset Handler 0x%08lX outside slot "
              "[0x%08lX, 0x%08lX). Halting.\r\n",
              appResetHandler,
              slot_start,
              slot_start + slot_size);

        return;
    }


    HAL_UART_DeInit(&huart2);
    HAL_CRC_DeInit(&hcrc);
    HAL_DeInit();

    /* Stop SysTick before changing the vector table. */
    SysTick->CTRL = 0;

    __disable_irq();

    /* Use the application's vector table. */
    SCB->VTOR = app_addr;

    /* Set the application's stack pointer. */
    __set_MSP(appStack);

    /* Start the application. */
    void (*resetHandler)(void) =
        (void (*)(void))appResetHandler;

    resetHandler();
}


/* -------------------------------------------------------------------------- */
/* UART                                                                       */
/* -------------------------------------------------------------------------- */

static void print(char *msg, ...)
{
    char buff[250];

    va_list args;

    va_start(args, msg);
    vsprintf(buff, msg, args);
    va_end(args);

    for (int i = 0; i < strlen(buff); i++)
    {
        USART2->DR = buff[i];

        while (!(USART2->SR & USART_SR_TXE))
        {
            ;
        }
    }

    while (!(USART2->SR & USART_SR_TC))
    {
        ;
    }
}


static void uart_send_data(uint8_t *data, uint16_t len)
{
    for (int i = 0; i < len; i++)
    {
        USART2->DR = data[i];

        while (!(USART2->SR & USART_SR_TXE))
        {
            ;
        }
    }

    while (!(USART2->SR & USART_SR_TC))
    {
        ;
    }
}


static void sendFrame(frame_format_t *frame)
{
    uint8_t *data = (uint8_t *)frame;

    uart_send_data(data, sizeof(frame_format_t));
}


/* -------------------------------------------------------------------------- */
/* Flash erase                                                                */
/* -------------------------------------------------------------------------- */

void erase_sector(void)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = 0;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.NbSectors = 1;
    erase.Sector = FLASH_SECTOR_5;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_FLASH_Unlock();

    HAL_FLASHEx_Erase(&erase, &error);

    HAL_FLASH_Lock();
}


uint32_t get_sector(uint32_t address)
{
    if (address < 0x08004000)
    {
        return FLASH_SECTOR_0;
    }
    else if (address < 0x08008000)
    {
        return FLASH_SECTOR_1;
    }
    else if (address < 0x0800C000)
    {
        return FLASH_SECTOR_2;
    }
    else if (address < 0x08010000)
    {
        return FLASH_SECTOR_3;
    }
    else if (address < 0x08020000)
    {
        return FLASH_SECTOR_4;
    }
    else if (address < 0x08040000)
    {
        return FLASH_SECTOR_5;
    }
    else if (address < 0x08060000)
    {
        return FLASH_SECTOR_6;
    }
    else
    {
        return FLASH_SECTOR_7;
    }
}


void erase_sectors_for_user_app(uint32_t app_start_address,
                                uint32_t app_size)
{
    if (app_size == 0)
    {
        return;
    }

    uint32_t app_end_address =
        app_start_address + app_size - 1;

    uint32_t first_sector =
        get_sector(app_start_address);

    uint32_t last_sector =
        get_sector(app_end_address);

    uint32_t sector_error = 0;

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {0};

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    for (uint32_t sector = first_sector;
         sector <= last_sector;
         sector++)
    {
        erase.Sector = sector;
        erase.NbSectors = 1;

        if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
        {
            print("ERROR: Erase failed at sector %d, "
                  "err code: %lu\r\n",
                  sector,
                  sector_error);

            break;
        }

        print("Sector %d erased successfully\r\n", sector);
    }

    HAL_FLASH_Lock();
}


/* -------------------------------------------------------------------------- */
/* Bootloader configuration                                                   */
/* -------------------------------------------------------------------------- */

void save_bootloader_config(bootloader_config_t *cfg)
{
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;

    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = CONFIG_FLASH_SECTOR;
    erase.NbSectors = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return;
    }

    uint32_t *data = (uint32_t *)cfg;
    uint32_t address = CONFIG_FLASH_ADDRESS;

    for (uint32_t i = 0;
         i < sizeof(bootloader_config_t) / 4;
         i++)
    {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                          address,
                          data[i]);

        address += 4;
    }

    HAL_FLASH_Lock();
}


bootloader_config_t *load_bootloader_config(void)
{
    return (bootloader_config_t *)CONFIG_FLASH_ADDRESS;
}


/* -------------------------------------------------------------------------- */
/* Boot menu                                                                  */
/* -------------------------------------------------------------------------- */

static void show_boot_menu(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    /* RX interrupt is not needed while handling the menu. */
    USART2->CR1 &= ~USART_CR1_RXNEIE;

    bool menu_printed = false;

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        if (!menu_printed)
        {
            print("\r\n--- Bootloader Menu (%lu ms) ---\r\n",
                  timeout_ms);

            print("1: Primary firmware (press 1 to show info)\r\n");
            print("2: Secondary firmware (press 2 to show info)\r\n");
            print("3: Select primary slot as upload target\r\n");
            print("4: Select secondary slot as upload target\r\n");
            print("Press any other key to continue immediately.\r\n\r\n");

            menu_printed = true;
        }


        if (USART2->SR & USART_SR_RXNE)
        {
            uint8_t c = (uint8_t)(USART2->DR & 0xFF);

            if (c == '1' || c == '2')
            {
                display_slot_info((uint8_t)(c - '0'));

                start = HAL_GetTick();
                menu_printed = false;
            }
            else if (c == '3' || c == '4')
            {
                upload_mode = true;

                if (c == '3')
                {
                    start_address = PRIMARY_APP_ADDRESS;
                    app_base_address = PRIMARY_APP_ADDRESS;

                    memset(&fw_header,
                           0xFF,
                           sizeof(fw_header));

                    print("Upload target set to PRIMARY "
                          "(0x%08lX).\r\n",
                          start_address);
                }
                else
                {
                    start_address = SECONDARY_APP_ADDRESS;
                    app_base_address = SECONDARY_APP_ADDRESS;

                    memset(&fw_header,
                           0xFF,
                           sizeof(fw_header));

                    print("Upload target set to SECONDARY "
                          "(0x%08lX).\r\n",
                          start_address);
                }

                print("Please upload firmware using the "
                      "companion GUI.\r\n");

                USART2->CR1 |= USART_CR1_RXNEIE;

                break;
            }
            else
            {
                break;
            }
        }

        HAL_Delay(10);
    }


    if (!upload_mode)
    {
        print("Exiting boot menu...\r\n\r\n");

        USART2->CR1 |= USART_CR1_RXNEIE;
    }
    else
    {
        print("Upload mode active - listening for "
              "update frames...\r\n");
    }
}


static void display_slot_info(uint8_t slot)
{
    uint32_t address = 0;
    uint32_t size = 0;

    fw_header_t header;


    if (slot == 1)
    {
        address = PRIMARY_APP_ADDRESS;
        size = PRIMARY_SLOT_SIZE;

        print("Primary slot:\r\n");
    }
    else if (slot == 2)
    {
        address = SECONDARY_APP_ADDRESS;
        size = SECONDARY_SLOT_SIZE;

        print("Secondary slot:\r\n");
    }
    else
    {
        print("Unknown slot %d\r\n", slot);
        return;
    }


    print("  Start Address: 0x%08lX\r\n", address);
    print("  Max Size: %lu bytes\r\n", size);


    if (read_fw_header_from_flash(address, &header))
    {
        print("  Stored FW - version: 0x%08lX, "
              "size: %lu bytes\r\n",
              header.fw_version,
              header.fw_size);
    }
    else
    {
        print("  No valid firmware header found "
              "in this slot\r\n");
    }


    print("\r\n");
    print("Press any key to return to the menu...\r\n");


    while (!(USART2->SR & USART_SR_RXNE))
    {
        HAL_Delay(10);
    }

    (void)USART2->DR;
}


static bool read_fw_header_from_flash(uint32_t addr,
                                      fw_header_t *out)
{
    if (out == NULL)
    {
        return false;
    }

    fw_header_t *header =
        (fw_header_t *)addr;


    if (header->fw_size == 0xFFFFFFFFu ||
        header->fw_version == 0xFFFFFFFFu)
    {
        return false;
    }


    uint32_t slot_max =
        (addr == PRIMARY_APP_ADDRESS) ?
        PRIMARY_SLOT_SIZE :
        SECONDARY_SLOT_SIZE;


    if (header->fw_size == 0 ||
        header->fw_size > slot_max)
    {
        return false;
    }


    memcpy(out, header, sizeof(fw_header_t));

    return true;
}

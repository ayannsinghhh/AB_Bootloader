#include "bootloader.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* Flash layout (STM32F411CEU, 512 KB total):
 *   Sector 0 (16 KB, 0x08000000) \u2500 Bootloader
 *   Sector 1 (16 KB, 0x08004000) \u2500 Bootloader (cont.)
 *   Sector 2 (16 KB, 0x08008000) \u2500 Unused / reserve
 *   Sector 3 (16 KB, 0x0800C000) \u2500 Unused / reserve
 *   Sector 4 (64 KB, 0x08010000) \u2500 Config / boot_state  (CONFIG_FLASH_SECTOR)
 *   Sector 5 (128 KB, 0x08020000) \u2500 Primary slot  (start)
 *   Sector 6 (128 KB, 0x08040000) \u2500 Primary slot  (end)   = 256 KB total
 *   Sector 7 (128 KB, 0x08060000) \u2500 Secondary slot        = 128 KB total
 */
#define PRIMARY_APP_ADDRESS   0x08020000u
#define PRIMARY_SLOT_SIZE     (256u * 1024u) /* sectors 5 + 6 = 256KB */
#define SECONDARY_APP_ADDRESS 0x08060000u
#define SECONDARY_SLOT_SIZE   (128u * 1024u)
#define USER_APPLICATION ((PRIMARY_APP_ADDRESS) + 4)

uint32_t start_address = PRIMARY_APP_ADDRESS; /* default write pointer (primary) */
/* base address of the selected app slot (used for jumping) */
uint32_t app_base_address = PRIMARY_APP_ADDRESS;
#define CONFIG_FLASH_SECTOR   FLASH_SECTOR_4
#define CONFIG_FLASH_ADDRESS  0x08010000

uint8_t bytes_buff[sizeof(frame_format_t)] = {0};
volatile uint8_t bytes_received_count = 0;
frame_format_t receivedFrame;
volatile static bool parse = false;
const size_t FRAME_SIZE = sizeof(frame_format_t);
bootloader_state bootloader_current_state = STATE_IDLE;
frame_format_t ackFrame;
frame_format_t nackFrame;
frame_format_t (*bootloader_state_functions[3])(void);
fw_header_t fw_header;  // Global struct to store firmware info from BL_HEADER

/* If true, the bootloader is waiting for an upload after user selected slot */
volatile bool upload_mode = false;



//volatile uint32_t *myHexWord = (volatile uint32_t *)SECTOR_6;

//---------------------------FUNCTION PROTOTYPES-----------------------------------

static void print(char *msg, ...);
static void jump_to_user_app(void);
void save_bootloader_config(bootloader_config_t *cfg);
bootloader_config_t* load_bootloader_config(void);
static void uart_send_data(uint8_t *data, uint16_t len);
static bool parse_frame(void);
void bootloaderInit(void);
frame_format_t idle_state_func(void);
static void reset_received_frame(void);
static void set_bl_state(bootloader_state state);
static void sendFrame(frame_format_t *frame);
void erase_sectors_for_user_app(uint32_t app_start_address, uint32_t app_size);
uint32_t get_sector(uint32_t address);
frame_format_t updating_state_func(void);
static void write_payload(void);
void erase_sector(void);

/* Boot-state (rollback) helpers */
static boot_state_t load_boot_state(void);
static void save_boot_state(boot_state_t *bs);
static bool verify_image_crc(uint32_t slot_addr, fw_header_t *hdr);

/* Menu helpers */
static void show_boot_menu(uint32_t timeout_ms);
static void display_slot_info(uint8_t slot);
static bool read_fw_header_from_flash(uint32_t addr, fw_header_t *out);


//-------------------------------------------------------------------




void bootloader_main(void){
	bootloaderInit();

	/* ---------------------------------------------------------------
	 * A/B Rollback: load persisted boot state and decide which slot
	 * to boot. If a pending slot is set, try to boot it (up to
	 * MAX_BOOT_ATTEMPTS times). If attempts are exhausted, revert to
	 * the confirmed active slot.
	 * ------------------------------------------------------------- */
	boot_state_t bs = load_boot_state();

	if (bs.pending_slot != 0xFF) {
		/* A new image is pending. Check attempt count. */
		if (bs.boot_attempt >= MAX_BOOT_ATTEMPTS) {
			/* Exceeded limit — rollback to active slot */
			print("[BL] Rollback: pending slot failed after %d attempts, reverting to slot %d\r\n",
				  MAX_BOOT_ATTEMPTS, bs.active_slot);
			bs.pending_slot     = 0xFF;
			bs.boot_attempt     = 0;
			bs.image_confirmed  = 0;
			save_boot_state(&bs);
		} else {
			/* Increment attempt counter and boot the pending slot */
			bs.boot_attempt++;
			save_boot_state(&bs);
			app_base_address = (bs.pending_slot == 0) ? PRIMARY_APP_ADDRESS : SECONDARY_APP_ADDRESS;
			print("[BL] Booting pending slot %d (attempt %d/%d)\r\n",
				  bs.pending_slot, bs.boot_attempt, MAX_BOOT_ATTEMPTS);
			jump_to_user_app();
			/* If jump returns, fall through to menu */
		}
	} else {
		/* No pending slot — boot the confirmed active slot */
		app_base_address = (bs.active_slot == 0) ? PRIMARY_APP_ADDRESS : SECONDARY_APP_ADDRESS;
	}

	/* Show boot menu for 20 seconds before enabling UART RX handling */
	show_boot_menu(20000);

	/* If the menu set upload_mode (user chose option 3/4) then enable RX and
	   process the bootloader state machine until upload completes. This prevents
	   the auto-boot timer from firing immediately after the menu exits. */
	if (upload_mode) {
		USART2->CR1 |= USART_CR1_RXNEIE;
		bootloader_state_functions[STATE_IDLE] = idle_state_func;
		bootloader_state_functions[STATE_UPDATING]= updating_state_func;
		/* process states until upload_mode cleared by BL_UPDATE_DONE handler */
		while (upload_mode) {
			(*bootloader_state_functions[bootloader_current_state])();
			HAL_Delay(10);
		}
		/* once upload completes, BL_UPDATE_DONE handler will have jumped to app */
		/* fall through if we ever return */
	}

	/* enable UART RX interrupt for normal bootloader operation */
	USART2->CR1 |= USART_CR1_RXNEIE;

	uint32_t curr_time = HAL_GetTick();    //current timestamp
	bootloader_state_functions[STATE_IDLE] = idle_state_func;
	bootloader_state_functions[STATE_UPDATING]= updating_state_func;

	while(1){
		(*bootloader_state_functions[bootloader_current_state])();
		/* only auto-boot if we are NOT in upload mode (i.e. user didn't select upload target)
		   when upload_mode is true we keep listening for BL_START_UPDATE / payloads */
		if(((HAL_GetTick()- curr_time) > 5000) && (bootloader_current_state == STATE_IDLE) && (!upload_mode)){
			print("launching user app...\r\n");
			jump_to_user_app();
		}
	}
}


//-------------------------------------------------------------------


void bootloaderInit(void)
{
	// create the ACK frame
	ackFrame.start_of_frame = BL_START_OF_FRAME;
	ackFrame.frame_id = BL_ACK_FRAME;
	ackFrame.payload_len = PAYLOAD_LEN;
	ackFrame.end_of_frame = BL_END_OF_FRAME;
	for (int i = 0; i < PAYLOAD_LEN; i++)
	{
		ackFrame.payload[i] = i;
	}

	// create the NACK frame
	nackFrame.start_of_frame = BL_START_OF_FRAME;
	nackFrame.frame_id = BL_NACK_FRAME;
	nackFrame.payload_len = PAYLOAD_LEN;
	nackFrame.end_of_frame = BL_END_OF_FRAME;
	for (int i = 0; i < PAYLOAD_LEN; i++)
	{
		nackFrame.payload[i] = i;
	}
}


//-------------------------------------------------------------------


// CRC removed: no finalizeFrameCRC function

/* ---------------------------------------------------------------------------
 * Boot State: Save/Load
 * The boot_state_t struct is stored in CONFIG_FLASH_SECTOR (Sector 4).
 * On first boot (magic mismatch) a default state is synthesized: active slot
 * is primary, no pending slot.
 * -------------------------------------------------------------------------*/
static boot_state_t load_boot_state(void)
{
    boot_state_t *p = (boot_state_t *)CONFIG_FLASH_ADDRESS;
    if (p->magic != BOOT_STATE_MAGIC) {
        /* First boot — return safe defaults, nothing persisted yet */
        boot_state_t def;
        def.magic           = BOOT_STATE_MAGIC;
        def.active_slot     = 0;    /* primary */
        def.pending_slot    = 0xFF; /* none */
        def.boot_attempt    = 0;
        def.image_confirmed = 0;
        def.reserved        = 0;
        return def;
    }
    return *p;
}

static void save_boot_state(boot_state_t *bs)
{
    bs->magic = BOOT_STATE_MAGIC;

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = CONFIG_FLASH_SECTOR;
    erase.NbSectors    = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    HAL_FLASHEx_Erase(&erase, &sector_error);

    /* Write boot_state_t word by word */
    uint32_t *data = (uint32_t *)bs;
    uint32_t  addr = CONFIG_FLASH_ADDRESS;
    for (uint32_t i = 0; i < sizeof(boot_state_t) / 4; i++) {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data[i]);
        addr += 4;
    }

    HAL_FLASH_Lock();
}

/* ---------------------------------------------------------------------------
 * verify_image_crc
 * Reads the fw_header_t from slot_addr, then streams the firmware bytes
 * through the hardware CRC unit and compares against hdr->crc32.
 * Returns true if the image is intact.
 * -------------------------------------------------------------------------*/
static bool verify_image_crc(uint32_t slot_addr, fw_header_t *hdr)
{
    if (hdr == NULL || hdr->magic != FW_HEADER_MAGIC) return false;
    if (hdr->fw_size == 0 || hdr->fw_size > (PRIMARY_SLOT_SIZE)) return false;

    /* Image bytes start immediately after the header */
    uint32_t image_start = slot_addr + sizeof(fw_header_t);
    uint32_t word_count  = hdr->fw_size / 4; /* HAL_CRC_Calculate operates on 32-bit words */

    __HAL_CRC_DR_RESET(&hcrc); /* Reset the CRC accumulator */
    uint32_t computed = HAL_CRC_Calculate(&hcrc, (uint32_t *)image_start, word_count);

    return (computed == hdr->crc32);
}

//-------------------------------------------------------------------

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

		// only states above are valid to switch out of idle state
		default:
			set_bl_state(STATE_IDLE);
			reset_received_frame();
		}
	}
	//return (frame_format_t)0;
	return ackFrame;
}


//-------------------------------------------------------------------


frame_format_t updating_state_func(void)
{
    // Once we are updating for sure, we erase required sectors only once
    static bool erased = false;

    if (parse_frame())
    {
        switch (receivedFrame.frame_id)
        {
		case BL_PAYLOAD:
			if (!erased) // only erase once at the beginning
			{
				/* Determine app size to erase. If a header was previously received use it,
				   otherwise fall back to the full slot size for the current start_address. */
				uint32_t app_size = fw_header.fw_size;
				uint32_t slot_max = (start_address == PRIMARY_APP_ADDRESS) ? PRIMARY_SLOT_SIZE : SECONDARY_SLOT_SIZE;
				if (app_size == 0 || app_size == 0xFFFFFFFFu || app_size > slot_max) {
					app_size = slot_max;
				}
				erase_sectors_for_user_app(start_address, app_size);
				erased = true;
			}
			write_payload();
			sendFrame(&ackFrame);
			break;

		case BL_UPDATE_DONE:
		{
			/* --- Image integrity check before committing --- */
			fw_header_t *stored_hdr = (fw_header_t *)app_base_address;
			if (stored_hdr->magic != FW_HEADER_MAGIC) {
				print("[BL] ERROR: No valid firmware header found after write.\r\n");
				sendFrame(&nackFrame);
				erased = false;
				break;
			}
			if (!verify_image_crc(app_base_address, stored_hdr)) {
				print("[BL] ERROR: Image CRC32 mismatch! Firmware rejected.\r\n");
				sendFrame(&nackFrame);
				erased = false;
				break;
			}

			/* --- Persist boot state for rollback --- */
			boot_state_t bs = load_boot_state();
			uint8_t new_slot = (app_base_address == PRIMARY_APP_ADDRESS) ? 0 : 1;
			bs.pending_slot    = new_slot;
			bs.boot_attempt    = 0;
			bs.image_confirmed = 0;
			save_boot_state(&bs);

			print("[BL] Image verified OK. Pending slot %d set. Booting...\r\n", new_slot);
			sendFrame(&ackFrame);
			upload_mode = false;
			erased = false;
			jump_to_user_app();
			break;
		}

        case BL_HEADER:

            memcpy(&fw_header, receivedFrame.payload, sizeof(fw_header));
            sendFrame(&ackFrame);
            break;

        case BL_STATUS_CHECK:

        	sendFrame(&ackFrame);
            break;

        default:

            sendFrame(&nackFrame);  // reject unknown/invalid frame
            break;
        }

        reset_received_frame();
    }

    return ackFrame;
}



//---------------------------------------------------------------------------------------------


static void write_payload(void){
	HAL_FLASH_Unlock();
	for(int i=0; i<16; i+=4){
		uint32_t *val = (uint32_t *) &receivedFrame.payload[i];
		HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD , start_address , *val);
		if (*(uint32_t*)start_address != *val) {
            print("Flash write verification failed at 0x%08lx\r\n", start_address);
            sendFrame(&nackFrame);
            break;
        }
		start_address +=4;
	}
	HAL_FLASH_Lock();

	reset_received_frame();
}


//---------------------------------------------------------------------------------------------

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

//---------------------------------------------------------------------------------------------

static void set_bl_state(bootloader_state state){
	bootloader_current_state = state ;
}

//---------------------------------------------------------------------------------------------

static void jump_to_user_app(void){
	/* Use the selected application base address (app_base_address) */
	uint32_t app_addr = app_base_address;
	uint32_t appStack = *((uint32_t*)app_addr);
	uint32_t appResetHandler = *((uint32_t*)(app_addr + 4));

	print("Read from 0x%08lX: MSP=0x%08lX, ResetHandler=0x%08lX\r\n", app_addr, appStack, appResetHandler);

	/* Validate MSP is within SRAM bounds (128 KB SRAM: 0x20000000–0x20020000) */
	if (appStack < 0x20000000 || appStack > 0x20020000) {
		print("ERROR: Invalid Stack Pointer read from app! Halting.\r\n");
		return;
	}

	/* M1 FIX: Validate Reset Handler against the slot containing app_base_address,
	 * not hardcoded PRIMARY range. Secondary firmware's vectors live in sector 7. */
	uint32_t slot_start = app_addr;
	uint32_t slot_size  = (app_addr == PRIMARY_APP_ADDRESS) ? PRIMARY_SLOT_SIZE : SECONDARY_SLOT_SIZE;
	if (appResetHandler < slot_start || appResetHandler >= (slot_start + slot_size)) {
		print("ERROR: Reset Handler 0x%08lX outside slot [0x%08lX, 0x%08lX). Halting.\r\n",
		      appResetHandler, slot_start, slot_start + slot_size);
		return;
	}

	/* Deinit used peripherals before disabling interrupts */
	HAL_UART_DeInit(&huart2);
	HAL_CRC_DeInit(&hcrc);
	HAL_DeInit();

	/* M2 FIX: Stop SysTick BEFORE disabling IRQs to prevent it firing into
	 * an uninitialized application vector table between VTOR reassignment
	 * and the application's own SysTick setup. */
	SysTick->CTRL = 0;

	/* Disable all interrupts */
	__disable_irq();

	/* Relocate vector table to application */
	SCB->VTOR = app_addr;

	/* Load application stack pointer */
	__set_MSP(appStack);

	/* Jump to application reset handler */
	void (*resetHandler)(void) = (void (*)(void))appResetHandler;
	resetHandler();
}

//---------------------------------------------------------------------------------------------

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
			;
	}

	while (!(USART2->SR & USART_SR_TC))
		;
}


//---------------------------------------------------------------------------------------------



void erase_sector(void)
{
	FLASH_EraseInitTypeDef erase;
	erase.TypeErase = FLASH_TYPEERASE_SECTORS;
	erase.NbSectors = 1;
	erase.Sector = FLASH_SECTOR_5;
	erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
	uint32_t err = 0;

	HAL_FLASH_Unlock();
	HAL_FLASHEx_Erase(&erase, &err);
	HAL_FLASH_Lock();
}

//---------------------------------------------------------------------------------------------


uint32_t get_sector(uint32_t address)
{
    if (address < 0x08004000) return FLASH_SECTOR_0;   // 16 KB
    else if (address < 0x08008000) return FLASH_SECTOR_1; // 16 KB
    else if (address < 0x0800C000) return FLASH_SECTOR_2; // 16 KB
    else if (address < 0x08010000) return FLASH_SECTOR_3; // 16 KB
    else if (address < 0x08020000) return FLASH_SECTOR_4; // 64 KB
    else if (address < 0x08040000) return FLASH_SECTOR_5; // 128 KB
    else if (address < 0x08060000) return FLASH_SECTOR_6; // 128 KB
    else if (address < 0x08080000) return FLASH_SECTOR_7; // 128 KB
    // Add more if your MCU has more sectors
    return FLASH_SECTOR_7; // default fallback
}


//---------------------------------------------------------------------------------------------



void erase_sectors_for_user_app(uint32_t app_start_address, uint32_t app_size)
{
	if (app_size == 0) return;
	uint32_t app_end_address = app_start_address + app_size - 1; /* inclusive end */
	uint32_t first_sector = get_sector(app_start_address);
	uint32_t last_sector  = get_sector(app_end_address);
    uint32_t sector_error = 0;

    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef eraseInit;
    eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
    eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    for(uint32_t sector = first_sector; sector <= last_sector; sector++)
    {
        eraseInit.Sector = sector;
        eraseInit.NbSectors = 1;

        if (HAL_FLASHEx_Erase(&eraseInit, &sector_error) != HAL_OK)
        {
            print("ERROR: Erase failed at sector %d, err code: %lu\r\n", sector, sector_error);
            break;
        }
        else
        {
            print("Sector %d erased successfully\r\n", sector);
        }
    }

    HAL_FLASH_Lock();
}


//---------------------------------------------------------------------------------------------



void save_bootloader_config(bootloader_config_t *cfg)
{
    HAL_FLASH_Unlock();

    // Erase sector before writing
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_error = 0;

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = CONFIG_FLASH_SECTOR;
    erase.NbSectors    = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &sector_error) != HAL_OK) {
        // Handle erase error
    }

    // Write struct word by word (32-bit)
    uint32_t *data = (uint32_t *)cfg;
    uint32_t addr  = CONFIG_FLASH_ADDRESS;

    for (uint32_t i = 0; i < sizeof(bootloader_config_t)/4; i++) {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data[i]);
        addr += 4;
    }

    HAL_FLASH_Lock();
}


//---------------------------------------------------------------------------------------------



bootloader_config_t* load_bootloader_config(void)
{
    return (bootloader_config_t*)CONFIG_FLASH_ADDRESS;
}


//--------------------------------------------------------------------------------------------



static void uart_send_data(uint8_t *data, uint16_t len){
	for(int i=0; i<len ; i++){
		USART2->DR = data[i];
		while(!(USART2->SR & USART_SR_TXE));

	}
	while(!(USART2->SR & USART_SR_TC));
}



//--------------------------------------------------------------------------------------------

//Send the frame thru UART
static void sendFrame(frame_format_t *frame){
	uint8_t * temp = (uint8_t *)frame;
	/* send full frame bytes */
	uart_send_data(temp, sizeof(frame_format_t));
}

//--------------------------------------------------------------------------------------------

static void show_boot_menu(uint32_t timeout_ms)
{
	uint32_t start = HAL_GetTick();
	/* Disable RXNE interrupt so menu input won't be pulled into the frame buffer */
	USART2->CR1 &= ~USART_CR1_RXNEIE;

	bool menu_printed = false;

	while ((HAL_GetTick() - start) < timeout_ms)
	{
		/* print the menu once when entering or after a restart */
		if (!menu_printed) {
			print("\r\n--- Bootloader Menu (%lu ms) ---\r\n", timeout_ms);
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
				/* restart the menu timer after returning from info view */
				start = HAL_GetTick();
				menu_printed = false;
			}
			else if (c == '3' || c == '4')
			{
				/* set upload target and enter upload mode */
				upload_mode = true;
				if (c == '3') {
					start_address = PRIMARY_APP_ADDRESS;
					app_base_address = PRIMARY_APP_ADDRESS;
					memset(&fw_header, 0xFF, sizeof(fw_header));
					print("Upload target set to PRIMARY (0x%08lX).\r\n", start_address);
				} else {
					start_address = SECONDARY_APP_ADDRESS;
					app_base_address = SECONDARY_APP_ADDRESS;
					memset(&fw_header, 0xFF, sizeof(fw_header));
					print("Upload target set to SECONDARY (0x%08lX).\r\n", start_address);
				}
				print("Please upload firmware using the companion GUI \r\n");
				/* re-enable RX now so the bootloader can accept frames immediately */
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

	if (!upload_mode) {
		print("Exiting boot menu...\r\n\r\n");
		/* Re-enable RXNE interrupt for normal bootloader operation */
		USART2->CR1 |= USART_CR1_RXNEIE;
	} else {
		print("Upload mode active - listening for update frames...\r\n");
	}
}

//--------------------------------------------------------------------------------------------


static void display_slot_info(uint8_t slot)
{
	uint32_t addr = 0;
	uint32_t size = 0;
	fw_header_t header;

	if (slot == 1) {
		addr = PRIMARY_APP_ADDRESS;
		size = PRIMARY_SLOT_SIZE;
		print("Primary slot:\r\n");
	} else if (slot == 2) {
		addr = SECONDARY_APP_ADDRESS;
		size = SECONDARY_SLOT_SIZE;
		print("Secondary slot:\r\n");
	} else {
		print("Unknown slot %d\r\n", slot);
		return;
	}

	print("  Start Address: 0x%08lX\r\n", addr);
	print("  Max Size: %lu bytes\r\n", size);

	if (read_fw_header_from_flash(addr, &header)) {
		print("  Stored FW - version: 0x%08lX, size: %lu bytes\r\n", header.fw_version, header.fw_size);
	} else {
		print("  No valid firmware header found in this slot\r\n");
	}
	print("\r\n");
	/* Prompt user to press any key to return to the menu and block until keypress */
	print("Press any key to return to the menu...\r\n");
	/* Wait for a character on USART2 (RXNE) while menu has RX interrupt disabled */
	while (!(USART2->SR & USART_SR_RXNE)) {
		HAL_Delay(10);
	}
	/* read and discard the character so subsequent menu loop doesn't see it */
	(void)USART2->DR;
}

//--------------------------------------------------------------------------------------------

static bool read_fw_header_from_flash(uint32_t addr, fw_header_t *out)
{
	if (out == NULL) return false;

	fw_header_t *p = (fw_header_t *)addr;

	if (p->fw_size == 0xFFFFFFFFu || p->fw_version == 0xFFFFFFFFu) {
		return false;
	}

	/* Determine slot max size based on addr parameter */
	uint32_t slot_max = (addr == PRIMARY_APP_ADDRESS) ? PRIMARY_SLOT_SIZE : SECONDARY_SLOT_SIZE;
	if (p->fw_size == 0 || p->fw_size > slot_max) {
		return false;
	}

	memcpy(out, p, sizeof(fw_header_t));
	return true;
}


//--------------------------------------------------------------------------------------------


void bootloader_USART2_callback(uint8_t data ){

	//fill buffer until we have enough bytes to asseble a frame
	if(bytes_received_count < FRAME_SIZE){
		bytes_buff[bytes_received_count++] = data;
		if(bytes_received_count == FRAME_SIZE){
			bytes_received_count = 0;
			parse = true ;
		}
	}
}



//--------------------------------------------------------------------------------------------


static bool parse_frame(void)
{
	// checks if we have a frame to parse
	if (parse)
	{
		parse = false;
		// assemble a frame from bytes_buff
		memcpy(&receivedFrame, bytes_buff, sizeof(frame_format_t));
		// clear bytes buffer
		memset(bytes_buff, 0, sizeof(frame_format_t));
		// the type of frame we get will dictate what the next state should be
		if (receivedFrame.start_of_frame == BL_START_OF_FRAME && receivedFrame.end_of_frame == BL_END_OF_FRAME)
		{
			/* M3: Per-packet CRC32 check using hardware CRC peripheral.
			 * The STM32F4 CRC unit computes CRC32/MPEG-2 over the 16-byte payload.
			 * The host GUI must compute the same CRC and embed it in frame.crc32. */
			__HAL_CRC_DR_RESET(&hcrc);
			uint32_t calc_crc = HAL_CRC_Calculate(&hcrc,
												  (uint32_t*)receivedFrame.payload,
												  receivedFrame.payload_len / 4);
			if (calc_crc != receivedFrame.crc32) {
				sendFrame(&nackFrame);
				return false; /* corrupted frame — host must retransmit */
			}

			if (receivedFrame.payload_len > PAYLOAD_LEN) {
				sendFrame(&nackFrame);
				return false;
			}

			return true;
		}
	}
	return false;
}




















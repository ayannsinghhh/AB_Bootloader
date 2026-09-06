#ifndef _FRAMES_H_
#define _FRAMES_H_


/**
 *  @brief Frame Format
 *  		---------------------------------------------
 *  		|-SOF-|-FRAME ID-|-LEN-|-DATA-|-CRC-|-EOF-|
 *  BYTES: ----4------4----------2----N------4-----4------
 *
 *  STM32F4 N=16
 */

//delimeters
#define BL_START_OF_FRAME	0X45444459 // EDDY
#define BL_END_OF_FRAME		0X46414952 //FAIR

//frame IDS
#define BL_HEADER       0xFEEDEDDE //sends info about firmware: size , version
#define BL_STATUS_CHECK 0x4b4b4b4b // Status check request/response
#define BL_START_UPDATE 0xBA5EBA11 //Command: start firmware update
#define BL_PAYLOAD      0xDEADBEEF // Contains firmware data chunk
#define BL_UPDATE_DONE  0xDEADDADE // Signals update completion
#define BL_ACK_FRAME    0x45634AED // Bootloader acknowledges frame
#define BL_NACK_FRAME   0x43636AEA // Bootloader rejects frame (error)
#define PAYLOAD_LEN 16

//frame formated struct
typedef struct __attribute__((packed)) // using __attribute__((packed)) to prevent compiler from adding padding so that our data frame remains exactly 34 bytes long
{
    uint32_t start_of_frame;
    uint32_t frame_id;
    uint16_t payload_len;
    uint8_t payload[PAYLOAD_LEN];
    uint32_t crc32;
    uint32_t  end_of_frame;

}frame_format_t;

// Bootloader configuration structure
typedef struct __attribute__((packed , aligned(4)))
{
    uint32_t fw_size;          // Total firmware size in bytes
    uint32_t fw_version;       // Firmware version number
    uint32_t received_bytes;   // Number of bytes received so far
    uint32_t expected_crc;     // Expected CRC of the entire firmware
    uint8_t update_in_progress;// Flag: 1 = update ongoing, 0 = idle
    uint8_t retries;           // Number of retries allowed for failed frames
    uint16_t reserved;         // Padding / reserved for future use
} bootloader_config_t;

/* Firmware header — placed at the very start of each application slot.
 * The CRC32 covers the image bytes that follow the header (i.e., the code).
 * magic must equal FW_HEADER_MAGIC for the header to be considered valid. */
#define FW_HEADER_MAGIC  0xDEADC0DEu

typedef struct __attribute__((packed)) {
    uint32_t magic;       // Must be FW_HEADER_MAGIC
    uint32_t fw_size;     // Total firmware size in bytes (excluding header)
    uint32_t crc32;       // CRC32 of the firmware image (excluding header)
    uint32_t fw_version;  // Firmware version number
} fw_header_t;

/* Boot state persisted in flash (CONFIG_FLASH_SECTOR).
 * Controls the A/B rollback mechanism. */
#define BOOT_STATE_MAGIC 0xB007B007u
#define MAX_BOOT_ATTEMPTS 3u

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic;           // Must be BOOT_STATE_MAGIC
    uint8_t  active_slot;     // 0=primary, 1=secondary — last confirmed good slot
    uint8_t  pending_slot;    // 0=primary, 1=secondary, 0xFF=none pending
    uint8_t  boot_attempt;    // Incremented before each pending-slot boot
    uint8_t  image_confirmed; // Set to 1 by application after successful boot
    uint32_t reserved;        // Future use
} boot_state_t;

//states
typedef enum bootloader_state{
	STATE_IDLE =0,
	STATE_START_UPDATE,
	STATE_UPDATING
}bootloader_state;

typedef enum{
	BL_STATUS_OK = 0,
	BL_STATUS_ERR
}bl_status;

extern frame_format_t(* bootloader_state_functions[3])(void);

#endif

/*
 * Minimal user application for STM32F411 that prints "Hello World" over USART2.
 *
 * Instructions: Create a new STM32CubeIDE project for STM32F411CEU6 and
 * replace the generated `main.c` with this file (or paste the UART init
 * and print logic into your project's main). Ensure USART2 is configured
 * for PA2(TX)/PA3(RX) and the system clock is initialized.
 */

#include "stm32f4xx_hal.h"
#include <string.h>

/*
 * Boot confirmation stub.
 *
 * After the bootloader boots a pending (newly updated) image, it sets
 * boot_state.image_confirmed = 0 and increments boot_attempt on each boot.
 * If boot_attempt reaches MAX_BOOT_ATTEMPTS (3) without confirmation,
 * the bootloader rolls back to the previous slot.
 *
 * The application MUST call boot_confirm() early in startup — after its
 * own self-test — to inform the bootloader that the new firmware is healthy.
 *
 * CONFIG_FLASH_ADDRESS must match the bootloader's CONFIG_FLASH_ADDRESS (0x08010000).
 */
#define BOOT_STATE_MAGIC     0xB007B007u
#define CONFIG_FLASH_ADDRESS 0x08010000u
#define CONFIG_FLASH_SECTOR  FLASH_SECTOR_4

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic;
    uint8_t  active_slot;
    uint8_t  pending_slot;
    uint8_t  boot_attempt;
    uint8_t  image_confirmed;
    uint32_t reserved;
} boot_state_t;

static void boot_confirm(void)
{
    boot_state_t *p = (boot_state_t *)CONFIG_FLASH_ADDRESS;

    /* Only act if a valid boot_state_t is present and not yet confirmed */
    if (p->magic != BOOT_STATE_MAGIC || p->image_confirmed == 1) {
        return;
    }

    /* Read current state into RAM */
    boot_state_t bs = *p;
    bs.image_confirmed = 1;         /* mark as healthy */
    bs.active_slot     = bs.pending_slot; /* promote pending to active */
    bs.pending_slot    = 0xFF;      /* clear pending */
    bs.boot_attempt    = 0;

    /* Re-write the config sector with the updated state */
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector_error = 0;
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = CONFIG_FLASH_SECTOR;
    erase.NbSectors    = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    HAL_FLASHEx_Erase(&erase, &sector_error);

    uint32_t *data = (uint32_t *)&bs;
    uint32_t  addr = CONFIG_FLASH_ADDRESS;
    for (uint32_t i = 0; i < sizeof(boot_state_t) / 4; i++) {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data[i]);
        addr += 4;
    }

    HAL_FLASH_Lock();
}

UART_HandleTypeDef huart2;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    /*
     * BOOT CONFIRMATION — must be called early, after peripheral init.
     * Tells the bootloader this firmware booted successfully.
     * If this is never called and the device resets 3 times, the bootloader
     * will automatically roll back to the previous firmware slot.
     */
    boot_confirm();

    const char *msg = "Hello World from user app!\r\n";

    while (1)
    {
        HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);
        HAL_Delay(1000);
    }
}

/* Minimal UART2 init for PA2/PA3, 115200 8N1 */
static void MX_USART2_UART_Init(void)
{
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);
}

/* Basic GPIO init to ensure PA2/PA3 are configured by CubeMX normally.
   If you paste only the UART init into your project, CubeMX will already
   configure the GPIOs. */
static void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};
    /* PA2 TX */
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* PA3 RX */
    GPIO_InitStruct.Pin = GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

/* Placeholder SystemClock_Config - replace with CubeMX generated one in your project */
void SystemClock_Config(void)
{
    /* In the generated project CubeMX will create a proper clock config.
       This placeholder prevents link errors if you paste everything into
       a project that already has a SystemClock_Config. */
}

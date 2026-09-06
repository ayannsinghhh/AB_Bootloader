# STM32F4 Custom Bootloader 

This project implements a custom, dual-slot bootloader for the STM32F411 microcontroller. It allows for firmware updates over a UART (USART2) interface using a custom serial protocol. A companion Python Tkinter GUI is provided to manage the firmware upload process and interact with the bootloader's menu. 

##  Features  

* **Dual-Slot Firmware:** Manages two separate application slots in flash:
    * **Primary Slot:** 256KB (Sectors 5 + 6) at `0x08020000`
    * **Secondary Slot:** 128KB (Sector 7) at `0x08060000`
* **Interactive Boot Menu:** On startup, a 20-second menu is displayed over UART, allowing the user to:
    * View firmware information (version, size) for each slot.
    * Select the target slot (Primary/Secondary) for a new firmware upload.
* **Auto-Boot:** If no menu option is selected, the bootloader automatically attempts to boot the application in the `PRIMARY_APP_ADDRESS` after a 5-second timeout.
* **Custom Serial Protocol:** A robust, frame-based protocol (`frame_format_t`) is used for all communication, including commands (start update, done) and data (header, payload).
* **Python Companion GUI:** A user-friendly Tkinter application (`companion_gui.py`) that provides:
    * A serial monitor to connect to the board and interact with the boot menu.
    * A file selector for choosing a `.bin` file to upload.
    * A full upload manager that handles the handshake, data transfer, and finalization.
    * A progress bar and detailed logging of the upload process.
* **Safe Booting:** The `jump_to_user_app` function performs sanity checks on the application's Stack Pointer (MSP) and Reset Handler address before jumping, preventing hard faults from a corrupt or empty flash slot.

---


##  Hardware & Software Requirements

### Hardware
* **STM32F411 MCU:** (e.g., STM32 Blackpill Dev board or a custom board).
* **USB-to-UART Adapter:** An adapter (like an FTDI, CP2102) to connect your PC to the MCU's USART2 pins.
    * `PC_TX` -> `MCU_USART2_RX`
    * `PC_RX` -> `MCU_USART2_TX`
* **ST-Link Debugger:** Required *once* to flash the bootloader itself.

### Software
* **STM32CubeIDE:** Or any other ARM-GCC toolchain to build the bootloader and application projects.
* **Python 3:** To run the host GUI.
* **Python Libraries:** Install from the `tools/` directory:
    ```bash
    pip install -r tools/requirements.txt
    ```

--- 
##  How To Build
This system consists of two separate programs that must be built: the **Bootloader** and the **User Application**.

### 1. Build the Bootloader

1.  Open the `bootloader/` project (or the root folder containing `Bootloader.ioc`) in STM32CubeIDE.
2.  The linker script `STM32F411CEUX_FLASH.ld` is already configured to place the bootloader at the start of flash (`0x08000000`).
3.  Build the project.
4.  Using an ST-Link debugger, **flash this bootloader `.elf` or `.bin` file to your MCU**. This only needs to be done once.

### 2. Build the User Application

1.  Open the `user_app/` project in a *separate* STM32CubeIDE window.
2.  **Crucial Step:** You **must** modify this project's linker script (e.g., `STM32F411CEUX_FLASH.ld` inside the `user_app` project) to set the application's origin address.
    * Change the `FLASH` origin from `0x08000000` to `0x08020000` (the `PRIMARY_APP_ADDRESS`).

    **Before:**
    ```ld
    MEMORY
    {
      RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
      FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 512K
    }
    ```
    **After:**
    ```ld
    MEMORY
    {
      RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
      FLASH (rx) : ORIGIN = 0x08020000, LENGTH = 256K /* Set to Primary Slot */
    }
    ```
3.  Build the project. This will produce the `user_app.bin` file that you will upload with the GUI.

---

## Firmware Upload Workflow  

1.  Connect your PC to the STM32's USART2 pins using your USB-to-UART adapter.
2.  Run the companion GUI:
    ```bash
    python tools/bootloader_gui.py
    ```
3.  In the GUI:
    * Select the correct COM port for your adapter.
    * Set the baudrate to `115200`.
    * Click **Connect**.
4.  Press the **RESET** button on your STM32 board.
5.  The "Serial Monitor" in the GUI will display the bootloader menu:
    ```
    --- Bootloader Menu (20000 ms) ---
    1: Primary firmware (press 1 to show info)
    ...
    3: Select primary slot as upload target
    4: Select secondary slot as upload target
    ...
    ```
6.  To start an upload, type `3` (for Primary) or `4` (for Secondary) into the "Send" box at the bottom of the monitor and press Enter.
7.  The bootloader will respond, confirming your choice (e.g., "Upload target set to PRIMARY...").
8.  In the "3. Firmware Settings" section of the GUI:
    * Click **Browse** and select the `user_app.bin` file you built earlier.
    * Enter a firmware version (e.g., `0x00000001`).
9.  Click the **Upload Firmware** button.
10. The progress bar will activate. **Note:** The upload will pause for several seconds after the first payload (`"Flash erase in progress..."`). This is **normal** as the bootloader is erasing the flash sectors.
11. When the upload completes, the log will show "Firmware upload completed successfully!"
12. The bootloader will automatically jump to your newly uploaded application. The "Serial Monitor" will re-activate and begin displaying any `printf` output from your `user_app`.

---

## Custom Protocol Overview

The communication is based on a fixed-size 34-byte frame.

**Frame Structure:**
| Field | Offset (bytes) | Length (bytes) | Description |
| :--- | :--- | :--- | :--- |
| `sof` | 0 | 4 | Start of Frame (`0x45444459`) |
| `frame_id` | 4 | 4 | Command/Data ID (e.g., `BL_PAYLOAD`) |
| `payload_len` | 8 | 2 | Length of data in payload (always 16) |
| `payload` | 10 | 16 | Data (firmware bytes, header info) |
| `crc32` | 26 | 4 | CRC32 of the payload |
| `eof` | 30 | 4 | End of Frame (`0x46414952`) |

**Key Frame IDs:**
* `BL_START_UPDATE (0xBA5EBA11)`: Host -> BL (Start session)
* `BL_HEADER (0xFEEDEDDE)`: Host -> BL (Send FW size/version)
* `BL_PAYLOAD (0xDEADBEEF)`: Host -> BL (Send 16 bytes of FW)
* `BL_UPDATE_DONE (0xDEADDADE)`: Host -> BL (Finalize)
* `BL_ACK_FRAME (0x45634AED)`: BL -> Host (Command OK)
* `BL_NACK_FRAME (0x43636AEA)`: BL -> Host (Command Failed)
## Authors

- [@madhavs004](https://www.github.com/madhavs004)


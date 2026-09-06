Testing the bootloader with a simple "Hello World" user application

This guide assumes:
- Target MCU: STM32F411CEU6 (your board)
- You have STM32CubeIDE or another toolchain to build the user application
- You have Tera Term installed on your PC
- You have Python 3 and pyserial installed to run the uploader script (optional)

Summary of steps:
1. Create a simple user application that prints "Hello World" on USART2.
   - I provided `main.c` in this folder. Create a new CubeIDE project for
     STM32F411CEU6 and paste this `main.c` into the project (or copy the
     UART init and transmit code into your generated `main.c`).
   - Ensure USART2 is configured on PA2 (TX) and PA3 (RX) and set to 115200,8,N,1.
   - Build the project and generate a `.bin` file (Project -> Build; .bin appears
     in `Debug/` or `Release/`).

2. Prepare the PC uploader script
   - I included `tools/send_firmware.py` in this workspace. It takes a binary and
     sends it to the bootloader using the frame protocol your bootloader expects.
   - Install Python 3 and pyserial: `pip install pyserial`

3. Use the bootloader menu to select upload target
   - Connect the board to PC over USB-to-UART (the UART the bootloader uses —
     ensure it is connected to the same COM port you will use in Tera Term).
   - Open Tera Term and select the appropriate COM port and set baud to 115200.
   - Reset the board. The bootloader menu will display for 5 seconds.
   - Press `3` to select the primary slot as upload target (you should see a confirmation).

4. Upload firmware using the Python script
   - Example:
     ```
     python tools/send_firmware.py --port COM3 --baud 115200 --file path/to/your_app.bin --version 0x00010001
     ```
   - The script sends a BL_HEADER frame first (containing fw_size and fw_version)
     then sends BL_PAYLOAD frames with 16-byte payloads until the whole binary
     is transmitted, then sends BL_UPDATE_DONE. The bootloader will ACK each
     payload; the script prints progress.

5. After upload completes
   - The bootloader will ACK the BL_UPDATE_DONE and (in your current code)
     immediately jump to the user application. Tera Term should then start
     receiving "Hello World" messages every second.

If you hit problems with CRC mismatches, see the uploader script notes — the
bootloader uses the MCU CRC peripheral; the script uses Python's binascii.crc32.
If they don't match on your hardware, we can either adjust the script or relax
the bootloader's CRC check for a quick test.

#!/usr/bin/env python3
"""
Simple Tkinter GUI firmware uploader for the custom bootloader frame protocol.
Sends BL_HEADER, BL_START_UPDATE, BL_PAYLOAD frames (16-byte payload), then BL_UPDATE_DONE.

Usage: python uploader_gui.py

Dependencies:
  pip install pyserial

This GUI mirrors the behavior of tools/send_firmware.py but with a file chooser,
COM selection, baud selection, progress, and logging window.
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import struct
import serial
import serial.tools.list_ports
import threading
import time
import os
import subprocess
import zlib

# Frame constants (must match frames.h)
BL_START_OF_FRAME = 0x45444459
BL_END_OF_FRAME   = 0x46414952
BL_HEADER         = 0xFEEDEDDE
BL_STATUS_CHECK   = 0x4b4b4b4b
BL_START_UPDATE   = 0xBA5EBA11
BL_PAYLOAD        = 0xDEADBEEF
BL_UPDATE_DONE    = 0xDEADDADE
BL_ACK_FRAME      = 0x45634AED
BL_NACK_FRAME     = 0x43636AEA
PAYLOAD_LEN = 16

FRAME_STRUCT = '<I I H 16s I I'  # start, id, len, payload[16], crc, end
FRAME_SIZE = struct.calcsize(FRAME_STRUCT)


import binascii


# -----------------------------------------------------------
# CRC32 compatible with STM32F4 hardware CRC peripheral
#
# The STM32F4 CRC unit computes CRC32/MPEG-2:
#   Poly=0x04C11DB7, Init=0xFFFFFFFF, RefIn=False, RefOut=False, XorOut=0x00000000
# Python's zlib.crc32 uses CRC32/ISO-HDLC (RefIn=True, RefOut=True)
# so we cannot use it directly. We must use a software implementation.
# -----------------------------------------------------------

def _reflect32(v):
    result = 0
    for _ in range(32):
        result = (result << 1) | (v & 1)
        v >>= 1
    return result & 0xFFFFFFFF


def stm32_crc32(data_bytes):
    """Compute CRC32/MPEG-2 as the STM32F4 hardware CRC unit does.

    The peripheral operates on 32-bit words in big-endian order.
    data_bytes must be a multiple of 4 bytes.
    """
    poly = 0x04C11DB7
    crc  = 0xFFFFFFFF
    # Process 4 bytes at a time (word-by-word, big-endian)
    for i in range(0, len(data_bytes), 4):
        word = struct.unpack('>I', data_bytes[i:i+4])[0]
        crc ^= word
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ poly) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


# Firmware header magic — must match FW_HEADER_MAGIC in frames.h
FW_HEADER_MAGIC = 0xDEADC0DE
FW_HEADER_STRUCT = '<I I I I'   # magic, fw_size, crc32, fw_version
FW_HEADER_SIZE   = struct.calcsize(FW_HEADER_STRUCT)  # 16 bytes


def make_frame(frame_id, payload_bytes):
    """Build a 34-byte frame with per-packet CRC32 (STM32 MPEG-2 variant)."""
    payload_bytes = payload_bytes[:PAYLOAD_LEN]
    # Pad payload to exactly PAYLOAD_LEN bytes
    payload = payload_bytes.ljust(PAYLOAD_LEN, b'\xFF')
    # Compute CRC over the 16-byte payload (4 words)
    crc = stm32_crc32(payload)
    return struct.pack(FRAME_STRUCT, BL_START_OF_FRAME, frame_id, len(payload_bytes), payload, crc, BL_END_OF_FRAME)


class UploaderGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('STM Custom Bootloader Uploader')
        self.geometry('700x420')

        self.filepath = tk.StringVar()
        self.port = tk.StringVar()
        self.baud = tk.IntVar(value=115200)
        self.version = tk.StringVar(value='0x00010000')
        self.log_lines = tk.StringVar(value='')

        self.create_widgets()

    def create_widgets(self):
        frm = ttk.Frame(self, padding=10)
        frm.pack(fill=tk.BOTH, expand=True)

        # file chooser
        file_frame = ttk.Frame(frm)
        file_frame.pack(fill=tk.X)
        ttk.Label(file_frame, text='Firmware (.bin):').pack(side=tk.LEFT)
        ttk.Entry(file_frame, textvariable=self.filepath, width=60).pack(side=tk.LEFT, padx=6)
        ttk.Button(file_frame, text='Browse...', command=self.browse_file).pack(side=tk.LEFT)

        # port/baud/version
        cfg_frame = ttk.Frame(frm)
        cfg_frame.pack(fill=tk.X, pady=8)
        ttk.Label(cfg_frame, text='Port:').pack(side=tk.LEFT)
        self.combobox_port = ttk.Combobox(cfg_frame, textvariable=self.port, width=12, values=())
        self.combobox_port.pack(side=tk.LEFT)
        ttk.Button(cfg_frame, text='Refresh', command=self.refresh_ports).pack(side=tk.LEFT, padx=(6,8))
        ttk.Label(cfg_frame, text='Baud:').pack(side=tk.LEFT, padx=(8,0))
        ttk.Entry(cfg_frame, textvariable=self.baud, width=10).pack(side=tk.LEFT)
        ttk.Label(cfg_frame, text='FW version:').pack(side=tk.LEFT, padx=(8,0))
        ttk.Entry(cfg_frame, textvariable=self.version, width=12).pack(side=tk.LEFT)

        # populate ports list initially
        self.after(100, self.refresh_ports)
        # buttons
        btn_frame = ttk.Frame(frm)
        btn_frame.pack(fill=tk.X, pady=6)
        self.upload_btn = ttk.Button(btn_frame, text='Upload', command=self.start_upload)
        self.upload_btn.pack(side=tk.LEFT)
        ttk.Button(btn_frame, text='Open Tera Term', command=self.open_teraterm).pack(side=tk.LEFT, padx=6)

        # progress and log
        prog_frame = ttk.Frame(frm)
        prog_frame.pack(fill=tk.BOTH, expand=True, pady=(8,0))
        self.progress = ttk.Progressbar(prog_frame, length=600)
        self.progress.pack(fill=tk.X, padx=4, pady=4)

        log_label = ttk.Label(prog_frame, text='Log:')
        log_label.pack(anchor=tk.W)
        self.log = tk.Text(prog_frame, height=12)
        self.log.pack(fill=tk.BOTH, expand=True)
        self.log.configure(state=tk.DISABLED)

    def browse_file(self):
        fname = filedialog.askopenfilename(filetypes=[('Binary files', '*.bin'), ('All files', '*.*')])
        if fname:
            self.filepath.set(fname)

    def refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if not ports:
            ports = []
        self.combobox_port['values'] = ports
        if ports and not self.port.get():
            self.port.set(ports[0])

    def open_teraterm(self):
        """
        Try to find ttermpro.exe in common install locations. If not found, ask the user to locate it.
        Launch Tera Term with the selected COM port and baud.
        """
        port = self.port.get()
        baud = str(self.baud.get())

        possible_paths = [
            r"C:\Program Files (x86)\teraterm\ttermpro.exe",
            r"C:\Program Files\teraterm\ttermpro.exe",
            r"C:\Program Files\teraterm\ttermpro64.exe",
        ]

        teraterm_exe = None
        for p in possible_paths:
            if os.path.exists(p):
                teraterm_exe = p
                break

        if teraterm_exe is None:
            # Ask user to locate ttermpro.exe
            messagebox.showinfo('Tera Term not found', 'Tera Term executable not found in common locations. Please locate ttermpro.exe.')
            exe = filedialog.askopenfilename(title='Locate ttermpro.exe', filetypes=[('ttermpro', 'ttermpro.exe'), ('All files', '*.*')])
            if not exe:
                self.log_msg('Tera Term launch cancelled by user')
                return
            teraterm_exe = exe

        # Build command line options for Tera Term - common syntax: /C=COMx /BAUD=baud
        cmd = [teraterm_exe, '/C=' + port, '/BAUD=' + baud]
        try:
            subprocess.Popen(cmd)
            self.log_msg('Launched Tera Term: %s %s %s' % (teraterm_exe, port, baud))
        except Exception as e:
            self.log_msg('Failed to launch Tera Term: %s' % e)
            messagebox.showerror('Error', 'Failed to launch Tera Term: %s' % e)

    def log_msg(self, s):
        self.log.configure(state=tk.NORMAL)
        self.log.insert(tk.END, s + '\n')
        self.log.see(tk.END)
        self.log.configure(state=tk.DISABLED)

    def start_upload(self):
        if not os.path.exists(self.filepath.get()):
            messagebox.showerror('Error', 'Firmware file not found')
            return
        try:
            ver = int(self.version.get(), 0)
        except Exception:
            messagebox.showerror('Error', 'Invalid version (use hex like 0x00010001 or decimal)')
            return

        self.upload_btn.configure(state=tk.DISABLED)
        t = threading.Thread(target=self.upload_worker, args=(self.filepath.get(), self.port.get(), int(self.baud.get()), ver))
        t.daemon = True
        t.start()

    def upload_worker(self, filepath, port, baud, version):
        try:
            with open(filepath, 'rb') as f:
                raw_image = f.read()
        except Exception as e:
            self.log_msg('Failed to open file: %s' % e)
            self.upload_btn.configure(state=tk.NORMAL)
            return

        # --- Prepend firmware header (fw_header_t) ---
        # The image CRC32 is computed over the raw firmware bytes (not including header)
        # Pad image to a multiple of 4 bytes (required by STM32 CRC hardware)
        padded_image = raw_image
        if len(padded_image) % 4 != 0:
            padded_image += b'\xFF' * (4 - len(padded_image) % 4)

        image_crc = stm32_crc32(padded_image)
        fw_size   = len(padded_image)

        # Build fw_header_t:  magic | fw_size | crc32 | fw_version
        header_bytes = struct.pack(FW_HEADER_STRUCT, FW_HEADER_MAGIC, fw_size, image_crc, version)

        # Full data to upload = header + image
        data = header_bytes + padded_image

        self.log_msg('Firmware: raw=%d bytes, padded=%d bytes, version=0x%08X' % (len(raw_image), fw_size, version))
        self.log_msg('Image CRC32 (STM32/MPEG-2): 0x%08X' % image_crc)
        self.log_msg('Header prepended (%d bytes). Total upload size: %d bytes' % (FW_HEADER_SIZE, len(data)))

        try:
            ser = serial.Serial(port, baud, timeout=0.5)
            time.sleep(0.1)
        except Exception as e:
            # If access denied, check if Tera Term is running and offer to close it
            errstr = str(e)
            self.log_msg('Failed to open serial port: %s' % errstr)
            # Only attempt the Tera Term helper on Windows
            if 'PermissionError' in errstr or 'Access is denied' in errstr or isinstance(e, serial.SerialException):
                try:
                    # look for common Tera Term executables via tasklist
                    proc = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ttermpro.exe'], capture_output=True, text=True)
                    if 'ttermpro.exe' in proc.stdout.lower():
                        # ask user whether to close Tera Term automatically
                        do_close = messagebox.askyesno('Port busy', 'COM port %s appears to be held by Tera Term. Allow the uploader to close Tera Term so it can use the port?' % port)
                        if do_close:
                            try:
                                subprocess.run(['taskkill', '/IM', 'ttermpro.exe', '/F'], check=True)
                                self.log_msg('Closed Tera Term process to free %s' % port)
                                time.sleep(0.3)
                                ser = serial.Serial(port, baud, timeout=0.5)
                                time.sleep(0.1)
                            except Exception as e2:
                                self.log_msg('Failed to close Tera Term or reopen port: %s' % e2)
                                self.upload_btn.configure(state=tk.NORMAL)
                                return
                        else:
                            self.log_msg('Upload cancelled because port is in use. Close Tera Term or choose another port.')
                            self.upload_btn.configure(state=tk.NORMAL)
                            return
                except Exception:
                    # Fall through to normal error handling
                    pass
            self.upload_btn.configure(state=tk.NORMAL)
            return

        # Drain any unsolicited output from the bootloader (e.g., diagnostic prints)
        try:
            ser.timeout = 0.1
            pending = ser.read(2048)
            if pending:
                try:
                    txt = pending.decode('utf-8', errors='replace')
                    self.log_msg('Bootloader output: %s' % txt.strip())
                except Exception:
                    self.log_msg('Bootloader output (hex): %s' % pending.hex())
        except Exception:
            pass

        def send_and_wait_ack(frame, timeout=2.0):
            try:
                ser.write(frame)
                ser.flush()
            except Exception as e:
                self.log_msg('Serial write error: %s' % e)
                return False, None
            ser.timeout = timeout
            try:
                resp = ser.read(FRAME_SIZE)
                if len(resp) != FRAME_SIZE:
                    # try to decode partial response as ascii (bootloader debug prints)
                    try:
                        txt = resp.decode('utf-8', errors='replace')
                        self.log_msg('Partial response (len=%d): %s' % (len(resp), txt.strip()))
                    except Exception:
                        self.log_msg('Partial response (len=%d) hex=%s' % (len(resp), resp.hex()))
                    return False, None
                s_of, fid, plen, payload, crc, eof = struct.unpack(FRAME_STRUCT, resp)
                return True, fid
            except Exception as e:
                self.log_msg('Exception while waiting for ack: %s' % e)
                return False, None

        # Send BL_HEADER — contains fw_size and version for erase calculation
        # Pack: fw_size (4) + fw_version (4) = 8 bytes, fits in 16-byte payload
        header_payload = struct.pack('<I I', fw_size, version)
        frame = make_frame(BL_HEADER, header_payload)
        self.log_msg('Sending BL_HEADER (fw_size=%d, version=0x%08X)...' % (fw_size, version))
        ok, fid = send_and_wait_ack(frame)
        self.log_msg('Header send result: %s, resp id=%s' % (ok, hex(fid) if fid else str(fid)))
        if not ok:
            self.log_msg('Header not acked, aborting')
            ser.close()
            self.upload_btn.configure(state=tk.NORMAL)
            return

        # Send BL_START_UPDATE
        start_frame = make_frame(BL_START_UPDATE, b'')
        self.log_msg('Sending BL_START_UPDATE...')
        ok, fid = send_and_wait_ack(start_frame)
        self.log_msg('Start update result: %s, resp id=%s' % (ok, hex(fid) if fid else str(fid)))
        if not ok:
            self.log_msg('Start update not acked, aborting')
            ser.close()
            self.upload_btn.configure(state=tk.NORMAL)
            return

        # Send payloads
        offset = 0
        sent = 0
        self.progress['maximum'] = fw_size
        while offset < fw_size:
            chunk = data[offset:offset+PAYLOAD_LEN]
            payload_padded = chunk.ljust(PAYLOAD_LEN, b'\xFF')
            self.log_msg('Sending BL_PAYLOAD offset=%d len=%d payload=%s' % (offset, len(chunk), payload_padded.hex()))
            frame = make_frame(BL_PAYLOAD, chunk)
            ok, fid = send_and_wait_ack(frame)
            if not ok:
                self.log_msg('Failed to send payload at offset %d' % offset)
                break
            if fid == BL_NACK_FRAME:
                self.log_msg('NACK received for chunk at %d' % offset)
                break
            offset += PAYLOAD_LEN
            sent += len(chunk)
            self.progress['value'] = sent
            self.log_msg('Sent %d/%d bytes' % (sent, fw_size))
            time.sleep(0.01)

        # Send BL_UPDATE_DONE
        done_frame = make_frame(BL_UPDATE_DONE, b'')
        self.log_msg('Sending BL_UPDATE_DONE...')
        ok, fid = send_and_wait_ack(done_frame)
        self.log_msg('Update done result: %s, resp id=%s' % (ok, hex(fid) if fid else str(fid)))

        ser.close()
        self.upload_btn.configure(state=tk.NORMAL)


if __name__ == '__main__':
    app = UploaderGUI()
    app.mainloop()

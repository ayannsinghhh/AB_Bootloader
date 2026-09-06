#!/usr/bin/env python3
"""
STM32F411 Custom Bootloader GUI with Integrated Serial Monitor
Firmware upload utility for custom bootloader protocol
"""

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext
import serial
import serial.tools.list_ports
import struct
import threading
import time
import os
from datetime import datetime
from typing import Optional, Tuple
import queue

# Frame IDs from your bootloader
BL_START_OF_FRAME = 0x45444459  # EDDY
BL_END_OF_FRAME = 0x46414952    # FAIR
BL_HEADER = 0xFEEDEDDE
BL_STATUS_CHECK = 0x4B4B4B4B
BL_START_UPDATE = 0xBA5EBA11
BL_PAYLOAD = 0xDEADBEEF
BL_UPDATE_DONE = 0xDEADDADE
BL_ACK_FRAME = 0x45634AED
BL_NACK_FRAME = 0x43636AEA

PAYLOAD_LEN = 16
FRAME_SIZE = 34  # 4+4+2+16+4+4 bytes

class BootloaderFrame:
    """Class to handle frame creation and parsing"""

    @staticmethod
    def create_frame(frame_id: int, payload: bytes) -> bytes:
        """Create a frame according to the bootloader protocol"""
        if len(payload) < PAYLOAD_LEN:
            payload = payload + b'\x00' * (PAYLOAD_LEN - len(payload))
        elif len(payload) > PAYLOAD_LEN:
            payload = payload[:PAYLOAD_LEN]
        
        frame = struct.pack('<I', BL_START_OF_FRAME)
        frame += struct.pack('<I', frame_id)
        frame += struct.pack('<H', PAYLOAD_LEN)
        frame += payload
        frame += struct.pack('<I', 0)  # CRC32 (0 since bootloader has CRC disabled)
        frame += struct.pack('<I', BL_END_OF_FRAME)
        return frame

    @staticmethod
    def parse_frame(data: bytes) -> Optional[Tuple[int, bytes]]:
        """Parse received frame data"""
        if len(data) != FRAME_SIZE:
            return None
        try:
            sof, frame_id, payload_len, payload, crc32, eof = struct.unpack('<IIH16sII', data)
            if sof == BL_START_OF_FRAME and eof == BL_END_OF_FRAME:
                return (frame_id, payload[:payload_len])
        except Exception:
            return None
        return None

class FirmwareUploader:
    """Handles the firmware upload process using an existing serial connection"""
    
    def __init__(self, serial_conn: serial.Serial):
        self.serial_conn = serial_conn
        self.firmware_data = b''
        self.firmware_size = 0
        self.bytes_sent = 0
        self.upload_active = False
        self.debug_mode = False

    def send_frame(self, frame: bytes, timeout: float = 2.0, is_first_payload: bool = False) -> bool:
        """Send frame and wait for ACK/NACK"""
        if not self.serial_conn or not self.serial_conn.is_open:
            return False
        
        try:
            if is_first_payload:
                timeout = 10.0  # 10 seconds for flash erase operation
            
            self.serial_conn.reset_input_buffer()
            
            old_timeout = self.serial_conn.timeout
            self.serial_conn.timeout = timeout
            
            if self.debug_mode:
                frame_id = struct.unpack('<I', frame[4:8])[0]
                print(f"Sending frame ID: 0x{frame_id:08X}, size: {len(frame)} bytes")
            
            self.serial_conn.write(frame)
            self.serial_conn.flush()
            
            response = self.serial_conn.read(FRAME_SIZE)
            
            self.serial_conn.timeout = old_timeout
            
            if self.debug_mode:
                print(f"Response received: {len(response)} bytes")
                if len(response) > 0:
                    print(f"Response hex: {response[:20].hex()}")

            if len(response) == FRAME_SIZE:
                parsed = BootloaderFrame.parse_frame(response)
                if parsed:
                    frame_id, _ = parsed
                    if self.debug_mode:
                        print(f"Parsed frame ID: 0x{frame_id:08X}")
                    return frame_id == BL_ACK_FRAME
            
            return False
        except Exception as e:
            print(f"Send frame error: {e}")
            return False

    def upload_firmware(self, firmware_path: str, version: int, progress_callback=None, log_callback=None):
        """Upload firmware to the bootloader"""
        self.upload_active = True
        try:
            with open(firmware_path, 'rb') as f:
                self.firmware_data = f.read()
            
            self.firmware_size = len(self.firmware_data)
            self.bytes_sent = 0
            
            if log_callback: log_callback(f"Firmware size: {self.firmware_size} bytes")
            
            # Send START_UPDATE command
            if log_callback: log_callback("Sending START_UPDATE command...")
            frame = BootloaderFrame.create_frame(BL_START_UPDATE, b'')
            if not self.send_frame(frame, timeout=5.0):
                if log_callback: log_callback("ERROR: Failed to start update. Ensure bootloader is in upload mode (selected via menu).")
                return False
            
            if log_callback: log_callback("Update started successfully")
            
            # Send HEADER frame
            if log_callback: log_callback("Sending firmware header...")
            header_data = struct.pack('<II', self.firmware_size, version) + b'\x00' * 8
            frame = BootloaderFrame.create_frame(BL_HEADER, header_data)
            if not self.send_frame(frame):
                if log_callback: log_callback("ERROR: Failed to send header")
                return False
            
            if log_callback: log_callback("Header sent successfully")
            
            # Send firmware data in chunks
            if log_callback: log_callback("Uploading firmware...")
            
            chunk_num = 0
            first_payload = True
            
            while self.bytes_sent < self.firmware_size and self.upload_active:
                chunk = self.firmware_data[self.bytes_sent:self.bytes_sent + PAYLOAD_LEN]
                
                frame = BootloaderFrame.create_frame(BL_PAYLOAD, chunk)
                
                retry_count = 0
                max_retries = 3
                success = False
                
                if first_payload and log_callback:
                    log_callback("First payload - flash erase in progress (this may take a few seconds)...")
                
                while retry_count < max_retries and not success:
                    if self.send_frame(frame, is_first_payload=first_payload):
                        success = True
                        # Accurately count bytes sent, especially for the last chunk
                        actual_chunk_len = len(self.firmware_data[self.bytes_sent:self.bytes_sent + PAYLOAD_LEN])
                        self.bytes_sent += actual_chunk_len
                        chunk_num += 1
                        
                        if first_payload:
                            first_payload = False
                            if log_callback: log_callback("Flash erase complete, continuing upload...")
                        
                        if progress_callback:
                            progress = (self.bytes_sent / self.firmware_size) * 100
                            progress_callback(progress, self.bytes_sent, self.firmware_size)
                        
                    else:
                        retry_count += 1
                        if log_callback:
                            log_callback(f"Retry {retry_count}/{max_retries} for chunk at offset {self.bytes_sent}")
                        time.sleep(0.5)
                
                if not success:
                    if log_callback: log_callback(f"ERROR: Failed to send chunk at offset {self.bytes_sent}")
                    return False
            
            if not self.upload_active:
                if log_callback: log_callback("Upload cancelled by user")
                return False
            
            # Send UPDATE_DONE command
            if log_callback: log_callback("Finalizing update...")
            frame = BootloaderFrame.create_frame(BL_UPDATE_DONE, b'')
            if not self.send_frame(frame):
                if log_callback: log_callback("ERROR: Failed to finalize update")
                return False
            
            if log_callback:
                log_callback("Firmware upload completed successfully!")
                log_callback("Bootloader will now jump to the new firmware.")
            return True
        except Exception as e:
            if log_callback: log_callback(f"ERROR: {str(e)}")
            return False
        finally:
            self.upload_active = False

    def cancel_upload(self):
        self.upload_active = False

class BootloaderGUI:
    """Main GUI application"""
    
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("STM32F411 Bootloader GUI")
        self.root.geometry("850x700")
        
        style = ttk.Style()
        style.theme_use('clam')
        
        # Serial connection state
        self.serial_conn = None
        self.serial_thread = None
        self.stop_thread = threading.Event()
        self.serial_queue = queue.Queue()
        
        self.uploader = None
        self.upload_thread = None
        self.log_queue = queue.Queue()
        
        self.setup_ui()
        self.refresh_ports()
        
        self.root.after(100, self.process_log_queue)
        self.root.after(100, self.process_serial_queue)
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

    def setup_ui(self):
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)

        # --- Connection Frame ---
        conn_frame = ttk.LabelFrame(main_frame, text="1. Connection Settings", padding="10")
        conn_frame.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        
        ttk.Label(conn_frame, text="Serial Port:").grid(row=0, column=0, sticky=tk.W, padx=5)
        self.port_combo = ttk.Combobox(conn_frame, width=20, state='readonly')
        self.port_combo.grid(row=0, column=1, padx=5)
        self.refresh_btn = ttk.Button(conn_frame, text="Refresh", command=self.refresh_ports)
        self.refresh_btn.grid(row=0, column=2, padx=5)
        
        ttk.Label(conn_frame, text="Baudrate:").grid(row=0, column=3, sticky=tk.W, padx=5)
        self.baud_combo = ttk.Combobox(conn_frame, width=15, values=["115200", "57600", "9600"], state='readonly')
        self.baud_combo.set("115200")
        self.baud_combo.grid(row=0, column=4, padx=5)
        
        self.connect_btn = ttk.Button(conn_frame, text="Connect", command=self.connect_serial)
        self.connect_btn.grid(row=0, column=5, padx=10)
        self.disconnect_btn = ttk.Button(conn_frame, text="Disconnect", command=self.disconnect_serial, state=tk.DISABLED)
        self.disconnect_btn.grid(row=0, column=6, padx=5)

        # --- Serial Monitor Frame ---
        monitor_frame = ttk.LabelFrame(main_frame, text="2. Serial Monitor (for Bootloader Menu)", padding="10")
        monitor_frame.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S), pady=5)
        monitor_frame.columnconfigure(0, weight=1)
        monitor_frame.rowconfigure(0, weight=1)
        
        self.monitor_text = scrolledtext.ScrolledText(monitor_frame, height=10, width=80, wrap=tk.WORD, state=tk.DISABLED)
        self.monitor_text.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        self.send_entry = ttk.Entry(monitor_frame, width=70, state=tk.DISABLED)
        self.send_entry.grid(row=1, column=0, pady=5, sticky=(tk.W, tk.E))
        self.send_entry.bind("<Return>", self.send_serial_data_event)
        self.send_btn = ttk.Button(monitor_frame, text="Send", command=self.send_serial_data, state=tk.DISABLED)
        self.send_btn.grid(row=1, column=1, pady=5, padx=5)

        # --- Firmware Frame ---
        fw_frame = ttk.LabelFrame(main_frame, text="3. Firmware Settings", padding="10")
        fw_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        fw_frame.columnconfigure(1, weight=1)
        
        ttk.Label(fw_frame, text="Firmware File:").grid(row=0, column=0, sticky=tk.W, padx=5)
        self.file_entry = ttk.Entry(fw_frame)
        self.file_entry.grid(row=0, column=1, sticky=(tk.W, tk.E), padx=5)
        self.browse_btn = ttk.Button(fw_frame, text="Browse", command=self.browse_file)
        self.browse_btn.grid(row=0, column=2, padx=5)
        
        ttk.Label(fw_frame, text="Version (hex):").grid(row=1, column=0, sticky=tk.W, padx=5, pady=5)
        self.version_entry = ttk.Entry(fw_frame, width=20)
        self.version_entry.insert(0, "0x00000001")
        self.version_entry.grid(row=1, column=1, sticky=tk.W, padx=5, pady=5)

        # --- Progress Frame ---
        prog_frame = ttk.LabelFrame(main_frame, text="4. Upload", padding="10")
        prog_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        prog_frame.columnconfigure(0, weight=1)
        
        self.progress_bar = ttk.Progressbar(prog_frame, mode='determinate')
        self.progress_bar.grid(row=0, column=0, sticky=(tk.W, tk.E), padx=5, pady=5)
        self.progress_label = ttk.Label(prog_frame, text="Ready")
        self.progress_label.grid(row=1, column=0, padx=5, sticky=tk.W)
        
        self.upload_btn = ttk.Button(prog_frame, text="Upload Firmware", command=self.start_upload, state=tk.DISABLED)
        self.upload_btn.grid(row=2, column=0, padx=5, pady=10, sticky=tk.W)
        self.cancel_btn = ttk.Button(prog_frame, text="Cancel Upload", command=self.cancel_upload, state=tk.DISABLED)
        self.cancel_btn.grid(row=2, column=0, padx=120, pady=10, sticky=tk.W)
        
        # --- Log Frame ---
        log_frame = ttk.LabelFrame(main_frame, text="Application Log", padding="10")
        log_frame.grid(row=4, column=0, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S), pady=5)
        log_frame.columnconfigure(0, weight=1)
        log_frame.rowconfigure(0, weight=1)
        main_frame.rowconfigure(4, weight=1)
        
        self.log_text = scrolledtext.ScrolledText(log_frame, height=8, width=80, wrap=tk.WORD)
        self.log_text.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
    def refresh_ports(self):
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combo['values'] = ports
        if ports:
            self.port_combo.set(ports[0])
        self.log("Refreshed serial ports")

    def browse_file(self):
        filename = filedialog.askopenfilename(
            title="Select Firmware File",
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")]
        )
        if filename:
            self.file_entry.delete(0, tk.END)
            self.file_entry.insert(0, filename)
            file_size = os.path.getsize(filename)
            self.log(f"Selected file: {os.path.basename(filename)} ({file_size} bytes)")

    def log(self, message: str):
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.log_queue.put(f"[{timestamp}] {message}")

    def process_log_queue(self):
        try:
            while True:
                message = self.log_queue.get_nowait()
                self.log_text.insert(tk.END, message + "\n")
                self.log_text.see(tk.END)
        except queue.Empty:
            pass
        finally:
            self.root.after(100, self.process_log_queue)

    def process_serial_queue(self):
        try:
            while True:
                data = self.serial_queue.get_nowait()
                try:
                    text = data.decode('ascii', errors='ignore')
                    self.monitor_text.config(state=tk.NORMAL)
                    self.monitor_text.insert(tk.END, text)
                    self.monitor_text.see(tk.END)
                    self.monitor_text.config(state=tk.DISABLED)
                except:
                    pass
        except queue.Empty:
            pass
        finally:
            self.root.after(100, self.process_serial_queue)

    def connect_serial(self):
        port = self.port_combo.get()
        baudrate = int(self.baud_combo.get())
        if not port:
            messagebox.showerror("Error", "Please select a serial port.")
            return

        try:
            self.serial_conn = serial.Serial(port, baudrate, timeout=0.1)
            self.stop_thread.clear()
            self.serial_thread = threading.Thread(target=self.serial_reader_thread)
            self.serial_thread.daemon = True
            self.serial_thread.start()
            
            self.connect_btn.config(state=tk.DISABLED)
            self.disconnect_btn.config(state=tk.NORMAL)
            self.port_combo.config(state=tk.DISABLED)
            self.baud_combo.config(state=tk.DISABLED)
            self.refresh_btn.config(state=tk.DISABLED)
            self.monitor_text.config(state=tk.NORMAL)
            self.send_entry.config(state=tk.NORMAL)
            self.send_btn.config(state=tk.NORMAL)
            self.upload_btn.config(state=tk.NORMAL)
            
            self.log(f"Connected to {port} at {baudrate} baud.")
            self.log("Please reset your board to see the bootloader menu.")
        except Exception as e:
            messagebox.showerror("Connection Error", f"Failed to open port {port}:\n{e}")
            self.serial_conn = None

    def disconnect_serial(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.stop_thread.set()
            time.sleep(0.2) # give thread time to stop
            self.serial_conn.close()
            self.serial_conn = None
            
            self.connect_btn.config(state=tk.NORMAL)
            self.disconnect_btn.config(state=tk.DISABLED)
            self.port_combo.config(state='readonly')
            self.baud_combo.config(state='readonly')
            self.refresh_btn.config(state=tk.NORMAL)
            self.monitor_text.config(state=tk.DISABLED)
            self.send_entry.config(state=tk.DISABLED)
            self.send_btn.config(state=tk.DISABLED)
            self.upload_btn.config(state=tk.DISABLED)
            
            self.log("Disconnected from serial port.")

    def serial_reader_thread(self):
        while not self.stop_thread.is_set():
            if self.serial_conn and self.serial_conn.is_open:
                try:
                    data = self.serial_conn.read(self.serial_conn.in_waiting or 1)
                    if data:
                        self.serial_queue.put(data)
                except serial.SerialException:
                    self.stop_thread.set()
                    self.root.after(0, self.disconnect_serial)
                    self.log("Serial port disconnected.")
                    break
            time.sleep(0.05)

    def send_serial_data_event(self, event):
        self.send_serial_data()

    def send_serial_data(self):
        if self.serial_conn and self.serial_conn.is_open:
            data = self.send_entry.get()
            if data:
                self.serial_conn.write(data.encode('ascii'))
                self.send_entry.delete(0, tk.END)

    def update_progress(self, percent: float, bytes_sent: int, total_bytes: int):
        self.progress_bar['value'] = percent
        self.progress_label.config(text=f"Uploading: {bytes_sent}/{total_bytes} bytes ({percent:.1f}%)")

    def start_upload(self):
        if not self.serial_conn or not self.serial_conn.is_open:
            messagebox.showerror("Error", "Not connected to a serial port.")
            return
        if not self.file_entry.get() or not os.path.exists(self.file_entry.get()):
            messagebox.showerror("Error", "Please select a valid firmware file.")
            return
        try:
            version = int(self.version_entry.get(), 0) # Base 0 auto-detects 0x
        except ValueError:
            messagebox.showerror("Error", "Invalid version number.")
            return

        # Disable controls for upload
        self.upload_btn.config(state=tk.DISABLED)
        self.cancel_btn.config(state=tk.NORMAL)
        self.disconnect_btn.config(state=tk.DISABLED)
        
        # Reset progress
        self.progress_bar['value'] = 0
        self.progress_label.config(text="Starting upload...")
        
        self.upload_thread = threading.Thread(
            target=self.upload_thread_func,
            args=(self.file_entry.get(), version)
        )
        self.upload_thread.daemon = True
        self.upload_thread.start()

    def upload_thread_func(self, firmware_path: str, version: int):
        # Stop the background reader thread to give exclusive port access to uploader
        self.stop_thread.set()
        time.sleep(0.2) # Give thread time to stop
        
        try:
            self.uploader = FirmwareUploader(self.serial_conn)
            
            success = self.uploader.upload_firmware(
                firmware_path,
                version,
                progress_callback=lambda p, b, t: self.root.after(0, self.update_progress, p, b, t),
                log_callback=lambda msg: self.log_queue.put(msg)
            )
            
            if success:
                self.root.after(0, lambda: messagebox.showinfo("Success", "Firmware uploaded successfully!"))
            else:
                self.root.after(0, lambda: messagebox.showerror("Error", "Firmware upload failed. Check log for details."))
        
        except Exception as e:
            self.log(f"ERROR: {str(e)}")
            self.root.after(0, lambda: messagebox.showerror("Error", str(e)))
        finally:
            self.uploader = None
            # Re-enable controls and restart reader thread
            self.root.after(0, self.reset_controls_after_upload)

    def cancel_upload(self):
        if self.uploader:
            self.uploader.cancel_upload()
            self.log("Upload cancellation requested.")

    def reset_controls_after_upload(self):
        self.upload_btn.config(state=tk.NORMAL)
        self.cancel_btn.config(state=tk.DISABLED)
        self.disconnect_btn.config(state=tk.NORMAL)
        self.progress_label.config(text="Ready")
        
        # Restart the serial reader thread to see output from the new application
        if self.serial_conn and self.serial_conn.is_open:
            self.stop_thread.clear()
            self.serial_thread = threading.Thread(target=self.serial_reader_thread)
            self.serial_thread.daemon = True
            self.serial_thread.start()
            self.log("Serial monitor re-activated.")

    def on_closing(self):
        self.disconnect_serial()
        self.root.destroy()
        
    def run(self):
        self.root.mainloop()

def main():
    app = BootloaderGUI()
    app.run()

if __name__ == "__main__":
    main()
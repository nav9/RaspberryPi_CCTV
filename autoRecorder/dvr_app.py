import sys
import tkinter as tk
from tkinter import messagebox, simpledialog
from flask import Flask, render_template, jsonify, request, current_app
import threading
import subprocess
import os
import re
import glob
import time
import shutil
from datetime import datetime

# --- Configuration ---
RAM_DISK_PATH = "/dev/shm/hls_buffer"  # Use shared memory as a RAM disk
VIDEO_SAVE_DIR = os.path.expanduser("~/Videos") # Save final videos here
BUFFER_DURATION_SECONDS = 1800  # 30 minutes
HLS_SEGMENT_DURATION = 4        # 4-second segments
HLS_LIST_SIZE = BUFFER_DURATION_SECONDS // HLS_SEGMENT_DURATION # 450 segments

class DeviceDetector:
    """Encapsulates the logic for finding active audio/video devices."""
    def eprint(self, *args, **kwargs):
        print(f"[Detector] ", *args, file=sys.stderr, **kwargs)

    def find_active_video_device(self, resolution):
        potential_devices = sorted(glob.glob('/dev/video*'))
        for device in potential_devices:
            self.eprint(f"Testing video device: {device} at {resolution}...")
            command = [
                "ffmpeg", "-f", "v4l2", "-video_size", resolution, "-i", device,
                "-t", "0.5", "-frames:v", "1", "-f", "null", "-", "-loglevel", "error"
            ]
            try:
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                self.eprint(f"Success! Active video device found: {device}")
                return device
            except (subprocess.CalledProcessError, FileNotFoundError):
                self.eprint(f"{device} failed test or is not a capture device.")
                continue
        self.eprint("Warning: No active video device found.")
        return None

    def find_active_audio_device(self):
        try:
            output = subprocess.check_output(["arecord", "-l"], stderr=subprocess.DEVNULL).decode("utf-8")
            usb_devices = re.findall(r"card (\d+):.*USB", output, re.IGNORECASE)
            for card_num in usb_devices:
                device_id = f"plughw:{card_num},0"
                self.eprint(f"Testing audio device: {device_id}...")
                command = ["arecord", "-D", device_id, "-f", "S16_LE", "-r", "44100", "-d", "1", "/dev/null"]
                try:
                    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    self.eprint(f"Success! Active audio device found: {device_id}")
                    return device_id
                except (subprocess.CalledProcessError, FileNotFoundError):
                    self.eprint(f"{device_id} is not an active audio input.")
                    continue
        except (subprocess.CalledProcessError, FileNotFoundError):
            self.eprint("Error: 'arecord' command not found or failed.")
        self.eprint("Warning: No active USB audio device found.")
        return None

class CaptureManager:
    """Manages the ffmpeg process and application state in a thread-safe way."""
    def __init__(self):
        self.detector = DeviceDetector()
        self.lock = threading.Lock()
        self.ffmpeg_process = None
        self.status = "Stopped"
        self.capture_thread = None
        self.is_running = False
        self.start_time = None
        self.resolution = "640x480"
        self.last_video_device = None
        self.last_audio_device = None
        
        # Prepare directories
        os.makedirs(RAM_DISK_PATH, exist_ok=True)
        os.makedirs(VIDEO_SAVE_DIR, exist_ok=True)

    def _get_encoder(self):
        """Check for hardware encoders and fall back to software."""
        encoders_output = subprocess.run(['ffmpeg', '-hide_banner', '-encoders'], capture_output=True, text=True).stdout
        if 'h264_v4l2m2m' in encoders_output:
            return 'h264_v4l2m2m'
        if 'h264_omx' in encoders_output:
            return 'h264_omx'
        print("[Manager] WARN: No hardware encoder found, falling back to libx264 (slow).")
        return 'libx264'

    def _capture_loop(self):
        while self.is_running:
            with self.lock:
                current_resolution = self.resolution
                self.status = "Detecting devices..."
            
            video_device = self.detector.find_active_video_device(current_resolution)
            audio_device = self.detector.find_active_audio_device()

            if not video_device or not audio_device:
                with self.lock:
                    self.status = "Devices not found. Retrying..."
                time.sleep(5)
                continue
            
            with self.lock:
                self.last_video_device = video_device
                self.last_audio_device = audio_device
                self.status = "Starting ffmpeg..."
            
            # Clean up old buffer before starting
            if os.path.exists(RAM_DISK_PATH):
                shutil.rmtree(RAM_DISK_PATH)
            os.makedirs(RAM_DISK_PATH, exist_ok=True)

            encoder = self._get_encoder()
            
            command = [
                'ffmpeg', '-nostdin',
                '-f', 'v4l2', '-input_format', 'yuyv422', '-video_size', current_resolution, '-framerate', '30', '-i', video_device,
                '-f', 'alsa', '-ac', '1', '-ar', '44100', '-i', audio_device,
                '-c:v', encoder, 
                '-b:v', '1M', 
                '-preset', 'ultrafast' if encoder == 'libx264' else 'fast', # Ultrafast for software encoding
                '-g', '60',
                '-c:a', 'aac', '-b:a', '128k',
                '-f', 'hls',
                '-hls_time', str(HLS_SEGMENT_DURATION),
                '-hls_list_size', str(HLS_LIST_SIZE),
                '-hls_flags', 'delete_segments',
                '-hls_segment_filename', os.path.join(RAM_DISK_PATH, 'segment%06d.ts'),
                os.path.join(RAM_DISK_PATH, 'playlist.m3u8')
            ]
            
            print(f"[Manager] Starting ffmpeg with command: {' '.join(command)}")
            self.ffmpeg_process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            with self.lock:
                self.status = f"Recording at {current_resolution}"
                self.start_time = time.time()
            
            # Monitor the process
            stdout, stderr = self.ffmpeg_process.communicate()
            if self.is_running: # If it exited unexpectedly
                print(f"[Manager] ERROR: ffmpeg exited unexpectedly. Code: {self.ffmpeg_process.returncode}")
                print(f"[Manager] FFMPEG STDERR:\n{stderr.decode(errors='ignore')}")
                with self.lock:
                    self.status = "ffmpeg crashed. Restarting..."
                time.sleep(5)
        
        with self.lock:
            self.status = "Stopped"
        print("[Manager] Capture loop has terminated.")

    def start_capture(self):
        with self.lock:
            if self.is_running:
                print("[Manager] Capture is already running.")
                return
            self.is_running = True
            self.status = "Starting..."
            self.capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
            self.capture_thread.start()
            print("[Manager] Capture thread started.")

    def stop_capture(self):
        with self.lock:
            if not self.is_running:
                print("[Manager] Capture is not running.")
                return
            self.is_running = False # Signal the loop to stop
            if self.ffmpeg_process:
                print("[Manager] Terminating ffmpeg process...")
                self.ffmpeg_process.terminate() # Ask ffmpeg to stop gracefully
            self.ffmpeg_process = None
            self.start_time = None
        print("[Manager] Stop signal sent to capture thread.")
        # The thread will exit on its own after the process is terminated

    def restart_capture(self):
        print("[Manager] Restarting capture...")
        self.stop_capture()
        time.sleep(1) # Give it a moment to stop
        self.start_capture()
    
    def change_resolution(self, new_resolution):
        with self.lock:
            self.resolution = new_resolution
        self.restart_capture()

    def get_stats(self):
        with self.lock:
            stats = {
                "status": self.status,
                "resolution": self.resolution,
                "uptime": 0,
                "buffer_size_mb": 0,
                "free_ram_mb": 0,
                "buffered_segments": 0,
                "max_segments": HLS_LIST_SIZE
            }
            if self.is_running and self.start_time:
                stats["uptime"] = time.time() - self.start_time
            
            # Get buffer size
            if os.path.exists(RAM_DISK_PATH):
                total_size = 0
                ts_files = glob.glob(os.path.join(RAM_DISK_PATH, '*.ts'))
                stats["buffered_segments"] = len(ts_files)
                for f in ts_files:
                    total_size += os.path.getsize(f)
                stats["buffer_size_mb"] = total_size / (1024 * 1024)
            
            # Get free RAM
            mem_info = os.popen('free -m').read()
            stats["free_ram_mb"] = int(re.search(r'Mem:\s+\d+\s+\d+\s+(\d+)', mem_info).group(1))
            return stats

    def save_to_disk(self):
        with self.lock:
            if not self.is_running:
                return {"status": "error", "message": "Capture is not running."}
            self.status = "Saving video..."
        
        playlist_path = os.path.join(RAM_DISK_PATH, 'playlist.m3u8')
        if not os.path.exists(playlist_path):
            with self.lock:
                self.status = f"Recording at {self.resolution}" # Revert status
            return {"status": "error", "message": "Playlist file not found."}
        
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        output_filename = os.path.join(VIDEO_SAVE_DIR, f"recording_{timestamp}.mp4")
        
        # Use ffmpeg to concatenate HLS segments into a single MP4. This is the most reliable way.
        save_command = [
            'ffmpeg', '-y',
            '-i', playlist_path,
            '-c', 'copy', # Copy streams without re-encoding
            '-absf', 'aac_adtstoasc', # Important filter for AAC in MP4 container
            output_filename
        ]
        
        print(f"[Manager] Saving video with command: {' '.join(save_command)}")
        try:
            process = subprocess.run(save_command, check=True, capture_output=True, text=True)
            print(f"[Manager] Save successful: {output_filename}")
            result = {"status": "success", "message": f"Saved to {output_filename}"}
        except subprocess.CalledProcessError as e:
            print(f"[Manager] ERROR: Save failed. Code: {e.returncode}")
            print(f"[Manager] Save FFMPEG STDERR:\n{e.stderr}")
            result = {"status": "error", "message": "Failed to save video."}
        
        with self.lock:
            self.status = f"Recording at {self.resolution}" # Revert status
        return result

# --- Flask Web Server ---
app = Flask(__name__)

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/status")
def api_status():
    manager = current_app.config['manager']
    return jsonify(manager.get_stats())

@app.route("/api/save")
def api_save():
    manager = current_app.config['manager']
    result = manager.save_to_disk()
    return jsonify(result)

@app.route("/api/restart")
def api_restart():
    manager = current_app.config['manager']
    manager.restart_capture()
    return jsonify({"status": "success", "message": "Capture restarting..."})

@app.route("/api/shutdown")
def api_shutdown():
    os.system("sudo shutdown -h now")
    return jsonify({"status": "success", "message": "Shutdown command sent."})

@app.route("/api/reboot")
def api_reboot():
    os.system("sudo reboot")
    return jsonify({"status": "success", "message": "Reboot command sent."})
    
@app.route("/api/change_resolution", methods=['POST'])
def api_change_resolution():
    res = request.json.get('resolution')
    if res in ["640x480", "320x240", "160x120"]: # Add more valid resolutions if needed
        manager = current_app.config['manager']
        manager.change_resolution(res)
        return jsonify({"status": "success", "message": f"Changing resolution to {res} and restarting capture..."})
    return jsonify({"status": "error", "message": "Invalid resolution."}), 400


# --- Tkinter GUI ---
class AppGUI:
    def __init__(self, root, manager):
        self.root = root
        self.manager = manager
        self.root.title("RPi DVR Control")
        
        # Stats Frame
        stats_frame = tk.Frame(root, padx=10, pady=10)
        stats_frame.pack(fill=tk.X)
        
        self.status_label = tk.Label(stats_frame, text="Status: Initializing...", anchor="w")
        self.status_label.pack(fill=tk.X)
        self.uptime_label = tk.Label(stats_frame, text="Uptime: 0s", anchor="w")
        self.uptime_label.pack(fill=tk.X)
        self.buffer_label = tk.Label(stats_frame, text="Buffer: 0.00 MB", anchor="w")
        self.buffer_label.pack(fill=tk.X)
        self.ram_label = tk.Label(stats_frame, text="Free RAM: 0 MB", anchor="w")
        self.ram_label.pack(fill=tk.X)

        # Control Frame
        control_frame = tk.Frame(root, padx=10, pady=10)
        control_frame.pack(fill=tk.X)

        self.pause_button = tk.Button(control_frame, text="Pause Recording", command=self.toggle_pause)
        self.pause_button.pack(fill=tk.X, pady=5)
        self.save_button = tk.Button(control_frame, text="Save Buffer to Disk", command=self.save_video)
        self.save_button.pack(fill=tk.X, pady=5)
        self.stop_button = tk.Button(control_frame, text="Stop Program", command=self.stop_program, bg="salmon")
        self.stop_button.pack(fill=tk.X, pady=5)

        self.update_stats()

    def update_stats(self):
        stats = self.manager.get_stats()
        self.status_label.config(text=f"Status: {stats['status']}")
        self.uptime_label.config(text=f"Uptime: {int(stats['uptime'])}s")
        self.buffer_label.config(text=f"Buffer: {stats['buffered_segments']}/{stats['max_segments']} segments ({stats['buffer_size_mb']:.2f} MB)")
        self.ram_label.config(text=f"Free RAM: {stats['free_ram_mb']} MB")
        self.root.after(1000, self.update_stats) # Update every second

    def toggle_pause(self):
        with self.manager.lock:
            if self.manager.status.startswith("Recording"):
                if messagebox.askyesno("Confirm", "Pause recording? This will stop the current capture."):
                    self.manager.stop_capture()
                    self.pause_button.config(text="Resume Recording")
            else:
                if messagebox.askyesno("Confirm", "Resume recording?"):
                    self.manager.start_capture()
                    self.pause_button.config(text="Pause Recording")

    def save_video(self):
        # No confirmation for save
        threading.Thread(target=self.manager.save_to_disk, daemon=True).start()

    def stop_program(self):
        if messagebox.askyesno("Confirm", "Are you sure you want to stop the entire application?"):
            self.manager.stop_capture()
            self.root.quit()

# --- Main Execution ---
if __name__ == "__main__":
    # 1. Create the central manager
    capture_manager = CaptureManager()
    
    # 2. Configure and start Flask server in a daemon thread
    app.config['manager'] = capture_manager
    flask_thread = threading.Thread(
        target=lambda: app.run(host='0.0.0.0', port=5000, debug=False),
        daemon=True
    )
    flask_thread.start()
    print("[Main] Flask server thread started.")

    # 3. Start the capture process
    capture_manager.start_capture()
    
    # 4. Create and run the Tkinter GUI in the main thread
    root = tk.Tk()
    gui = AppGUI(root, capture_manager)
    print("[Main] Tkinter GUI started.")
    root.mainloop()
    
    # 5. Cleanup on GUI exit
    print("[Main] GUI closed, cleaning up...")
    capture_manager.stop_capture()
    print("[Main] Application finished.")

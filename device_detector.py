import subprocess
import re
import time
import os
import glob
import sys

def eprint(*args, **kwargs):
    """Helper function to print to stderr."""
    print(*args, file=sys.stderr, **kwargs)

def find_active_video_device():
    """Finds an active video device by trying to capture a few frames with ffmpeg."""
    potential_devices = sorted(glob.glob('/dev/video*'))
    
    for device in potential_devices:
        eprint(f"Testing video device: {device}...")
        command = [
            "ffmpeg",
            "-f", "v4l2",
            "-i", device,
            "-t", "0.5",
            "-frames:v", "1",
            "-f", "null",
            "-",
            "-loglevel", "error" 
        ]
        try:
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            eprint(f"Success! Active video device found: {device}")
            return device
        except (subprocess.CalledProcessError, FileNotFoundError):
            eprint(f"{device} is not an active video capture device or ffmpeg error.")
            continue
            
    eprint("Warning: No active video device found.")
    return None

def find_active_audio_device():
    """Finds an active USB audio device by trying to record a short clip, returning the plughw ID."""
    try:
        output = subprocess.check_output(["arecord", "-l"], stderr=subprocess.DEVNULL).decode("utf-8")
        usb_devices = re.findall(r"card (\d+):.*USB", output, re.IGNORECASE)
        
        for card_num in usb_devices:
            # *** USE plughw INSTEAD OF hw ***
            device_id = f"plughw:{card_num},0" 
            eprint(f"Testing audio device: {device_id}...")
            command = [
                "arecord",
                "-D", device_id, 
                "-f", "S16_LE",
                "-r", "44100",
                "-d", "1",
                "/dev/null"
            ]
            try:
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                eprint(f"Success! Active audio device found: {device_id}")
                return device_id
            except (subprocess.CalledProcessError, FileNotFoundError):
                eprint(f"{device_id} is not an active audio input or arecord error.")
                continue

    except (subprocess.CalledProcessError, FileNotFoundError):
        eprint("Error: 'arecord' command not found or failed to list devices.")
        
    eprint("Warning: No active USB audio device found.")
    return None

if __name__ == "__main__":
    if len(sys.argv) > 1:
        if sys.argv[1] == "video":
            video_dev = find_active_video_device()
            if video_dev:
                print(video_dev) 
        elif sys.argv[1] == "audio":
            audio_dev = find_active_audio_device()
            if audio_dev:
                print(audio_dev)

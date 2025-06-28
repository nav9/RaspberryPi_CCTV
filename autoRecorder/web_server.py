import subprocess
import os
import sys
from flask import Flask, request, render_template_string
import threading
import time

app = Flask(__name__)

# --- Determine the current directory dynamically ---
CURRENT_DIR = os.path.dirname(os.path.abspath(__file__))
CAMERA_RECORDER_EXECUTABLE = os.path.join(CURRENT_DIR, "camera_recorder")
RECORDINGS_DIR = os.path.join(CURRENT_DIR, "recordings")

# --- HTML Template with added buttons, confirmation dialogues, and a banner ---
HTML_TEMPLATE = """
<!DOCTYPE html>
<html>
<head>
    <title>RPi Camera Controller</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background-color: #f0f0f0; }
        .button {
            background-color: #4CAF50; /* Green */
            border: none;
            color: white;
            padding: 20px 40px;
            text-align: center;
            text-decoration: none;
            display: inline-block;
            font-size: 24px;
            margin: 10px;
            cursor: pointer;
            border-radius: 12px;
            width: 80%;
            max-width: 300px;
        }
        .button.red { background-color: #f44336; }
        .button.blue { background-color: #008CBA; }
        .button.orange { background-color: #ff9800; }

        /* Banner styles */
        #banner {
            position: fixed;
            top: 20px;
            left: 50%;
            transform: translateX(-50%);
            background-color: #333;
            color: white;
            padding: 15px 30px;
            border-radius: 8px;
            font-size: 18px;
            z-index: 1000;
            display: none; /* Hidden by default */
            opacity: 0;
            transition: opacity 0.5s ease-in-out;
        }
    </style>
    <script>
        // Function to show the confirmation banner
        function showBanner(message) {
            const banner = document.getElementById('banner');
            banner.innerText = message;
            banner.style.display = 'block';
            banner.style.opacity = 1;

            // Hide the banner after 3 seconds
            setTimeout(() => {
                banner.style.opacity = 0;
                setTimeout(() => { banner.style.display = 'none'; }, 500); // Wait for fade-out
            }, 3000);
        }

        // Function to handle button clicks with confirmation
        function sendCommand(command, confirmMessage) {
            if (confirmMessage) {
                if (!confirm(confirmMessage)) {
                    return; // User cancelled the action
                }
            }
            
            fetch('/' + command, { method: 'POST' })
                .then(response => response.text())
                .then(data => {
                    showBanner(data);
                })
                .catch(error => {
                    console.error('Error:', error);
                    showBanner('An error occurred. Check the server logs.');
                });
        }
    </script>
</head>
<body>
    <div id="banner"></div>
    <h1>Raspberry Pi Controller</h1>
    <button class="button" onclick="sendCommand('save_and_restart', 'Are you sure you want to save the current footage and start a new recording?')">
        Save Footage Now
    </button>
    <button class="button orange" onclick="sendCommand('relaunch_recorder', 'Are you sure you want to relaunch the recorder? This will stop the current recording.')">
        Relaunch Recorder
    </button>
    <button class="button blue" onclick="sendCommand('reboot_pi', 'Are you sure you want to reboot the Raspberry Pi?')">
        Reboot RPi
    </button>
    <button class="button red" onclick="sendCommand('shutdown_pi', 'Are you sure you want to shutdown the Raspberry Pi?')">
        Shutdown RPi
    </button>
</body>
</html>
"""

# --- Helper function to stop the C++ recorder process ---
def stop_recorder_process():
    """Sends SIGINT to the C++ process to trigger graceful shutdown."""
    try:
        # Use pkill to send a SIGINT signal to the C++ process
        # The `-f` flag matches the full command line, which is useful
        subprocess.run(['pkill', '-SIGINT', '-f', CAMERA_RECORDER_EXECUTABLE], check=True)
        # Give the process a moment to save the file and exit
        time.sleep(3)
        return True
    except subprocess.CalledProcessError:
        print("Recorder process was not running.")
        return False

# --- Helper function to launch the C++ recorder process ---
def launch_recorder_process():
    """Launches the C++ recorder process in the background using nohup."""
    try:
        # Popen is non-blocking. nohup ensures it keeps running after the shell exits.
        # preexec_fn=os.setpgrp is needed to detach the process from the parent group.
        subprocess.Popen(['nohup', CAMERA_RECORDER_EXECUTABLE, '&'], preexec_fn=os.setpgrp)
        return True
    except FileNotFoundError:
        print(f"Error: Executable not found at {CAMERA_RECORDER_EXECUTABLE}")
        return False

# --- Flask Routes ---
@app.route('/')
def index():
    """Main page with the buttons."""
    return render_template_string(HTML_TEMPLATE)

@app.route('/save_and_restart', methods=['POST'])
def save_and_restart():
    """Endpoint to stop the current recording, save it, and start a new one."""
    print("Received save and restart command.")
    
    # Stop the current recorder first
    stop_recorder_process()
    
    # Now, relaunch the C++ program to start a new recording
    if launch_recorder_process():
        return "Footage saved and a new recording has started.", 200
    else:
        return "Failed to relaunch the recorder.", 500

@app.route('/relaunch_recorder', methods=['POST'])
def relaunch_recorder():
    """Endpoint to stop and relaunch the recorder process without saving a file."""
    print("Received relaunch command.")
    
    # Stop any existing process first. This will save the current file.
    stop_recorder_process()
    
    # Launch a new process
    if launch_recorder_process():
        return "Recorder has been relaunched.", 200
    else:
        return "Failed to relaunch the recorder.", 500

@app.route('/reboot_pi', methods=['POST'])
def reboot_pi():
    """Endpoint to reboot the Raspberry Pi."""
    print("Received reboot command.")
    
    # Stop the recorder first for a graceful shutdown
    stop_recorder_process()
    
    # Execute the reboot command in a separate thread to not block the HTTP response
    def reboot_thread():
        time.sleep(3) # Give the C++ program time to save the file
        # Use `sudo` to run the shutdown command. The user running the script must have sudo privileges.
        subprocess.run(['sudo', 'reboot', 'now'])
    
    threading.Thread(target=reboot_thread).start()
    
    return "Reboot signal sent. The Raspberry Pi is restarting.", 200

@app.route('/shutdown_pi', methods=['POST'])
def shutdown_pi():
    """Endpoint to shut down the Raspberry Pi."""
    print("Received shutdown command.")
    
    # Stop the recorder first for a graceful shutdown
    stop_recorder_process()
    
    # Execute the shutdown command in a separate thread to not block the response
    def shutdown_thread():
        time.sleep(3) # Give the C++ program time to save the file
        subprocess.run(['sudo', 'shutdown', 'now'])
    
    threading.Thread(target=shutdown_thread).start()
    
    return "Shutdown signal sent. The Raspberry Pi is shutting down.", 200

if __name__ == '__main__':
    # Create the recordings directory if it doesn't exist
    os.makedirs(RECORDINGS_DIR, exist_ok=True)
    
    # Ensure the C++ executable has execute permissions
    if os.path.exists(CAMERA_RECORDER_EXECUTABLE):
        os.chmod(CAMERA_RECORDER_EXECUTABLE, 0o755)
    else:
        print(f"Error: C++ executable not found at {CAMERA_RECORDER_EXECUTABLE}. Please compile it first.")
        sys.exit(1)

    # Launch the C++ program on server startup
    print("Launching camera recorder C++ program...")
    launch_recorder_process()
    
    # Run the Flask app on the local network accessible from your phone
    app.run(host='0.0.0.0', port=5000, debug=False) # debug=False for production use

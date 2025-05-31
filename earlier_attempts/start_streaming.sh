#!/usr/bin/env bash
set -e

# --- Configuration ---
PORT=8090
PYTHON_SCRIPT_PATH="./device_detector.py"
SHUTDOWN_FILE="/tmp/shutdown_request"
SHUTDOWN_SERVER_PORT=8080
PIPE_FILE="/tmp/stream.ts"
LOG_FILE="streaming_log.txt"

# --- Cleanup function ---
cleanup() {
    echo "[INFO] Cleaning up…" | tee -a "$LOG_FILE"
    [ -n "$FFMPEG_PID" ] && kill "$FFMPEG_PID" 2>/dev/null
    [ -n "$CAT_PID" ] && kill "$CAT_PID" 2>/dev/null
    [ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null
    [ -n "$SHUTDOWN_PID" ] && kill "$SHUTDOWN_PID" 2>/dev/null
    rm -f "$SHUTDOWN_FILE" "$PIPE_FILE"
    echo "[INFO] Done. Exiting." | tee -a "$LOG_FILE"
    exit 0
}

trap cleanup SIGINT SIGTERM

# --- Prepare log file ---
touch "$LOG_FILE" || { echo "FATAL: Cannot create log file $LOG_FILE" >&2; exit 1; }

# --- Start Flask-based shutdown listener ---
python3 - <<EOF &> /dev/null &
from flask import Flask
import os

app = Flask(__name__)

@app.route('/shutdown')
def shutdown():
    open('$SHUTDOWN_FILE', 'a').close()
    return 'Shutdown initiated...'

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=$SHUTDOWN_SERVER_PORT)
EOF

SHUTDOWN_PID=$!
echo "[INFO] Shutdown listener PID: $SHUTDOWN_PID" | tee -a "$LOG_FILE"


# --- Main Streaming Loop ---
while true; do
    # 1) Check for shutdown‐request
    if [ -f "$SHUTDOWN_FILE" ]; then
        echo "[INFO] Shutdown file found. Shutting down Pi…" | tee -a "$LOG_FILE"
        sudo shutdown -h now
        break
    fi

    # 2) Detect devices via your Python script
    VIDEO_DEVICE=$(python3 "$PYTHON_SCRIPT_PATH" video 2>>"$LOG_FILE")
    AUDIO_DEVICE=$(python3 "$PYTHON_SCRIPT_PATH" audio 2>>"$LOG_FILE")

    if [[ -n "$VIDEO_DEVICE" && -n "$AUDIO_DEVICE" ]]; then
        echo "[INFO] Devices OK: Video=$VIDEO_DEVICE Audio=$AUDIO_DEVICE" | tee -a "$LOG_FILE"

        # 3) Recreate named pipe (FIFO)
        rm -f "$PIPE_FILE"
        mkfifo "$PIPE_FILE"
        echo "[INFO] FIFO $PIPE_FILE created." | tee -a "$LOG_FILE"
        sleep 1   # give kernel a moment

        # 4) Spawn a background reader so FFmpeg’s write end never blocks
        cat "$PIPE_FILE" > /dev/null &
        CAT_PID=$!
        echo "[INFO] cat reader PID: $CAT_PID (reading from $PIPE_FILE)" | tee -a "$LOG_FILE"

        # 5) Decide which video codec to use:
        #    Try h264_v4l2m2m first; if that fails, fallback to libx264.
        #    (You can test availability by checking `ffmpeg -encoders | grep h264_v4l2m2m`)
        USE_CODEC="h264_v4l2m2m"
        if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "h264_v4l2m2m"; then
            echo "[WARN] h264_v4l2m2m not found. Falling back to libx264 (software)." | tee -a "$LOG_FILE"
            USE_CODEC="libx264"
        fi

        # 6) Launch FFmpeg → named pipe
        #ffmpeg -y -nostdin \
        #    -f v4l2 -framerate 30 -video_size 640x480 -i "$VIDEO_DEVICE" \
        #    -f alsa -ac 1 -ar 44100 -i "$AUDIO_DEVICE" \
        #    -c:v "$USE_CODEC" -preset ultrafast -tune zerolatency -b:v 1M -bufsize 2M \
        #    -c:a aac -b:a 128k -ac 1 \
        #    -f mpegts -flush_packets 1 -muxpreload 0 -muxdelay 0.001 \
        #    "$PIPE_FILE" \
        #>> "$LOG_FILE" 2>&1 &

        # 6) Launch FFmpeg → named pipe (not working)
        #ffmpeg -y -nostdin \
        #    -f v4l2 -framerate 30 -video_size 640x480 -i "$VIDEO_DEVICE" \
        #    -f alsa -ac 1 -ar 44100 -i "$AUDIO_DEVICE" \
        #    -c:v "$USE_CODEC" -preset ultrafast -tune zerolatency -b:v 1M -bufsize 2M \
        #    -c:a aac -b:a 128k -ac 1 \
        #    -f mpegts -flush_packets 1 -muxpreload 0 -muxdelay 0.001 \
        #    "$PIPE_FILE"

        # 6) Launch FFmpeg → named pipe (working but corrupted video)
        ffmpeg -y -nostdin \
            -thread_queue_size 512 \
            -f v4l2 -input_format yuyv422 -video_size 640x480 -framerate 30 -i "$VIDEO_DEVICE" \
            -thread_queue_size 512 \
            -f alsa -ac 1 -ar 44100 -i "$AUDIO_DEVICE" \
            -c:v "$USE_CODEC" -b:v 1M -g 50 -preset veryfast \
            -c:a aac -b:a 128k -ac 1 \
            -f mpegts -muxdelay 0.001 \
            "$PIPE_FILE" \
        >> "$LOG_FILE" 2>&1 &

        FFMPEG_PID=$!
        echo "[INFO] FFmpeg PID: $FFMPEG_PID (using codec: $USE_CODEC)" | tee -a "$LOG_FILE"

        # 7) Start a simple HTTP server that serves the MPEG-TS stream
        python3 stream_server.py &> /dev/null &
        HTTP_PID=$!
        echo "[INFO] Python HTTP server PID: $HTTP_PID (serving on port $PORT)" | tee -a "$LOG_FILE"

        #echo "[INFO] HTTP server PID: $HTTP_PID (serving on port $PORT)" | tee -a "$LOG_FILE"

        # 8) Wait for FFmpeg to exit (because of a crash or manual kill)
        wait "$FFMPEG_PID"
        FFMPEG_EXIT_CODE=$?
        echo "[WARN] FFmpeg exited (code $FFMPEG_EXIT_CODE). Cleaning up in 5s…" | tee -a "$LOG_FILE"

        # 9) Kill the cat reader & HTTP server, then retry
        kill "$CAT_PID" 2>/dev/null || true
        CAT_PID=""
        kill "$HTTP_PID" 2>/dev/null || true
        HTTP_PID=""
        FFMPEG_PID=""

        sleep 5
    else
        echo "[ERROR] Missing video or audio device (V='$VIDEO_DEVICE', A='$AUDIO_DEVICE'). Retrying in 10s…" | tee -a "$LOG_FILE"
        sleep 10
    fi
done

cleanup

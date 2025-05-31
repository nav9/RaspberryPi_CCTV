#!/bin/bash
#This script plays the video stream from the webcam using VLC and this can be viewed from any 
#other device by connecting via VNC (although no audio will be available over free versions of VNC)

# Auto-detect devices using your detection script since sometimes the endpoint number varies when connecting to a different USB port
VIDEO_DEV=$(python3 /home/nav/RaspberryPi_CCTV/device_detector.py video)
AUDIO_DEV=$(python3 /home/nav/RaspberryPi_CCTV/device_detector.py audio)

# Fall back to defaults if detection fails
[[ -z "$VIDEO_DEV" || ! -e "$VIDEO_DEV" ]] && VIDEO_DEV="/dev/video0"
[[ -z "$AUDIO_DEV" ]] && AUDIO_DEV="plughw:2,0"

# Run VLC
#cvlc v4l2://$VIDEO_DEV input-slave=alsa://$AUDIO_DEV --fullscreen --no-osd --quiet
#cvlc v4l2://$VIDEO_DEV input-slave=alsa://$AUDIO_DEV --quiet
vlc v4l2://$VIDEO_DEV :input-slave=alsa://$AUDIO_DEV --no-qt-fs-controller --video-on-top --no-video-title-show --fullscreen

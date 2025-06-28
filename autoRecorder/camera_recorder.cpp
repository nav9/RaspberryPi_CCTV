#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <alsa/asoundlib.h>
#include <libudev.h>

// --- Global variables for thread management and state ---
std::mutex mtx; // Mutex for shared resources
volatile bool running = true;
volatile bool ffmpeg_is_running = false;
volatile bool webcam_connected = false;
volatile bool mic_connected = false;
std::string video_device_path = "";
std::string audio_device_path = "";
pid_t ffmpeg_pid = -1;
FILE* ffmpeg_pipe_video = nullptr;
FILE* ffmpeg_pipe_audio = nullptr;

// --- Constants ---
const int VIDEO_WIDTH = 640;
const int VIDEO_HEIGHT = 480;
const int VIDEO_FPS = 30;
const int VIDEO_FRAME_SIZE = VIDEO_WIDTH * VIDEO_HEIGHT * 2; // YUYV format uses 2 bytes per pixel
const int AUDIO_SAMPLE_RATE = 44100;
const int AUDIO_CHANNELS = 1;
const int AUDIO_BITS_PER_SAMPLE = 16;
const int AUDIO_FRAME_SIZE = (AUDIO_SAMPLE_RATE * AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS; // bytes per second
const int AUDIO_BUFFER_SIZE_MS = 20; // 20ms audio buffer chunk
const int AUDIO_BUFFER_SIZE = (AUDIO_SAMPLE_RATE / 1000) * AUDIO_BUFFER_SIZE_MS * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS;

// --- Function to execute a shell command and check status ---
bool run_command(const std::string& cmd) {
    int status = system(cmd.c_str());
    if (status == -1) {
        std::cerr << "Error running command: " << cmd << std::endl;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// --- Device detection and management functions ---
std::string find_usb_device(const std::string& subsystem, const std::string& devtype) {
    struct udev* udev;
    struct udev_enumerate* enumerate;
    struct udev_list_entry* devices, *dev_list_entry;
    struct udev_device* dev;
    std::string found_path = "";

    udev = udev_new();
    if (!udev) {
        std::cerr << "Can't create udev context." << std::endl;
        return "";
    }

    enumerate = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(enumerate, subsystem.c_str());
    udev_enumerate_scan_devices(enumerate);
    devices = udev_enumerate_get_list_entry(enumerate);

    udev_list_entry_foreach(dev_list_entry, devices) {
        const char* path = udev_list_entry_get_name(dev_list_entry);
        dev = udev_device_new_from_syspath(udev, path);
        if (dev && udev_device_get_devnode(dev) && std::string(udev_device_get_devnode(dev)).find("/dev/" + devtype) != std::string::npos) {
            // Found a device of the correct type
            // You can add more specific checks here, like vendor ID, product ID etc.
            std::cout << "Found " << subsystem << " device: " << udev_device_get_devnode(dev) << std::endl;
            found_path = udev_device_get_devnode(dev);
            udev_device_unref(dev);
            break;
        }
        udev_device_unref(dev);
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    return found_path;
}

void check_and_update_devices() {
    std::lock_guard<std::mutex> lock(mtx);
    std::string new_video_path = find_usb_device("video4linux", "video");
    std::string new_audio_path = find_usb_device("sound", "pcm");

    if (new_video_path.empty()) {
        if (webcam_connected) {
            std::cout << "Webcam disconnected." << std::endl;
            webcam_connected = false;
        }
    } else {
        if (!webcam_connected || new_video_path != video_device_path) {
            std::cout << "Webcam connected/re-connected at: " << new_video_path << std::endl;
            webcam_connected = true;
            video_device_path = new_video_path;
        }
    }

    if (new_audio_path.empty()) {
        if (mic_connected) {
            std::cout << "Mic disconnected." << std::endl;
            mic_connected = false;
        }
    } else {
        if (!mic_connected || new_audio_path != audio_device_path) {
            std::cout << "Mic connected/re-connected at: " << new_audio_path << std::endl;
            mic_connected = true;
            audio_device_path = new_audio_path;
        }
    }
}

// --- FFmpeg process management functions ---
void start_ffmpeg_process() {
    std::lock_guard<std::mutex> lock(mtx);
    if (ffmpeg_is_running) {
        std::cout << "FFmpeg is already running." << std::endl;
        return;
    }

    if (!webcam_connected || !mic_connected) {
        std::cout << "Cannot start recording: Webcam and/or mic not connected." << std::endl;
        return;
    }

    // Create a unique timestamp for the filename
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d_%H-%M-%S");
    std::string filename = "/home/pi/recordings/footages_" + ss.str() + ".mp4";

    // Create the directory if it doesn't exist
    run_command("mkdir -p /home/pi/recordings");

    // Construct the FFmpeg command
    // Use `h264_omx` for hardware acceleration on RPi 3B
    // The `-i -` tells ffmpeg to read from standard input
    std::string ffmpeg_cmd = "ffmpeg -y -f rawvideo -pix_fmt yuyv422 -s " + std::to_string(VIDEO_WIDTH) + "x" + std::to_string(VIDEO_HEIGHT) + " -r " + std::to_string(VIDEO_FPS) + " -i - " +
                             "-f alsa -ac " + std::to_string(AUDIO_CHANNELS) + " -ar " + std::to_string(AUDIO_SAMPLE_RATE) + " -i default " +
                             "-c:v h264_omx -b:v 2M -c:a aac -b:a 128k -f mp4 " + filename;

    std::cout << "Starting FFmpeg command: " << ffmpeg_cmd << std::endl;

    // Use popen to create a pipe and spawn the process. "w" for writing to stdin.
    // Note: popen does not give you a PID directly. You need to get it from `pclose` or by checking the process list.
    ffmpeg_pipe_video = popen(ffmpeg_cmd.c_str(), "w");
    if (!ffmpeg_pipe_video) {
        std::cerr << "Failed to open pipe to FFmpeg." << std::endl;
        return;
    }

    // A simple way to get the PID is to use `pclose`'s return value or check `/proc`.
    // We'll rely on the parent-child relationship and manage the state.
    // A more robust method would involve `fork` and `exec`.
    
    ffmpeg_is_running = true;
    std::cout << "FFmpeg process started, recording to " << filename << std::endl;
}

void stop_ffmpeg_process() {
    std::lock_guard<std::mutex> lock(mtx);
    if (!ffmpeg_is_running) {
        std::cout << "FFmpeg is not running." << std::endl;
        return;
    }

    std::cout << "Stopping FFmpeg process..." << std::endl;
    
    // Close the pipe. This sends EOF to FFmpeg's stdin, which tells it to finalize the file and exit.
    if (ffmpeg_pipe_video) {
        pclose(ffmpeg_pipe_video);
        ffmpeg_pipe_video = nullptr;
    }
    
    ffmpeg_is_running = false;
    std::cout << "FFmpeg process stopped." << std::endl;
}

// --- Capture threads ---
void video_capture_thread() {
    int fd_video = -1;
    v4l2_format format;
    char* video_buffer = nullptr;

    while (running) {
        // Check for device and connect if needed
        if (!webcam_connected) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (fd_video == -1) {
            fd_video = open(video_device_path.c_str(), O_RDWR);
            if (fd_video < 0) {
                std::cerr << "Failed to open video device " << video_device_path << ". Retrying..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Set video format
            memset(&format, 0, sizeof(format));
            format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            format.fmt.pix.width = VIDEO_WIDTH;
            format.fmt.pix.height = VIDEO_HEIGHT;
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // Webcam output format
            if (ioctl(fd_video, VIDIOC_S_FMT, &format) < 0) {
                std::cerr << "Failed to set video format." << std::endl;
                close(fd_video);
                fd_video = -1;
                continue;
            }

            // Set frame rate
            v4l2_streamparm parm;
            memset(&parm, 0, sizeof(parm));
            parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = VIDEO_FPS;
            if (ioctl(fd_video, VIDIOC_S_PARM, &parm) < 0) {
                 std::cerr << "Failed to set frame rate." << std::endl;
                 // It's not a fatal error, so we can continue
            }

            // Allocate a buffer for the frame
            video_buffer = new char[VIDEO_FRAME_SIZE];
            std::cout << "Video capture setup complete on " << video_device_path << std::endl;
        }

        // Capture and write the frame
        if (read(fd_video, video_buffer, VIDEO_FRAME_SIZE) == VIDEO_FRAME_SIZE) {
            if (ffmpeg_is_running && ffmpeg_pipe_video) {
                std::lock_guard<std::mutex> lock(mtx);
                fwrite(video_buffer, 1, VIDEO_FRAME_SIZE, ffmpeg_pipe_video);
                fflush(ffmpeg_pipe_video); // Important to flush the buffer
            }
        } else {
            // Error or device disconnected
            std::cerr << "Failed to read video frame. Device may have been disconnected." << std::endl;
            if (fd_video != -1) {
                close(fd_video);
                fd_video = -1;
                delete[] video_buffer;
                video_buffer = nullptr;
                webcam_connected = false; // Mark as disconnected to trigger reconnection logic
            }
        }
    }

    // Cleanup on exit
    if (fd_video != -1) {
        close(fd_video);
    }
    if (video_buffer) {
        delete[] video_buffer;
    }
    std::cout << "Video capture thread terminated." << std::endl;
}

void audio_capture_thread() {
    snd_pcm_t* handle_audio = nullptr;
    snd_pcm_hw_params_t* params = nullptr;
    unsigned int rate = AUDIO_SAMPLE_RATE;
    int dir;
    char* audio_buffer = nullptr;
    snd_pcm_uframes_t frames = AUDIO_BUFFER_SIZE / (AUDIO_BITS_PER_SAMPLE / 8); // frames per chunk

    while (running) {
        // Check for device and connect if needed
        if (!mic_connected) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (handle_audio == nullptr) {
            if (snd_pcm_open(&handle_audio, audio_device_path.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
                std::cerr << "Failed to open audio device " << audio_device_path << ". Retrying..." << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            // Set audio parameters
            snd_pcm_hw_params_alloca(&params);
            snd_pcm_hw_params_any(handle_audio, params);
            snd_pcm_hw_params_set_access(handle_audio, params, SND_PCM_ACCESS_RW_INTERLEAVED);
            snd_pcm_hw_params_set_format(handle_audio, params, SND_PCM_FORMAT_S16_LE); // 16-bit little-endian
            snd_pcm_hw_params_set_channels(handle_audio, params, AUDIO_CHANNELS);
            snd_pcm_hw_params_set_rate_near(handle_audio, params, &rate, &dir);
            snd_pcm_hw_params_set_period_size_near(handle_audio, params, &frames, &dir);

            if (snd_pcm_hw_params(handle_audio, params) < 0) {
                std::cerr << "Failed to set audio parameters." << std::endl;
                snd_pcm_close(handle_audio);
                handle_audio = nullptr;
                continue;
            }

            audio_buffer = new char[AUDIO_BUFFER_SIZE];
            std::cout << "Audio capture setup complete on " << audio_device_path << std::endl;
        }

        // Capture and write the audio chunk
        if (snd_pcm_readi(handle_audio, audio_buffer, frames) > 0) {
            if (ffmpeg_is_running && ffmpeg_pipe_audio) {
                std::lock_guard<std::mutex> lock(mtx);
                fwrite(audio_buffer, 1, AUDIO_BUFFER_SIZE, ffmpeg_pipe_audio);
                fflush(ffmpeg_pipe_audio); // Important
            }
        } else {
            // Error or device disconnected
            std::cerr << "Failed to read audio frame. Device may have been disconnected." << std::endl;
            if (handle_audio != nullptr) {
                snd_pcm_close(handle_audio);
                handle_audio = nullptr;
                delete[] audio_buffer;
                audio_buffer = nullptr;
                mic_connected = false; // Mark as disconnected to trigger reconnection logic
            }
        }
    }

    // Cleanup on exit
    if (handle_audio != nullptr) {
        snd_pcm_close(handle_audio);
    }
    if (audio_buffer) {
        delete[] audio_buffer;
    }
    std::cout << "Audio capture thread terminated." << std::endl;
}

// --- Signal handler to stop gracefully ---
void signal_handler(int signum) {
    std::cout << "\nSignal " << signum << " received. Stopping gracefully..." << std::endl;
    running = false;
    stop_ffmpeg_process();
}

int main() {
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initial device check
    check_and_update_devices();

    // Start a thread to monitor for device changes
    std::thread device_monitor_thread([&]() {
        while (running) {
            check_and_update_devices();
            // Check for devices and start/stop recording if needed
            if (webcam_connected && mic_connected && !ffmpeg_is_running) {
                start_ffmpeg_process();
            } else if ((!webcam_connected || !mic_connected) && ffmpeg_is_running) {
                stop_ffmpeg_process();
            }
            std::this_thread::sleep_for(std::chrono::seconds(2)); // Check every 2 seconds
        }
    });

    // Start capture threads
    std::thread video_thread(video_capture_thread);
    std::thread audio_thread(audio_capture_thread);

    // Main loop to keep the program alive
    while (running) {
        // The main thread can do other work or just sleep
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Join threads before exiting
    if (video_thread.joinable()) {
        video_thread.join();
    }
    if (audio_thread.joinable()) {
        audio_thread.join();
    }
    if (device_monitor_thread.joinable()) {
        device_monitor_thread.join();
    }

    std::cout << "Program exited." << std::endl;
    return 0;
}

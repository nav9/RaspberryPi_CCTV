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
#include <regex> // For parsing command output
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <linux/videodev2.h>
#include <alsa/asoundlib.h>
#include <libudev.h>
#include <algorithm>

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

// --- Configurable Parameters (now dynamic) ---
int VIDEO_WIDTH = 0;
int VIDEO_HEIGHT = 0;
int VIDEO_FPS = 0;
int VIDEO_FRAME_SIZE = 0;
__u32 VIDEO_PIXEL_FORMAT = 0;
std::string VIDEO_FFMPEG_PIX_FMT = "";

int AUDIO_SAMPLE_RATE = 0;
int AUDIO_CHANNELS = 0;
int AUDIO_BITS_PER_SAMPLE = 0;
int AUDIO_BUFFER_SIZE = 0;
const int AUDIO_BUFFER_SIZE_MS = 20; // 20ms audio buffer chunk

// --- System Thresholds ---
const long long MIN_DISK_SPACE_MB = 100; // Minimum 100 MB free disk space
const long long MIN_RAM_MB = 50; // Minimum 50 MB free RAM

// --- Data structure to hold a video format configuration ---
struct VideoFormatConfig {
    __u32 pixel_format;
    unsigned int width;
    unsigned int height;
    float fps;
};

// --- Custom comparator for sorting video formats (prioritizes FPS, then resolution) ---
bool compareVideoFormatConfigs(const VideoFormatConfig& a, const VideoFormatConfig& b) {
    if (a.fps != b.fps) {
        return a.fps > b.fps; // Higher FPS is better
    }
    return (a.width * a.height) > (b.width * b.height); // Then, more pixels
}

// --- Helper function to convert a fourcc code to a string ---
std::string fourcc_to_string(__u32 fourcc) {
    char s[5];
    s[0] = fourcc & 0xFF;
    s[1] = (fourcc >> 8) & 0xFF;
    s[2] = (fourcc >> 16) & 0xFF;
    s[3] = (fourcc >> 24) & 0xFF;
    s[4] = '\0';
    return std::string(s);
}

// --- Video Format Enumerator Class (Modular & Flexible) ---
class VideoFormatEnumerator {
public:
    // Tries to enumerate using a shell command first, then falls back to ioctl
    static std::vector<VideoFormatConfig> enumerate_all_formats(const std::string& device_path) {
        std::vector<VideoFormatConfig> configs = enumerate_from_shell(device_path);
        if (!configs.empty()) {
            std::cout << "Successfully enumerated formats using 'v4l2-ctl' command." << std::endl;
            return configs;
        }

        std::cout << "Falling back to direct ioctl enumeration..." << std::endl;
        int fd = open(device_path.c_str(), O_RDWR);
        if (fd < 0) {
            std::cerr << "Error: Could not open video device for ioctl enumeration." << std::endl;
            return {};
        }
        configs = enumerate_from_ioctl(fd);
        close(fd);
        if (!configs.empty()) {
            std::cout << "Successfully enumerated formats using direct ioctl." << std::endl;
        } else {
            std::cerr << "Error: Failed to enumerate formats using both methods." << std::endl;
        }
        return configs;
    }

private:
    // Failsafe method: executes v4l2-ctl and parses its output
    static std::vector<VideoFormatConfig> enumerate_from_shell(const std::string& device_path) {
        std::vector<VideoFormatConfig> configs;
        std::string command = "v4l2-ctl --list-formats-ext -d " + device_path + " 2>&1";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            std::cerr << "Warning: 'v4l2-ctl' not found or failed to execute. Proceeding with ioctl fallback." << std::endl;
            return {};
        }
        
        char buffer[128];
        std::string result = "";
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);

        // Regular expressions for parsing
        std::regex format_regex("\\[\\d+\\]: '([A-Z0-9]{4})' .*");
        std::regex size_regex("Size: Discrete (\\d+)x(\\d+)");
        std::regex interval_regex("Interval: Discrete [0-9\\.]+s \\(([0-9\\.]+) fps\\)");
        
        std::smatch match;
        std::istringstream stream(result);
        std::string line;
        
        __u32 current_fourcc = 0;
        unsigned int current_width = 0;
        unsigned int current_height = 0;

        while (std::getline(stream, line)) {
            // Match for format
            if (std::regex_search(line, match, format_regex) && match.size() > 1) {
                std::string fourcc_str = match.str(1);
                current_fourcc = v4l2_fourcc(fourcc_str[0], fourcc_str[1], fourcc_str[2], fourcc_str[3]);
                current_width = 0;
                current_height = 0;
            }
            
            // Match for size
            if (std::regex_search(line, match, size_regex) && match.size() > 2) {
                current_width = std::stoi(match.str(1));
                current_height = std::stoi(match.str(2));
            }
            
            // Match for interval (and build config)
            if (std::regex_search(line, match, interval_regex) && match.size() > 1) {
                if (current_fourcc != 0 && current_width != 0 && current_height != 0) {
                    float fps = std::stof(match.str(1));
                    configs.push_back({current_fourcc, current_width, current_height, fps});
                }
            }
        }
        return configs;
    }
    
    // Direct ioctl method (used as a fallback)
    static std::vector<VideoFormatConfig> enumerate_from_ioctl(int fd) {
        std::vector<VideoFormatConfig> configs;
        v4l2_fmtdesc fmtdesc;
        memset(&fmtdesc, 0, sizeof(fmtdesc));
        fmtdesc.index = 0;
        fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) >= 0) {
            v4l2_frmsizeenum frmsize;
            memset(&frmsize, 0, sizeof(frmsize));
            frmsize.index = 0;
            frmsize.pixel_format = fmtdesc.pixelformat;
            while (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) >= 0) {
                if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                    v4l2_frmivalenum frmival;
                    memset(&frmival, 0, sizeof(frmival));
                    frmival.index = 0;
                    frmival.pixel_format = frmsize.pixel_format;
                    frmival.width = frmsize.discrete.width;
                    frmival.height = frmsize.discrete.height;
                    while (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) >= 0) {
                        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                            float fps = static_cast<float>(frmival.discrete.denominator) / frmival.discrete.numerator;
                            configs.push_back({frmsize.pixel_format, frmsize.discrete.width, frmsize.discrete.height, fps});
                        }
                        frmival.index++;
                    }
                }
                frmsize.index++;
            }
            fmtdesc.index++;
        }
        return configs;
    }
};

// --- Helper function to execute a shell command ---
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
        if (webcam_connected) { std::cout << "Webcam disconnected." << std::endl; webcam_connected = false; }
    } else {
        if (!webcam_connected || new_video_path != video_device_path) {
            std::cout << "Webcam connected/re-connected at: " << new_video_path << std::endl;
            webcam_connected = true; video_device_path = new_video_path;
        }
    }
    if (new_audio_path.empty()) {
        if (mic_connected) { std::cout << "Mic disconnected." << std::endl; mic_connected = false; }
    } else {
        if (!mic_connected || new_audio_path != audio_device_path) {
            std::cout << "Mic connected/re-connected at: " << new_audio_path << std::endl;
            mic_connected = true; audio_device_path = new_audio_path;
        }
    }
}

// --- Auto-detect and set video format ---
bool setup_video_capture(int fd) {
    // Phase 1: Enumerate all supported formats using the robust enumerator
    std::vector<VideoFormatConfig> supported_configs = VideoFormatEnumerator::enumerate_all_formats(video_device_path);
    
    if (supported_configs.empty()) {
        std::cerr << "Error: No supported video formats found by any method." << std::endl;
        return false;
    }

    // Phase 2: Sort the configurations to find the best one
    std::sort(supported_configs.begin(), supported_configs.end(), compareVideoFormatConfigs);
    std::cout << "Found " << supported_configs.size() << " valid configurations. Trying to set the best one..." << std::endl;
    
    // Phase 3: Try to set the configurations in order of preference
    v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    
    bool format_set = false;
    for (const auto& config : supported_configs) {
        format.fmt.pix.pixelformat = config.pixel_format;
        format.fmt.pix.width = config.width;
        format.fmt.pix.height = config.height;
        
        std::cout << "Attempting to set: " << config.width << "x" << config.height
                  << " @ " << std::fixed << std::setprecision(2) << config.fps << "fps, format: " << fourcc_to_string(config.pixel_format) << "... ";
        
        if (ioctl(fd, VIDIOC_S_FMT, &format) == 0) {
            v4l2_streamparm parm;
            memset(&parm, 0, sizeof(parm));
            parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            parm.parm.capture.timeperframe.numerator = 1;
            parm.parm.capture.timeperframe.denominator = static_cast<unsigned int>(config.fps);
            
            if (ioctl(fd, VIDIOC_S_PARM, &parm) == 0) {
                VIDEO_WIDTH = format.fmt.pix.width;
                VIDEO_HEIGHT = format.fmt.pix.height;
                VIDEO_PIXEL_FORMAT = format.fmt.pix.pixelformat;
                VIDEO_FPS = static_cast<int>(config.fps);
                format_set = true;
                std::cout << "SUCCESS!" << std::endl;
                break;
            }
        }
        std::cout << "FAILED." << std::endl;
    }

    if (!format_set) {
        std::cerr << "Error: Could not set a supported video format from the enumerated list." << std::endl;
        return false;
    }

    // Phase 4: Determine FFmpeg pixel format string and buffer size
    if (VIDEO_PIXEL_FORMAT == v4l2_fourcc('Y', 'U', 'Y', 'V')) {
        VIDEO_FFMPEG_PIX_FMT = "yuyv422";
        VIDEO_FRAME_SIZE = VIDEO_WIDTH * VIDEO_HEIGHT * 2;
    } else if (VIDEO_PIXEL_FORMAT == v4l2_fourcc('M', 'J', 'P', 'G')) {
        VIDEO_FFMPEG_PIX_FMT = "mjpeg";
        VIDEO_FRAME_SIZE = VIDEO_WIDTH * VIDEO_HEIGHT;
        if (VIDEO_FRAME_SIZE < 100 * 1024) VIDEO_FRAME_SIZE = 100 * 1024;
    } else {
        std::cerr << "Warning: Unhandled pixel format " << fourcc_to_string(VIDEO_PIXEL_FORMAT) << ". Using a raw video fallback." << std::endl;
        VIDEO_FFMPEG_PIX_FMT = "rawvideo";
        VIDEO_FRAME_SIZE = VIDEO_WIDTH * VIDEO_HEIGHT * 4;
    }

    std::cout << "Video capture successfully configured: " << VIDEO_WIDTH << "x" << VIDEO_HEIGHT
              << " @ " << VIDEO_FPS << "fps, format: " << VIDEO_FFMPEG_PIX_FMT << std::endl;

    return true;
}

// --- Audio setup, resource checks, FFmpeg process management, and threads (unchanged) ---
bool setup_audio_capture(snd_pcm_t* handle) {
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    if (snd_pcm_hw_params_any(handle, params) < 0) { std::cerr << "Error: Failed to get hardware parameters for audio device." << std::endl; return false; }
    const std::vector<snd_pcm_access_t> preferred_access = {SND_PCM_ACCESS_RW_INTERLEAVED};
    const std::vector<int> preferred_channels = {1, 2};
    const std::vector<snd_pcm_format_t> preferred_formats = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S32_LE};
    const std::vector<unsigned int> preferred_rates = {44100, 48000, 16000};
    bool params_set = false;
    for (snd_pcm_access_t access : preferred_access) { if (snd_pcm_hw_params_set_access(handle, params, access) < 0) continue;
        for (int channels : preferred_channels) { if (snd_pcm_hw_params_set_channels(handle, params, channels) < 0) continue;
            for (snd_pcm_format_t format : preferred_formats) { if (snd_pcm_hw_params_set_format(handle, params, format) < 0) continue;
                for (unsigned int rate : preferred_rates) { unsigned int actual_rate = rate; int dir;
                    if (snd_pcm_hw_params_set_rate_near(handle, params, &actual_rate, &dir) >= 0) {
                        if (snd_pcm_hw_params(handle, params) >= 0) { AUDIO_SAMPLE_RATE = actual_rate; AUDIO_CHANNELS = channels; AUDIO_BITS_PER_SAMPLE = snd_pcm_format_width(format); params_set = true; break; } } }
                if (params_set) break; } if (params_set) break; } if (params_set) break; }
    if (!params_set) { std::cerr << "Error: Could not set a supported audio format and rate." << std::endl; return false; }
    snd_pcm_uframes_t frames = (AUDIO_SAMPLE_RATE / 1000) * AUDIO_BUFFER_SIZE_MS; int dir;
    if (snd_pcm_hw_params_set_period_size_near(handle, params, &frames, &dir) < 0) { std::cerr << "Warning: Failed to set audio period size. Using device default." << std::endl; }
    if (snd_pcm_hw_params(handle, params) < 0) { std::cerr << "Error: Failed to set finalized audio parameters." << std::endl; return false; }
    AUDIO_BUFFER_SIZE = frames * (AUDIO_BITS_PER_SAMPLE / 8) * AUDIO_CHANNELS;
    std::cout << "Audio capture configured: " << AUDIO_SAMPLE_RATE << " Hz, " << AUDIO_CHANNELS << " channels, " << AUDIO_BITS_PER_SAMPLE << " bits per sample." << std::endl; return true;
}

bool check_system_resources(const std::string& directory) {
    struct statvfs disk_info;
    if (statvfs(directory.c_str(), &disk_info) != 0) { std::cerr << "Error: Failed to get disk information for " << directory << ". Proceeding with caution." << std::endl; } else {
        long long free_bytes = (long long)disk_info.f_bavail * disk_info.f_frsize; long long free_mb = free_bytes / (1024 * 1024);
        std::cout << "Available disk space: " << free_mb << " MB" << std::endl;
        if (free_mb < MIN_DISK_SPACE_MB) { std::cerr << "Warning: Low disk space (" << free_mb << " MB). Recording will be stopped." << std::endl; return false; } }
    struct sysinfo mem_info;
    if (sysinfo(&mem_info) != 0) { std::cerr << "Error: Failed to get RAM information. Proceeding with caution." << std::endl; } else {
        long long free_ram_bytes = mem_info.freeram; long long free_ram_mb = free_ram_bytes / (1024 * 1024);
        std::cout << "Available RAM: " << free_ram_mb << " MB" << std::endl;
        if (free_ram_mb < MIN_RAM_MB) { std::cerr << "Warning: Low RAM (" << free_ram_mb << " MB). Recording will be stopped." << std::endl; return false; } }
    return true;
}

void start_ffmpeg_process() {
    std::lock_guard<std::mutex> lock(mtx);
    if (ffmpeg_is_running) { std::cout << "FFmpeg is already running." << std::endl; return; }
    if (!webcam_connected || !mic_connected) { std::cout << "Cannot start recording: Webcam and/or mic not connected." << std::endl; return; }
    if (!check_system_resources("./recordings")) { std::cerr << "Resource check failed. Not starting recording." << std::endl; return; }
    auto now = std::chrono::system_clock::now(); std::time_t now_c = std::chrono::system_clock::to_time_t(now); std::stringstream ss; ss << std::put_time(std::localtime(&now_c), "%Y-%m-%d_%H-%M-%S");
    std::string filename = "./recordings/footages_" + ss.str() + ".mp4"; run_command("mkdir -p ./recordings");
    std::string ffmpeg_cmd = "ffmpeg -y -f rawvideo -pix_fmt " + VIDEO_FFMPEG_PIX_FMT + " -s " + std::to_string(VIDEO_WIDTH) + "x" + std::to_string(VIDEO_HEIGHT) + " -r " + std::to_string(VIDEO_FPS) + " -i - " +
                             "-f alsa -ac " + std::to_string(AUDIO_CHANNELS) + " -ar " + std::to_string(AUDIO_SAMPLE_RATE) + " -i default " +
                             "-c:v h264_omx -b:v 2M -c:a aac -b:a 128k -f mp4 " + filename;
    std::cout << "Starting FFmpeg command: " << ffmpeg_cmd << std::endl;
    ffmpeg_pipe_video = popen(ffmpeg_cmd.c_str(), "w");
    if (!ffmpeg_pipe_video) { std::cerr << "Failed to open pipe to FFmpeg." << std::endl; return; }
    ffmpeg_is_running = true; std::cout << "FFmpeg process started, recording to " << filename << std::endl;
}

void stop_ffmpeg_process() {
    std::lock_guard<std::mutex> lock(mtx);
    if (!ffmpeg_is_running) { std::cout << "FFmpeg is not running." << std::endl; return; }
    std::cout << "Stopping FFmpeg process..." << std::endl;
    if (ffmpeg_pipe_video) { pclose(ffmpeg_pipe_video); ffmpeg_pipe_video = nullptr; }
    ffmpeg_is_running = false; std::cout << "FFmpeg process stopped." << std::endl;
}

void video_capture_thread() {
    int fd_video = -1; char* video_buffer = nullptr;
    while (running) {
        if (!webcam_connected) { if (fd_video != -1) { close(fd_video); fd_video = -1; } if (video_buffer) { delete[] video_buffer; video_buffer = nullptr; } std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
        if (fd_video == -1) {
            fd_video = open(video_device_path.c_str(), O_RDWR);
            if (fd_video < 0) { std::cerr << "Failed to open video device " << video_device_path << ". Retrying..." << std::endl; std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
            if (!setup_video_capture(fd_video)) { close(fd_video); fd_video = -1; webcam_connected = false; continue; }
            video_buffer = new (std::nothrow) char[VIDEO_FRAME_SIZE];
            if (!video_buffer) { std::cerr << "Failed to allocate video buffer memory." << std::endl; close(fd_video); fd_video = -1; webcam_connected = false; continue; } }
        ssize_t bytes_read = read(fd_video, video_buffer, VIDEO_FRAME_SIZE);
        if (bytes_read > 0) { if (ffmpeg_is_running && ffmpeg_pipe_video) { std::lock_guard<std::mutex> lock(mtx); fwrite(video_buffer, 1, bytes_read, ffmpeg_pipe_video); fflush(ffmpeg_pipe_video); } }
        else if (bytes_read == 0) { std::cerr << "Video device unplugged or read error." << std::endl; close(fd_video); fd_video = -1; delete[] video_buffer; video_buffer = nullptr; webcam_connected = false; }
        else if (bytes_read < 0) { if (errno == EAGAIN || errno == EINTR) { continue; } std::cerr << "Failed to read video frame: " << strerror(errno) << std::endl; close(fd_video); fd_video = -1; delete[] video_buffer; video_buffer = nullptr; webcam_connected = false; } }
    if (fd_video != -1) close(fd_video); if (video_buffer) delete[] video_buffer; std::cout << "Video capture thread terminated." << std::endl;
}

void audio_capture_thread() {
    snd_pcm_t* handle_audio = nullptr; char* audio_buffer = nullptr;
    while (running) {
        if (!mic_connected) { if (handle_audio != nullptr) { snd_pcm_close(handle_audio); handle_audio = nullptr; } if (audio_buffer) { delete[] audio_buffer; audio_buffer = nullptr; } std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
        if (handle_audio == nullptr) {
            if (snd_pcm_open(&handle_audio, audio_device_path.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) { std::cerr << "Failed to open audio device " << audio_device_path << ". Retrying..." << std::endl; std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
            if (!setup_audio_capture(handle_audio)) { snd_pcm_close(handle_audio); handle_audio = nullptr; mic_connected = false; continue; }
            audio_buffer = new (std::nothrow) char[AUDIO_BUFFER_SIZE];
            if (!audio_buffer) { std::cerr << "Failed to allocate audio buffer memory." << std::endl; snd_pcm_close(handle_audio); handle_audio = nullptr; mic_connected = false; continue; } }
        if (snd_pcm_readi(handle_audio, audio_buffer, AUDIO_BUFFER_SIZE / (AUDIO_BITS_PER_SAMPLE / 8 * AUDIO_CHANNELS)) > 0) {
            if (ffmpeg_is_running && ffmpeg_pipe_video) { std::lock_guard<std::mutex> lock(mtx); fwrite(audio_buffer, 1, AUDIO_BUFFER_SIZE, ffmpeg_pipe_video); fflush(ffmpeg_pipe_video); } }
        else { std::cerr << "Failed to read audio frame. Device may have been disconnected: " << snd_strerror(snd_pcm_readi(handle_audio, audio_buffer, AUDIO_BUFFER_SIZE / (AUDIO_BITS_PER_SAMPLE / 8 * AUDIO_CHANNELS))) << std::endl; snd_pcm_close(handle_audio); handle_audio = nullptr; delete[] audio_buffer; audio_buffer = nullptr; mic_connected = false; } }
    if (handle_audio != nullptr) snd_pcm_close(handle_audio); if (audio_buffer) delete[] audio_buffer; std::cout << "Audio capture thread terminated." << std::endl;
}

// --- Signal handler and main function (unchanged) ---
void signal_handler(int signum) {
    std::cout << "\nSignal " << signum << " received. Stopping gracefully..." << std::endl;
    running = false; stop_ffmpeg_process();
}

int main() {
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    check_and_update_devices();
    std::thread device_monitor_thread([&]() {
        while (running) {
            check_and_update_devices();
            if (webcam_connected && mic_connected && !ffmpeg_is_running) { start_ffmpeg_process(); }
            else if ((!webcam_connected || !mic_connected) && ffmpeg_is_running) { stop_ffmpeg_process(); }
            if (ffmpeg_is_running && !check_system_resources("./recordings")) { std::cerr << "System resource limit reached. Stopping recording." << std::endl; stop_ffmpeg_process(); }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    });
    std::thread video_thread(video_capture_thread); std::thread audio_thread(audio_capture_thread);
    while (running) { std::this_thread::sleep_for(std::chrono::seconds(1)); }
    if (video_thread.joinable()) { video_thread.join(); } if (audio_thread.joinable()) { audio_thread.join(); }
    if (device_monitor_thread.joinable()) { device_monitor_thread.join(); }
    std::cout << "Program exited." << std::endl; return 0;
}

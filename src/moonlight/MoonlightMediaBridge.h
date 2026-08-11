#pragma once

#include "media/MediaSender.h"

#include <Limelight.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gateway {

struct MoonlightVideoDiagnostics {
    int frameNumber;
    int frameType;
    std::uint32_t rtpTimestamp;
    std::uint64_t presentationTimeUs;
    std::uint64_t receiveTimeUs;
    std::uint64_t enqueueTimeUs;
    std::size_t bytes;
};

class MoonlightMediaBridge {
public:
    using Logger = std::function<void(const std::string&)>;

    MoonlightMediaBridge(MediaSender& sender, Logger logger);
    ~MoonlightMediaBridge();

    MoonlightMediaBridge(const MoonlightMediaBridge&) = delete;
    MoonlightMediaBridge& operator=(const MoonlightMediaBridge&) = delete;

    DECODER_RENDERER_CALLBACKS* videoCallbacks();
    AUDIO_RENDERER_CALLBACKS* audioCallbacks();

    void onWebRtcKeyframeRequest();
    [[nodiscard]] bool hasPendingIdrRequest() const;
    [[nodiscard]] std::optional<MoonlightVideoDiagnostics> lastVideoFrame() const;

private:
    static int videoSetupCallback(int videoFormat,
                                  int width,
                                  int height,
                                  int redrawRate,
                                  void* context,
                                  int drFlags);
    static void videoStartCallback();
    static void videoStopCallback();
    static void videoCleanupCallback();
    static int videoSubmitCallback(PDECODE_UNIT decodeUnit);

    static int audioInitCallback(int audioConfiguration,
                                 const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                                 void* context,
                                 int arFlags);
    static void audioStartCallback();
    static void audioStopCallback();
    static void audioCleanupCallback();
    static void audioSubmitCallback(char* sampleData, int sampleLength);

    int setupVideo(int videoFormat, int width, int height, int redrawRate, int drFlags);
    int submitVideo(PDECODE_UNIT decodeUnit);
    int setupAudio(const POPUS_MULTISTREAM_CONFIGURATION opusConfig, int arFlags);
    void submitAudio(char* sampleData, int sampleLength);
    std::optional<std::vector<std::uint8_t>> flattenDecodeUnit(PDECODE_UNIT decodeUnit);
    void log(const std::string& message);

    static std::atomic<MoonlightMediaBridge*> activeVideoBridge_;
    static std::atomic<MoonlightMediaBridge*> activeAudioBridge_;

    MediaSender& sender_;
    Logger logger_;
    DECODER_RENDERER_CALLBACKS videoCallbacks_{};
    AUDIO_RENDERER_CALLBACKS audioCallbacks_{};

    std::atomic<bool> videoConfigured_ = false;
    std::atomic<bool> videoStarted_ = false;
    std::atomic<bool> audioConfigured_ = false;
    std::atomic<bool> audioStarted_ = false;
    std::atomic<bool> moonlightIdrRequested_ = false;
    std::atomic<int> audioSamplesPerFrame_ = 0;
    std::atomic<std::uint32_t> audioRtpTimestamp_ = 0;
    std::atomic<std::uint64_t> videoFrames_ = 0;
    std::atomic<std::uint64_t> videoBytes_ = 0;
    std::atomic<std::uint64_t> audioPackets_ = 0;
    std::atomic<std::uint64_t> audioBytes_ = 0;

    mutable std::mutex diagnosticsMutex_;
    std::optional<MoonlightVideoDiagnostics> lastVideoFrame_;
    std::chrono::steady_clock::time_point nextVideoLog_{};
    std::chrono::steady_clock::time_point nextAudioLog_{};
    std::mutex logMutex_;
};

} // namespace gateway

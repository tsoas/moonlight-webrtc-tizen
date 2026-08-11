#include "moonlight/MoonlightMediaBridge.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gateway {

std::atomic<MoonlightMediaBridge*> MoonlightMediaBridge::activeVideoBridge_ = nullptr;
std::atomic<MoonlightMediaBridge*> MoonlightMediaBridge::activeAudioBridge_ = nullptr;

MoonlightMediaBridge::MoonlightMediaBridge(MediaSender& sender, Logger logger)
    : sender_(sender)
    , logger_(std::move(logger))
{
    LiInitializeVideoCallbacks(&videoCallbacks_);
    videoCallbacks_.setup = &MoonlightMediaBridge::videoSetupCallback;
    videoCallbacks_.start = &MoonlightMediaBridge::videoStartCallback;
    videoCallbacks_.stop = &MoonlightMediaBridge::videoStopCallback;
    videoCallbacks_.cleanup = &MoonlightMediaBridge::videoCleanupCallback;
    videoCallbacks_.submitDecodeUnit = &MoonlightMediaBridge::videoSubmitCallback;

    LiInitializeAudioCallbacks(&audioCallbacks_);
    audioCallbacks_.init = &MoonlightMediaBridge::audioInitCallback;
    audioCallbacks_.start = &MoonlightMediaBridge::audioStartCallback;
    audioCallbacks_.stop = &MoonlightMediaBridge::audioStopCallback;
    audioCallbacks_.cleanup = &MoonlightMediaBridge::audioCleanupCallback;
    audioCallbacks_.decodeAndPlaySample = &MoonlightMediaBridge::audioSubmitCallback;
    audioCallbacks_.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
}

MoonlightMediaBridge::~MoonlightMediaBridge()
{
    auto* expected = this;
    activeVideoBridge_.compare_exchange_strong(expected, nullptr);
    expected = this;
    activeAudioBridge_.compare_exchange_strong(expected, nullptr);
}

DECODER_RENDERER_CALLBACKS* MoonlightMediaBridge::videoCallbacks()
{
    return &videoCallbacks_;
}

AUDIO_RENDERER_CALLBACKS* MoonlightMediaBridge::audioCallbacks()
{
    return &audioCallbacks_;
}

void MoonlightMediaBridge::onWebRtcKeyframeRequest()
{
    moonlightIdrRequested_.store(true, std::memory_order_release);
}

bool MoonlightMediaBridge::hasPendingIdrRequest() const
{
    return moonlightIdrRequested_.load(std::memory_order_acquire);
}

std::optional<MoonlightVideoDiagnostics> MoonlightMediaBridge::lastVideoFrame() const
{
    const std::lock_guard lock(diagnosticsMutex_);
    return lastVideoFrame_;
}

int MoonlightMediaBridge::videoSetupCallback(int videoFormat,
                                              int width,
                                              int height,
                                              int redrawRate,
                                              void* context,
                                              int drFlags)
{
    auto* bridge = static_cast<MoonlightMediaBridge*>(context);
    if (!bridge) {
        return -1;
    }

    activeVideoBridge_.store(bridge, std::memory_order_release);
    return bridge->setupVideo(videoFormat, width, height, redrawRate, drFlags);
}

void MoonlightMediaBridge::videoStartCallback()
{
    if (auto* bridge = activeVideoBridge_.load(std::memory_order_acquire)) {
        bridge->videoStarted_.store(true, std::memory_order_release);
        const std::lock_guard lock(bridge->diagnosticsMutex_);
        bridge->nextVideoLog_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
}

void MoonlightMediaBridge::videoStopCallback()
{
    if (auto* bridge = activeVideoBridge_.load(std::memory_order_acquire)) {
        bridge->videoStarted_.store(false, std::memory_order_release);
    }
}

void MoonlightMediaBridge::videoCleanupCallback()
{
    if (auto* bridge = activeVideoBridge_.exchange(nullptr, std::memory_order_acq_rel)) {
        bridge->videoStarted_.store(false, std::memory_order_release);
        bridge->videoConfigured_.store(false, std::memory_order_release);
    }
}

int MoonlightMediaBridge::videoSubmitCallback(PDECODE_UNIT decodeUnit)
{
    auto* bridge = activeVideoBridge_.load(std::memory_order_acquire);
    if (!bridge) {
        return DR_NEED_IDR;
    }

    try {
        return bridge->submitVideo(decodeUnit);
    } catch (const std::exception& error) {
        bridge->log("Moonlight video callback error: " + std::string(error.what()));
        return DR_NEED_IDR;
    } catch (...) {
        bridge->log("Moonlight video callback error: unknown exception");
        return DR_NEED_IDR;
    }
}

int MoonlightMediaBridge::audioInitCallback(
    int,
    const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
    void* context,
    int arFlags)
{
    auto* bridge = static_cast<MoonlightMediaBridge*>(context);
    if (!bridge) {
        return -1;
    }

    activeAudioBridge_.store(bridge, std::memory_order_release);
    return bridge->setupAudio(opusConfig, arFlags);
}

void MoonlightMediaBridge::audioStartCallback()
{
    if (auto* bridge = activeAudioBridge_.load(std::memory_order_acquire)) {
        bridge->audioStarted_.store(true, std::memory_order_release);
        const std::lock_guard lock(bridge->diagnosticsMutex_);
        bridge->nextAudioLog_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }
}

void MoonlightMediaBridge::audioStopCallback()
{
    if (auto* bridge = activeAudioBridge_.load(std::memory_order_acquire)) {
        bridge->audioStarted_.store(false, std::memory_order_release);
    }
}

void MoonlightMediaBridge::audioCleanupCallback()
{
    if (auto* bridge = activeAudioBridge_.exchange(nullptr, std::memory_order_acq_rel)) {
        bridge->audioStarted_.store(false, std::memory_order_release);
        bridge->audioConfigured_.store(false, std::memory_order_release);
    }
}

void MoonlightMediaBridge::audioSubmitCallback(char* sampleData, int sampleLength)
{
    auto* bridge = activeAudioBridge_.load(std::memory_order_acquire);
    if (!bridge) {
        return;
    }

    try {
        bridge->submitAudio(sampleData, sampleLength);
    } catch (const std::exception& error) {
        bridge->log("Moonlight audio callback error: " + std::string(error.what()));
    } catch (...) {
        bridge->log("Moonlight audio callback error: unknown exception");
    }
}

int MoonlightMediaBridge::setupVideo(int videoFormat,
                                      int width,
                                      int height,
                                      int redrawRate,
                                      int)
{
    std::ostringstream message;
    message << "Moonlight video setup: " << width << 'x' << height << " @ " << redrawRate
            << ", format=" << videoFormat;
    log(message.str());

    if (videoFormat != VIDEO_FORMAT_H264) {
        log("Unsupported Moonlight video format: " + std::to_string(videoFormat)
            + " (only H.264 is supported)");
        videoConfigured_.store(false, std::memory_order_release);
        return -1;
    }

    videoConfigured_.store(true, std::memory_order_release);
    return 0;
}

int MoonlightMediaBridge::submitVideo(PDECODE_UNIT decodeUnit)
{
    if (!videoConfigured_.load(std::memory_order_acquire)
        || !videoStarted_.load(std::memory_order_acquire) || !decodeUnit) {
        return DR_NEED_IDR;
    }

    const auto flattened = flattenDecodeUnit(decodeUnit);
    if (!flattened) {
        log("Invalid Moonlight video decode unit");
        return DR_NEED_IDR;
    }

    sender_.sendH264AccessUnit(*flattened, decodeUnit->rtpTimestamp);

    if (decodeUnit->frameType == FRAME_TYPE_IDR
        && waitingForMoonlightIdr_.exchange(false, std::memory_order_acq_rel)) {
        log("Moonlight IDR frame observed after keyframe request");
    }

    const auto frames = videoFrames_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto bytes = videoBytes_.fetch_add(flattened->size(), std::memory_order_relaxed)
        + flattened->size();
    const auto now = std::chrono::steady_clock::now();
    bool report = false;
    {
        const std::lock_guard lock(diagnosticsMutex_);
        lastVideoFrame_ = MoonlightVideoDiagnostics{
            decodeUnit->frameNumber,
            decodeUnit->frameType,
            decodeUnit->rtpTimestamp,
            decodeUnit->presentationTimeUs,
            decodeUnit->receiveTimeUs,
            decodeUnit->enqueueTimeUs,
            flattened->size(),
        };
        if (now >= nextVideoLog_) {
            nextVideoLog_ = now + std::chrono::seconds(1);
            report = true;
        }
    }

    if (report) {
        std::ostringstream message;
        message << "Moonlight video: " << frames << " frames, " << bytes
                << " bytes, last RTP=" << decodeUnit->rtpTimestamp;
        log(message.str());
    }

    if (moonlightIdrRequested_.exchange(false, std::memory_order_acq_rel)) {
        waitingForMoonlightIdr_.store(true, std::memory_order_release);
        log("WebRTC keyframe request forwarded to Moonlight");
        return DR_NEED_IDR;
    }

    return DR_OK;
}

int MoonlightMediaBridge::setupAudio(const POPUS_MULTISTREAM_CONFIGURATION opusConfig, int)
{
    if (!opusConfig) {
        log("Unsupported Moonlight audio configuration: missing Opus configuration");
        return -1;
    }

    std::ostringstream message;
    message << "Moonlight audio setup: " << opusConfig->sampleRate << " Hz, "
            << opusConfig->channelCount << " channels, " << opusConfig->samplesPerFrame
            << " samples/frame";
    log(message.str());

    if (opusConfig->sampleRate != 48000 || opusConfig->channelCount != 2
        || opusConfig->streams != 1 || opusConfig->coupledStreams != 1
        || opusConfig->samplesPerFrame <= 0) {
        log("Unsupported Moonlight audio configuration: expected 48000 Hz stereo Opus");
        audioConfigured_.store(false, std::memory_order_release);
        return -1;
    }

    audioSamplesPerFrame_.store(opusConfig->samplesPerFrame, std::memory_order_release);
    audioRtpTimestamp_.store(0, std::memory_order_release);
    audioConfigured_.store(true, std::memory_order_release);
    return 0;
}

void MoonlightMediaBridge::submitAudio(char* sampleData, int sampleLength)
{
    if (!audioConfigured_.load(std::memory_order_acquire)
        || !audioStarted_.load(std::memory_order_acquire) || !sampleData
        || sampleLength <= 0) {
        return;
    }

    const auto samplesPerFrame = audioSamplesPerFrame_.load(std::memory_order_acquire);
    const auto timestamp = audioRtpTimestamp_.fetch_add(
        static_cast<std::uint32_t>(samplesPerFrame), std::memory_order_acq_rel);
    sender_.sendOpusPacket(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(sampleData),
                                      static_cast<std::size_t>(sampleLength)),
        timestamp);

    const auto packets = audioPackets_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto bytes = audioBytes_.fetch_add(static_cast<std::size_t>(sampleLength),
                                             std::memory_order_relaxed)
        + static_cast<std::size_t>(sampleLength);
    const auto now = std::chrono::steady_clock::now();
    bool report = false;
    {
        const std::lock_guard lock(diagnosticsMutex_);
        if (now >= nextAudioLog_) {
            nextAudioLog_ = now + std::chrono::seconds(1);
            report = true;
        }
    }

    if (report) {
        std::ostringstream message;
        message << "Moonlight audio: " << packets << " packets, " << bytes << " bytes";
        log(message.str());
    }
}

std::optional<std::vector<std::uint8_t>> MoonlightMediaBridge::flattenDecodeUnit(
    PDECODE_UNIT decodeUnit)
{
    if (!decodeUnit || !decodeUnit->bufferList || decodeUnit->fullLength <= 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> flattened;
    flattened.reserve(static_cast<std::size_t>(decodeUnit->fullLength));

    for (auto* entry = decodeUnit->bufferList; entry; entry = entry->next) {
        if (!entry->data || entry->length <= 0) {
            return std::nullopt;
        }
        const auto* begin = reinterpret_cast<const std::uint8_t*>(entry->data);
        flattened.insert(flattened.end(), begin, begin + entry->length);
    }

    if (flattened.size() != static_cast<std::size_t>(decodeUnit->fullLength)) {
        return std::nullopt;
    }

    return flattened;
}

void MoonlightMediaBridge::log(const std::string& message)
{
    const std::lock_guard lock(logMutex_);
    if (logger_) {
        logger_(message);
    }
}

} // namespace gateway

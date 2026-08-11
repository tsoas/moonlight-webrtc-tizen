#include "moonlight/MoonlightMediaBridge.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct SentFrame {
    std::vector<std::uint8_t> data;
    std::uint32_t rtpTimestamp;
};

class RecordingMediaSender final : public gateway::MediaSender {
public:
    void sendH264AccessUnit(std::span<const std::uint8_t> accessUnit,
                            std::uint32_t rtpTimestamp) override
    {
        video.push_back({{accessUnit.begin(), accessUnit.end()}, rtpTimestamp});
    }

    void sendOpusPacket(std::span<const std::uint8_t> packet,
                        std::uint32_t rtpTimestamp) override
    {
        audio.push_back({{packet.begin(), packet.end()}, rtpTimestamp});
    }

    std::vector<SentFrame> video;
    std::vector<SentFrame> audio;
};

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main()
{
    try {
        RecordingMediaSender sender;
        std::vector<std::string> logs;
        gateway::MoonlightMediaBridge bridge(sender, [&logs](const std::string& message) {
            logs.push_back(message);
        });

        auto* videoCallbacks = bridge.videoCallbacks();
        require(videoCallbacks->setup(VIDEO_FORMAT_H265, 1280, 720, 60, &bridge, 0) != 0,
                "HEVC must be rejected");
        require(videoCallbacks->setup(VIDEO_FORMAT_H264, 1280, 720, 60, &bridge, 0) == 0,
                "H.264 setup failed");
        videoCallbacks->start();

        std::array<char, 6> sps = {0, 0, 0, 1, 0x67, 0x01};
        std::array<char, 6> pps = {0, 0, 0, 1, 0x68, 0x02};
        std::array<char, 7> idr = {0, 0, 0, 1, 0x65, 0x03, 0x04};
        LENTRY idrEntry{nullptr, idr.data(), static_cast<int>(idr.size()), BUFFER_TYPE_PICDATA};
        LENTRY ppsEntry{&idrEntry, pps.data(), static_cast<int>(pps.size()), BUFFER_TYPE_PPS};
        LENTRY spsEntry{&ppsEntry, sps.data(), static_cast<int>(sps.size()), BUFFER_TYPE_SPS};

        DECODE_UNIT idrUnit{};
        idrUnit.frameNumber = 42;
        idrUnit.frameType = FRAME_TYPE_IDR;
        idrUnit.receiveTimeUs = 1000;
        idrUnit.enqueueTimeUs = 2000;
        idrUnit.presentationTimeUs = 3000;
        idrUnit.rtpTimestamp = 123456;
        idrUnit.fullLength = static_cast<int>(sps.size() + pps.size() + idr.size());
        idrUnit.bufferList = &spsEntry;

        require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                "IDR decode unit did not return DR_OK");
        require(sender.video.size() == 1, "IDR decode unit was not forwarded");
        require(sender.video[0].rtpTimestamp == idrUnit.rtpTimestamp,
                "Moonlight video RTP timestamp changed");

        std::vector<std::uint8_t> expectedIdr;
        expectedIdr.insert(expectedIdr.end(), sps.begin(), sps.end());
        expectedIdr.insert(expectedIdr.end(), pps.begin(), pps.end());
        expectedIdr.insert(expectedIdr.end(), idr.begin(), idr.end());
        require(sender.video[0].data == expectedIdr,
                "DECODE_UNIT buffer chain was not flattened in order");

        const auto idrDiagnostics = bridge.lastVideoFrame();
        require(idrDiagnostics && idrDiagnostics->frameNumber == 42
                    && idrDiagnostics->frameType == FRAME_TYPE_IDR
                    && idrDiagnostics->presentationTimeUs == 3000
                    && idrDiagnostics->receiveTimeUs == 1000
                    && idrDiagnostics->enqueueTimeUs == 2000,
                "IDR diagnostics were not preserved");

        std::array<char, 7> pframe = {0, 0, 0, 1, 0x41, 0x05, 0x06};
        LENTRY pframeEntry{
            nullptr, pframe.data(), static_cast<int>(pframe.size()), BUFFER_TYPE_PICDATA};
        DECODE_UNIT pframeUnit{};
        pframeUnit.frameNumber = 43;
        pframeUnit.frameType = FRAME_TYPE_PFRAME;
        pframeUnit.rtpTimestamp = 124956;
        pframeUnit.fullLength = static_cast<int>(pframe.size());
        pframeUnit.bufferList = &pframeEntry;

        bridge.onWebRtcKeyframeRequest();
        require(bridge.hasPendingIdrRequest(), "IDR request flag was not set");
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_NEED_IDR,
                "WebRTC keyframe request did not produce DR_NEED_IDR");
        require(!bridge.hasPendingIdrRequest(), "IDR request flag was not consumed");
        require(videoCallbacks->submitDecodeUnit(&pframeUnit) == DR_OK,
                "IDR request flag was consumed more than once");

        const auto pframeDiagnostics = bridge.lastVideoFrame();
        require(pframeDiagnostics && pframeDiagnostics->frameType == FRAME_TYPE_PFRAME,
                "P-frame diagnostics were not preserved");

        auto* audioCallbacks = bridge.audioCallbacks();
        OPUS_MULTISTREAM_CONFIGURATION unsupportedAudio{};
        unsupportedAudio.sampleRate = 48000;
        unsupportedAudio.channelCount = 6;
        unsupportedAudio.samplesPerFrame = 960;
        require(audioCallbacks->init(0, &unsupportedAudio, &bridge, 0) != 0,
                "Surround Opus must be rejected");

        OPUS_MULTISTREAM_CONFIGURATION opusConfig{};
        opusConfig.sampleRate = 48000;
        opusConfig.channelCount = 2;
        opusConfig.streams = 1;
        opusConfig.coupledStreams = 1;
        opusConfig.samplesPerFrame = 960;
        opusConfig.mapping[0] = 0;
        opusConfig.mapping[1] = 1;
        require(audioCallbacks->init(0, &opusConfig, &bridge, 0) == 0,
                "Stereo Opus setup failed");
        audioCallbacks->start();

        std::array<char, 5> opusPacket = {0x01, 0x02, 0x03, 0x04, 0x05};
        audioCallbacks->decodeAndPlaySample(opusPacket.data(),
                                            static_cast<int>(opusPacket.size()));
        audioCallbacks->decodeAndPlaySample(opusPacket.data(),
                                            static_cast<int>(opusPacket.size()));

        require(sender.audio.size() == 2, "Encoded Opus packets were not forwarded");
        require(sender.audio[0].data
                    == std::vector<std::uint8_t>(opusPacket.begin(), opusPacket.end()),
                "Encoded Opus packet was modified");
        require(sender.audio[0].rtpTimestamp == 0 && sender.audio[1].rtpTimestamp == 960,
                "samplesPerFrame did not drive the Opus RTP timeline");

        bool keyframeLogFound = false;
        for (const auto& message : logs) {
            keyframeLogFound |= message == "WebRTC keyframe request forwarded to Moonlight";
        }
        require(keyframeLogFound, "Keyframe forwarding diagnostic was not logged");

        audioCallbacks->stop();
        audioCallbacks->cleanup();
        videoCallbacks->stop();
        videoCallbacks->cleanup();

        std::cout << "Moonlight media bridge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Moonlight media bridge test failed: " << error.what() << '\n';
        return 1;
    }
}

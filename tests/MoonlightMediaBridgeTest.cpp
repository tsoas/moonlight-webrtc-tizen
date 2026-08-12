#include "media/HevcSpsParser.h"
#include "moonlight/MoonlightMediaBridge.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct SentFrame {
    std::vector<std::uint8_t> data;
    std::uint32_t rtpTimestamp;
    gateway::VideoCodec codec;
};

class RecordingMediaSender final : public gateway::MediaSender {
public:
    void sendVideoAccessUnit(gateway::VideoCodec codec,
                             std::span<const std::uint8_t> accessUnit,
                             std::uint32_t rtpTimestamp) override
    {
        video.push_back({{accessUnit.begin(), accessUnit.end()}, rtpTimestamp, codec});
    }

    void sendOpusPacket(std::span<const std::uint8_t> packet,
                        std::uint32_t rtpTimestamp) override
    {
        audio.push_back(
            {{packet.begin(), packet.end()}, rtpTimestamp, gateway::VideoCodec::H264});
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

class BitWriter {
public:
    void writeBits(std::uint64_t value, int count)
    {
        for (int bit = count - 1; bit >= 0; --bit) {
            if (bitOffset_ == 0) {
                bytes_.push_back(0);
            }
            bytes_.back() |= static_cast<std::uint8_t>(
                ((value >> bit) & 1U) << (7 - bitOffset_));
            bitOffset_ = (bitOffset_ + 1) % 8;
        }
    }

    void writeUnsignedExpGolomb(std::uint32_t value)
    {
        const std::uint32_t code = value + 1;
        int bitCount = 0;
        for (auto current = code; current != 0; current >>= 1) {
            ++bitCount;
        }
        writeBits(0, bitCount - 1);
        writeBits(code, bitCount);
    }

    std::vector<std::uint8_t> take()
    {
        return std::move(bytes_);
    }

private:
    std::vector<std::uint8_t> bytes_;
    int bitOffset_ = 0;
};

std::vector<std::uint8_t> syntheticMain10Sps()
{
    BitWriter writer;
    writer.writeBits(0, 4);
    writer.writeBits(0, 3);
    writer.writeBits(1, 1);
    writer.writeBits(0, 2);
    writer.writeBits(0, 1);
    writer.writeBits(2, 5);
    writer.writeBits(0, 32);
    writer.writeBits(0, 48);
    writer.writeBits(120, 8);
    writer.writeUnsignedExpGolomb(0);
    writer.writeUnsignedExpGolomb(1);
    writer.writeUnsignedExpGolomb(1920);
    writer.writeUnsignedExpGolomb(1080);
    writer.writeBits(0, 1);
    writer.writeUnsignedExpGolomb(2);
    writer.writeUnsignedExpGolomb(2);

    const auto rbsp = writer.take();
    std::vector<std::uint8_t> nal{0, 0, 0, 1, 0x42, 0x01};
    int zeroCount = 0;
    for (const auto byte : rbsp) {
        if (zeroCount >= 2 && byte <= 3) {
            nal.push_back(3);
            zeroCount = 0;
        }
        nal.push_back(byte);
        zeroCount = byte == 0 ? zeroCount + 1 : 0;
    }
    return nal;
}

} // namespace

int main()
{
    try {
        RecordingMediaSender sender;
        std::vector<std::string> logs;
        gateway::MoonlightMediaBridge bridge(
            sender,
            gateway::defaultStreamSettings(),
            [&logs](const std::string& message) { logs.push_back(message); });

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
        require(sender.video[0].codec == gateway::VideoCodec::H264,
                "H.264 decode unit used the wrong sender codec");

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
        require(videoCallbacks->submitDecodeUnit(&idrUnit) == DR_OK,
                "Moonlight IDR after keyframe request was not accepted");

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
        bool idrObservedLogFound = false;
        for (const auto& message : logs) {
            keyframeLogFound |= message == "WebRTC keyframe request forwarded to Moonlight";
            idrObservedLogFound |=
                message == "Moonlight IDR frame observed after keyframe request";
        }
        require(keyframeLogFound, "Keyframe forwarding diagnostic was not logged");
        require(idrObservedLogFound, "Moonlight IDR observation was not logged");

        audioCallbacks->stop();
        audioCallbacks->cleanup();
        videoCallbacks->stop();
        videoCallbacks->cleanup();

        RecordingMediaSender hevcSender;
        auto hevcSettings = gateway::defaultStreamSettings(
            1920, 1080, gateway::VideoCodec::HEVC);
        gateway::MoonlightMediaBridge hevcBridge(
            hevcSender,
            hevcSettings,
            [&logs](const std::string& message) { logs.push_back(message); });
        auto* hevcVideoCallbacks = hevcBridge.videoCallbacks();
        require(hevcVideoCallbacks->setup(
                    VIDEO_FORMAT_H265_MAIN10, 1920, 1080, 60, &hevcBridge, 0)
                    != 0,
                "HEVC Main10 must be rejected for an HEVC Main session");
        require(hevcVideoCallbacks->setup(
                    VIDEO_FORMAT_H265, 1920, 1080, 60, &hevcBridge, 0)
                    == 0,
                "HEVC Main setup failed");
        hevcVideoCallbacks->start();

        std::array<char, 7> vps = {0, 0, 0, 1, 0x40, 0x01, 0x11};
        std::array<char, 7> hevcSps = {0, 0, 0, 1, 0x42, 0x01, 0x22};
        std::array<char, 7> hevcPps = {0, 0, 0, 1, 0x44, 0x01, 0x33};
        std::array<char, 8> hevcIdr = {0, 0, 0, 1, 0x26, 0x01, 0x44, 0x55};
        LENTRY hevcIdrEntry{
            nullptr,
            hevcIdr.data(),
            static_cast<int>(hevcIdr.size()),
            BUFFER_TYPE_PICDATA};
        LENTRY hevcPpsEntry{
            &hevcIdrEntry,
            hevcPps.data(),
            static_cast<int>(hevcPps.size()),
            BUFFER_TYPE_PPS};
        LENTRY hevcSpsEntry{
            &hevcPpsEntry,
            hevcSps.data(),
            static_cast<int>(hevcSps.size()),
            BUFFER_TYPE_SPS};
        LENTRY vpsEntry{
            &hevcSpsEntry,
            vps.data(),
            static_cast<int>(vps.size()),
            BUFFER_TYPE_VPS};
        DECODE_UNIT hevcIdrUnit{};
        hevcIdrUnit.frameNumber = 100;
        hevcIdrUnit.frameType = FRAME_TYPE_IDR;
        hevcIdrUnit.rtpTimestamp = 90000;
        hevcIdrUnit.fullLength = static_cast<int>(
            vps.size() + hevcSps.size() + hevcPps.size() + hevcIdr.size());
        hevcIdrUnit.bufferList = &vpsEntry;

        require(hevcVideoCallbacks->submitDecodeUnit(&hevcIdrUnit) == DR_OK,
                "HEVC access unit was rejected");
        std::vector<std::uint8_t> expectedHevc;
        expectedHevc.insert(expectedHevc.end(), vps.begin(), vps.end());
        expectedHevc.insert(expectedHevc.end(), hevcSps.begin(), hevcSps.end());
        expectedHevc.insert(expectedHevc.end(), hevcPps.begin(), hevcPps.end());
        expectedHevc.insert(expectedHevc.end(), hevcIdr.begin(), hevcIdr.end());
        require(hevcSender.video.size() == 1
                    && hevcSender.video[0].data == expectedHevc
                    && hevcSender.video[0].codec == gateway::VideoCodec::HEVC,
                "HEVC VPS/SPS/PPS/IDR access unit was modified or misclassified");
        require(hevcSender.video[0].rtpTimestamp == 90000,
                "HEVC Moonlight RTP timestamp changed");

        std::array<char, 7> hevcPframe = {0, 0, 0, 1, 0x02, 0x01, 0x66};
        LENTRY hevcPframeEntry{nullptr,
                               hevcPframe.data(),
                               static_cast<int>(hevcPframe.size()),
                               BUFFER_TYPE_PICDATA};
        DECODE_UNIT hevcPframeUnit{};
        hevcPframeUnit.frameNumber = 101;
        hevcPframeUnit.frameType = FRAME_TYPE_PFRAME;
        hevcPframeUnit.rtpTimestamp = 91500;
        hevcPframeUnit.fullLength = static_cast<int>(hevcPframe.size());
        hevcPframeUnit.bufferList = &hevcPframeEntry;
        hevcBridge.onWebRtcKeyframeRequest();
        require(hevcVideoCallbacks->submitDecodeUnit(&hevcPframeUnit) == DR_NEED_IDR,
                "HEVC PLI did not produce one DR_NEED_IDR");
        require(hevcVideoCallbacks->submitDecodeUnit(&hevcPframeUnit) == DR_OK,
                "HEVC PLI produced more than one DR_NEED_IDR");
        require(hevcVideoCallbacks->submitDecodeUnit(&hevcIdrUnit) == DR_OK,
                "HEVC keyframe after PLI was rejected");
        hevcVideoCallbacks->stop();
        hevcVideoCallbacks->cleanup();

        RecordingMediaSender hdrSender;
        auto hdrSettings = hevcSettings;
        hdrSettings.hdr = true;
        bool hdrValidationFailed = false;
        gateway::MoonlightMediaBridge hdrBridge(
            hdrSender,
            hdrSettings,
            [&logs](const std::string& message) { logs.push_back(message); },
            [&hdrValidationFailed](const std::string&) {
                hdrValidationFailed = true;
            });
        auto* hdrCallbacks = hdrBridge.videoCallbacks();
        require(hdrCallbacks->setup(
                    VIDEO_FORMAT_H265, 1920, 1080, 60, &hdrBridge, 0)
                    != 0,
                "HEVC Main must be rejected for an HDR session");
        require(hdrCallbacks->setup(
                    VIDEO_FORMAT_H265_MAIN10, 1920, 1080, 60, &hdrBridge, 0)
                    == 0,
                "HEVC Main10 HDR setup failed");
        hdrCallbacks->start();

        std::vector<std::uint8_t> hdrVps{0, 0, 0, 1, 0x40, 0x01, 0x11};
        auto hdrSps = syntheticMain10Sps();
        std::vector<std::uint8_t> hdrPps{0, 0, 0, 1, 0x44, 0x01, 0x22};
        std::vector<std::uint8_t> hdrSei{0, 0, 0, 1, 0x4e, 0x01, 0x33, 0x80};
        std::vector<std::uint8_t> hdrIdr{0, 0, 0, 1, 0x26, 0x01, 0x44, 0x55};

        const auto parsedSps = gateway::parseHevcSps(hdrSps);
        require(parsedSps && parsedSps->isMain10_420()
                    && parsedSps->profileIdc == 2
                    && parsedSps->bitDepthLuma == 10
                    && parsedSps->bitDepthChroma == 10,
                "Synthetic HEVC Main10 SPS parser test failed");

        LENTRY hdrIdrEntry{nullptr,
                           reinterpret_cast<char*>(hdrIdr.data()),
                           static_cast<int>(hdrIdr.size()),
                           BUFFER_TYPE_PICDATA};
        LENTRY hdrSeiEntry{&hdrIdrEntry,
                           reinterpret_cast<char*>(hdrSei.data()),
                           static_cast<int>(hdrSei.size()),
                           BUFFER_TYPE_PICDATA};
        LENTRY hdrPpsEntry{&hdrSeiEntry,
                           reinterpret_cast<char*>(hdrPps.data()),
                           static_cast<int>(hdrPps.size()),
                           BUFFER_TYPE_PPS};
        LENTRY hdrSpsEntry{&hdrPpsEntry,
                           reinterpret_cast<char*>(hdrSps.data()),
                           static_cast<int>(hdrSps.size()),
                           BUFFER_TYPE_SPS};
        LENTRY hdrVpsEntry{&hdrSpsEntry,
                           reinterpret_cast<char*>(hdrVps.data()),
                           static_cast<int>(hdrVps.size()),
                           BUFFER_TYPE_VPS};
        DECODE_UNIT hdrUnit{};
        hdrUnit.frameNumber = 200;
        hdrUnit.frameType = FRAME_TYPE_IDR;
        hdrUnit.rtpTimestamp = 180000;
        hdrUnit.hdrActive = true;
        hdrUnit.colorspace = COLORSPACE_REC_2020;
        hdrUnit.fullLength = static_cast<int>(hdrVps.size() + hdrSps.size()
            + hdrPps.size() + hdrSei.size() + hdrIdr.size());
        hdrUnit.bufferList = &hdrVpsEntry;

        require(hdrCallbacks->submitDecodeUnit(&hdrUnit) == DR_OK,
                "Valid HEVC Main10 HDR access unit was rejected");
        std::vector<std::uint8_t> expectedHdr;
        for (const auto* part : {&hdrVps, &hdrSps, &hdrPps, &hdrSei, &hdrIdr}) {
            expectedHdr.insert(expectedHdr.end(), part->begin(), part->end());
        }
        require(hdrSender.video.size() == 1
                    && hdrSender.video[0].data == expectedHdr
                    && hdrSender.video[0].rtpTimestamp == 180000,
                "HDR VPS/SPS/PPS/SEI/IDR or Moonlight timestamp was modified");
        const auto hdrDiagnostics = hdrBridge.lastVideoFrame();
        require(hdrDiagnostics && hdrDiagnostics->hdrActive
                    && hdrDiagnostics->colorSpace == COLORSPACE_REC_2020
                    && hdrDiagnostics->main10Verified
                    && hdrDiagnostics->bitDepthLuma == 10
                    && hdrDiagnostics->bitDepthChroma == 10
                    && hdrDiagnostics->chromaFormatIdc == 1
                    && !hdrValidationFailed,
                "HDR DECODE_UNIT diagnostics were not propagated");
        require(std::ranges::find(logs, "HEVC bit depth: 10") != logs.end(),
                "Verified HEVC bit depth diagnostic was not logged");
        hdrCallbacks->stop();
        hdrCallbacks->cleanup();

        std::cout << "Moonlight media bridge tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Moonlight media bridge test failed: " << error.what() << '\n';
        return 1;
    }
}

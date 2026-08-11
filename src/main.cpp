#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

#include <rtc/rtc.hpp>

int main()
{
    std::mutex mutex;
    std::condition_variable completion;
    bool offerGenerated = false;
    bool gatheringComplete = false;

    rtc::Configuration configuration;
    rtc::PeerConnection peerConnection(configuration);

    peerConnection.onStateChange([&mutex](rtc::PeerConnection::State state) {
        const std::lock_guard lock(mutex);
        std::cout << "PeerConnection state: " << state << '\n';
    });

    peerConnection.onLocalDescription(
        [&mutex, &completion, &offerGenerated](rtc::Description description) {
            const std::string sdp(description);

            {
                const std::lock_guard lock(mutex);
                std::cout << "===== WEBRTC SDP OFFER =====\n" << sdp;
                if (sdp.empty() || sdp.back() != '\n') {
                    std::cout << '\n';
                }
                std::cout << "===== END SDP OFFER =====\n";
                offerGenerated = true;
            }

            completion.notify_all();
        });

    peerConnection.onLocalCandidate([&mutex](rtc::Candidate candidate) {
        const std::lock_guard lock(mutex);
        std::cout << "ICE candidate: " << candidate.candidate() << '\n';
    });

    peerConnection.onGatheringStateChange(
        [&mutex, &completion, &gatheringComplete](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete) {
                return;
            }

            {
                const std::lock_guard lock(mutex);
                gatheringComplete = true;
            }

            completion.notify_all();
        });

    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(96);
    const auto videoTrack = peerConnection.addTrack(video);

    peerConnection.setLocalDescription();

    using namespace std::chrono_literals;
    bool completed;
    {
        std::unique_lock lock(mutex);
        completed = completion.wait_for(lock, 15s, [&offerGenerated, &gatheringComplete] {
            return offerGenerated && gatheringComplete;
        });
    }

    if (!completed) {
        std::cerr << "Timed out waiting for the SDP offer and ICE gathering\n";
        return 1;
    }

    return 0;
}

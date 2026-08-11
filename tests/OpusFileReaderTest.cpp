#include "OpusFileReader.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: opus_file_inspect <sample.opus>\n";
        return 2;
    }

    try {
        const moonlight::OpusFileReader reader(argv[1]);
        if (reader.sampleRate() != 48000 || reader.channels() != 2) {
            throw std::runtime_error("Expected stereo Opus at 48000 Hz");
        }
        if (reader.packets().empty()) {
            throw std::runtime_error("Expected at least one encoded Opus packet");
        }

        const auto expectedDuration = std::chrono::milliseconds(20);
        for (std::size_t index = 0; index < reader.packets().size(); ++index) {
            if (reader.packetDuration(index) != expectedDuration) {
                throw std::runtime_error("Expected every Opus packet to be 20 ms");
            }
        }

        std::cout << "Encoded packets: " << reader.packets().size() << '\n';
        std::cout << "Sample rate: " << reader.sampleRate() << " Hz\n";
        std::cout << "Channels: " << static_cast<unsigned int>(reader.channels()) << '\n';
        std::cout << "Packet duration: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(expectedDuration).count()
                  << " ms\n";
        std::cout << "Total packet duration: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         reader.totalPacketDuration())
                         .count()
                  << " ms\n";
        std::cout << "Pre-skip: " << reader.preSkip() << " samples\n";
    } catch (const std::exception& error) {
        std::cerr << "Opus validation failed: " << error.what() << '\n';
        return 1;
    }
}

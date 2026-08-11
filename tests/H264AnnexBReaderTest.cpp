#include "H264AnnexBReader.h"

#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: h264_annex_b_inspect <sample.h264>\n";
        return 2;
    }

    try {
        const moonlight::H264AnnexBReader reader(argv[1]);

        std::cout << "Access units: " << reader.accessUnits().size() << '\n'
                  << "SPS detected: " << (reader.hasSps() ? "yes" : "no") << '\n'
                  << "PPS detected: " << (reader.hasPps() ? "yes" : "no") << '\n'
                  << "IDR detected: " << (reader.hasIdr() ? "yes" : "no") << '\n'
                  << "Long start codes: " << (reader.hasLongStartCodes() ? "yes" : "no")
                  << '\n'
                  << "Short start codes: " << (reader.hasShortStartCodes() ? "yes" : "no")
                  << '\n';

        const bool valid = !reader.accessUnits().empty() && reader.hasSps() && reader.hasPps()
            && reader.hasIdr() && reader.hasLongStartCodes() && reader.hasShortStartCodes();
        return valid ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "H.264 inspection failed: " << error.what() << '\n';
        return 1;
    }
}

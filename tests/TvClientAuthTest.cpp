#include "gateway/TvClientAuth.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

std::filesystem::path temporaryStorePath()
{
    const auto directory = std::filesystem::temp_directory_path()
        / ("tv-client-auth-test-" + gateway::tvauth::randomHex(8));
    std::filesystem::create_directories(directory);
    return directory / "tv-clients.json";
}

} // namespace

int main()
{
    using namespace gateway::tvauth;
    try {
        // RFC 4231 test case 2.
        require(hmacSha256Hex("4a656665", "what do ya want for nothing?")
                    == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
                "HMAC-SHA256 does not match RFC 4231");
        // The same vector is asserted by tests/TizenGatewayAuthTest.js, so both ends agree.
        require(authenticationProof("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                                    "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100")
                    == "a8daa311fff071e004bcdf679a27d61367025038036ed95f4ff96691000053d5",
                "authentication proof does not match the TV implementation");

        require(isHex(randomHex(NonceBytes), NonceBytes), "random nonce is not lower-case hex");
        require(randomHex(NonceBytes) != randomHex(NonceBytes), "random nonces repeat");
        require(!isHex("ABCD", 2), "upper-case hex must be rejected so IDs compare exactly");
        require(!isHex("abc", 2), "short hex must be rejected");
        require(constantTimeEquals("abcd", "abcd") && !constantTimeEquals("abcd", "abce")
                    && !constantTimeEquals("abcd", "abc"),
                "constant-time comparison is wrong");
        require(sanitizeClientName("  Living room\n TV  ") == "Living room TV", "client name was not sanitized");
        require(sanitizeClientName("\x01\x02") == "Samsung TV", "empty client name needs a fallback");
        require(sanitizeClientName(std::string(200, 'x')).size() == MaximumClientNameLength,
                "client name was not bounded");

        const auto path = temporaryStorePath();
        {
            TvClientStore store(path);
            require(store.count() == 0, "a new store must be empty");
            const auto client = store.add("Living room");
            require(isHex(client.id, ClientIdBytes) && isHex(client.secret, ClientSecretBytes),
                    "issued credentials have the wrong shape");
            require(store.find(client.id).has_value(), "a paired TV was not found");
            require(!store.find("0123").has_value(), "an unknown TV was found");
        }
        {
            TvClientStore reloaded(path);
            require(reloaded.count() == 1, "paired TVs did not survive a restart");
            for (std::size_t index = 0; index < MaximumClients + 3; ++index) reloaded.add("TV");
            require(reloaded.count() == MaximumClients, "the paired TV list is not bounded");
            require(reloaded.removeAll() == MaximumClients && reloaded.count() == 0, "forgetting TVs failed");
        }
        require(TvClientStore(path).count() == 0, "forgotten TVs came back after a restart");
        {
            std::ofstream(path, std::ios::trunc) << "{not json";
            require(TvClientStore(path).count() == 0, "a corrupt store must trust nobody");
        }
        std::filesystem::remove_all(path.parent_path());

        using Clock = TvPairingWindow::Clock;
        const auto start = Clock::time_point{} + std::chrono::hours(1);
        TvPairingWindow window;
        require(window.state(start) == PairingState::Idle, "window starts open");
        require(window.attempt("1234", start) == PairingAttempt::NotOpen, "a closed window accepted a PIN");

        std::string pin = window.open(start);
        require(pin.size() == 4, "PIN must have four digits");
        require(window.attempt(pin, start + std::chrono::seconds(10)) == PairingAttempt::Accepted,
                "the correct PIN was rejected");
        require(window.state(start) == PairingState::Paired, "a successful pairing was not reported");
        require(window.attempt(pin, start) == PairingAttempt::NotOpen, "a used PIN was accepted twice");

        pin = window.open(start);
        const std::string wrong = pin == "0000" ? "0001" : "0000";
        require(window.attempt(wrong, start) == PairingAttempt::IncorrectPin, "wrong PIN #1");
        require(window.attempt("12a4", start) == PairingAttempt::IncorrectPin, "malformed PIN");
        require(window.attempt(wrong, start) == PairingAttempt::TooManyAttempts, "attempts are not limited");
        require(window.attempt(pin, start) == PairingAttempt::NotOpen, "the right PIN worked after lockout");
        require(window.state(start) == PairingState::Failed, "a lockout was not reported");

        pin = window.open(start);
        require(window.state(start + PairingWindowLifetime) == PairingState::Expired, "window did not expire");
        require(window.attempt(pin, start + PairingWindowLifetime) == PairingAttempt::NotOpen,
                "an expired PIN was accepted");

        pin = window.open(start);
        window.close();
        require(window.attempt(pin, start) == PairingAttempt::NotOpen, "a cancelled PIN was accepted");

        std::set<std::string> pins;
        for (int index = 0; index < 50; ++index) pins.insert(window.open(start));
        require(pins.size() > 1, "PINs are not random");

        std::cout << "TV client authentication tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TV client authentication test failed: " << error.what() << '\n';
        return 1;
    }
}

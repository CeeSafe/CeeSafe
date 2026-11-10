// ADR 0088 §2.2: ceesafe/embed.h included by a C++ host, which calls ceesafe_init and an export.
#include "ceesafe/embed.h"

#include <cstdio>

// ceesafe_exports.h gains its extern "C" guards separately (ADR 0088 §2.6); the export is declared here.
extern "C" uint64_t embed_sum(uint8_t const *bytes, size_t length);

int main() {
    ceesafe_config config{};
    config.size = static_cast<uint32_t>(sizeof config);
    if (ceesafe_init(&config) != 0) {
        return 1;
    }
    const uint8_t bytes[3] = {1, 2, 3};
    std::printf("c++ sum=%llu live=%llu\n", static_cast<unsigned long long>(embed_sum(bytes, 3)),
                static_cast<unsigned long long>(ceesafe_shutdown()));
    return 0;
}

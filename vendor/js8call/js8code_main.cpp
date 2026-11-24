// skimmer-js8code — test-only (ours, GPL-3): the 79 channel tones of one JS8
// frame from JS8Call-improved's own encoder (JS8::encode), so the gates
// synthesize JS8 without an encoder of our own.
//   skimmer-js8code SUBMODE TYPE RAW12   →   79 digits 0–7 on one line
#include "JS8.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

int main(int argc, char **argv) {
    if (argc != 4 || std::strlen(argv[3]) != 12) {
        std::fprintf(stderr, "Usage: skimmer-js8code SUBMODE TYPE RAW12\n");
        return 2;
    }
    int const submode = std::atoi(argv[1]);
    int const type = std::atoi(argv[2]);
    auto const &costas = JS8::Costas::array(submode == 0 ? JS8::Costas::Type::ORIGINAL
                                                         : JS8::Costas::Type::MODIFIED);
    int tones[79];
    try {
        JS8::encode(type, costas, argv[3], tones);
    } catch (std::exception const &e) {
        std::fprintf(stderr, "skimmer-js8code: %s\n", e.what());
        return 2;
    }
    for (int t : tones)
        std::putchar('0' + t);
    std::putchar('\n');
    return 0;
}

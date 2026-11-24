// skimmer-js8d — the JS8 decoder helper of skimmer-headless (ours, GPL-3).
//
// JS8Call-improved's decoder core (JS8.cpp, pinned, through WIFILT's
// decoder_shim C API) over 12 kHz mono PCM16 WAV files, one TX period each:
//
//   skimmer-js8d -m SUBMODE [-L HZ] [-H HZ] [-f HZ] FILE.wav ...
//   skimmer-js8d --serve
//
// SUBMODE is JS8's own number: 0 normal (15 s), 1 fast (10 s), 2 turbo
// (6 s), 4 slow (30 s), 8 ultra (4 s). A file's UTC comes from its name
// (…YYMMDD_HHMMSS.wav), as skimmer-ft8d takes it.
//
// --serve keeps one process (and with it each submode's decoder state, so
// upstream's soft combining sees the earlier periods of the same dial) and
// reads jobs from stdin, one a line: "SUBMODE LO HI FILE.wav"; each job's
// decodes end with "<DecodeFinished>" as in the one-shot form — a job it
// cannot do too, after an "<Error> …" line, so the caller never waits for
// an answer that will not come. EOF ends it.
//
// One line a decode:
//   HHMMSS SUBMODE SNR DT FREQ TYPE QUALITY RAW
// e.g. "143015 0 -18 0.32 1234.5 3 0.917 0123456789Ab": RAW is the 72-bit
// frame as 12 characters of JS8's alphabet72, TYPE its 3 transmission bits
// (1 first, 2 last, 4 fast data), QUALITY 1 − hard errors/60. Bad input:
// a message on stderr, exit 2 (one-shot) or "<Error> …" (serve).

#include "decoder_c_api.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::uint16_t read_u16(std::istream &in) {
    unsigned char b[2]{};
    in.read(reinterpret_cast<char *>(b), 2);
    return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}

std::uint32_t read_u32(std::istream &in) {
    unsigned char b[4]{};
    in.read(reinterpret_cast<char *>(b), 4);
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
}

std::vector<std::int16_t> read_wave(std::string const &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    char riff[4]{}, wave[4]{};
    in.read(riff, 4);
    (void)read_u32(in);
    in.read(wave, 4);
    if (std::memcmp(riff, "RIFF", 4) || std::memcmp(wave, "WAVE", 4))
        throw std::runtime_error(path + ": not a RIFF/WAVE file");
    std::uint16_t format = 0, channels = 0, bits = 0;
    std::uint32_t rate = 0;
    std::vector<std::int16_t> samples;
    while (in) {
        char id[4]{};
        in.read(id, 4);
        if (!in)
            break;
        auto const size = read_u32(in);
        auto const payload = in.tellg();
        if (!std::memcmp(id, "fmt ", 4)) {
            format = read_u16(in);
            channels = read_u16(in);
            rate = read_u32(in);
            (void)read_u32(in);
            (void)read_u16(in);
            bits = read_u16(in);
        } else if (!std::memcmp(id, "data", 4)) {
            samples.resize(size / 2);
            in.read(reinterpret_cast<char *>(samples.data()), size & ~1u);
        }
        in.seekg(payload + static_cast<std::streamoff>(size + (size & 1)));
    }
    if (format != 1 || channels != 1 || bits != 16 || rate != 12000 || samples.empty())
        throw std::runtime_error(path + ": expected 12000 Hz mono PCM16");
    return samples;
}

// HHMMSS from the "YYMMDD_HHMMSS.wav" at the end of a name; 0 without one.
int utc_of(std::string const &path) {
    auto const slash = path.find_last_of('/');
    std::string const base = slash == std::string::npos ? path : path.substr(slash + 1);
    auto const us = base.rfind('_');
    if (us == std::string::npos || base.size() < us + 7)
        return 0;
    int v = 0;
    for (std::size_t i = us + 1; i < us + 7; ++i) {
        if (base[i] < '0' || base[i] > '9')
            return 0;
        v = v * 10 + (base[i] - '0');
    }
    return v;
}

bool submode_ok(int m) { return m == 0 || m == 1 || m == 2 || m == 4 || m == 8; }

int period_s(int m) {
    switch (m) {
    case 0: return 15;
    case 1: return 10;
    case 2: return 6;
    case 4: return 30;
    default: return 4;
    }
}

struct Ctx {
    int utc;
};

void print_decode(Js8ReferenceDecode const *d, void *user) {
    auto const *ctx = static_cast<Ctx const *>(user);
    std::printf("%06d %d %d %.2f %.1f %d %.3f %s\n", ctx->utc, d->submode, d->snr,
                static_cast<double>(d->dt), static_cast<double>(d->frequency_hz),
                d->type & 7, static_cast<double>(d->quality), d->data);
}

// One file through one submode's decoder; FALSE (and a message) on bad input.
bool decode_file(int submode, int lo, int hi, int fq, std::string const &path,
                 std::string &error) {
    try {
        auto samples = read_wave(path);
        std::size_t const max = static_cast<std::size_t>(period_s(submode)) * 12000;
        if (samples.size() > max)
            samples.resize(max);
        Ctx ctx{utc_of(path)};
        int const n = js8_reference_decode(samples.data(), samples.size(), submode, lo, hi,
                                           fq, print_decode, &ctx);
        if (n < 0)
            throw std::runtime_error(path + ": decoder refused the input");
    } catch (std::exception const &e) {
        error = e.what();
        return false;
    }
    std::printf("<DecodeFinished>\n");
    std::fflush(stdout);
    return true;
}

int usage() {
    std::fprintf(stderr,
                 "Usage: skimmer-js8d -m SUBMODE [-L HZ] [-H HZ] [-f HZ] FILE.wav ...\n"
                 "       skimmer-js8d --serve   (stdin: \"SUBMODE LO HI FILE.wav\" a line)\n"
                 "SUBMODE: 0 normal, 1 fast, 2 turbo, 4 slow, 8 ultra\n");
    return 2;
}

int serve() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        int submode = -1, lo = 0, hi = 0, used = 0;
        if (std::sscanf(line.c_str(), "%d %d %d %n", &submode, &lo, &hi, &used) < 3 ||
            !submode_ok(submode) || used <= 0 || static_cast<std::size_t>(used) >= line.size()) {
            std::printf("<Error> bad job: %s\n<DecodeFinished>\n", line.c_str());
            std::fflush(stdout);
            continue;
        }
        std::string error;
        if (!decode_file(submode, lo, hi, 1500, line.substr(used), error)) {
            std::printf("<Error> %s\n<DecodeFinished>\n", error.c_str());
            std::fflush(stdout);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--serve") == 0)
        return serve();
    int submode = 0, lo = 200, hi = 3500, fq = 1500, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; ++i) {
        if (i + 1 >= argc)
            return usage();
        int const v = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "-m"))
            submode = v;
        else if (!std::strcmp(argv[i], "-L"))
            lo = v;
        else if (!std::strcmp(argv[i], "-H"))
            hi = v;
        else if (!std::strcmp(argv[i], "-f"))
            fq = v;
        else
            return usage();
        ++i;
    }
    if (i >= argc || !submode_ok(submode) || lo < 0 || hi <= lo || hi > 6000)
        return usage();
    for (; i < argc; ++i) {
        std::string error;
        if (!decode_file(submode, lo, hi, fq, argv[i], error)) {
            std::fprintf(stderr, "skimmer-js8d: %s\n", error.c_str());
            return 2;
        }
    }
    return 0;
}

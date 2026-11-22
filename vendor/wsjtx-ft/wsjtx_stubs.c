/* OURS, not WSJT-X's (see VENDOR.md). Stands in for the two C++ pieces the
 * FT8/FT4 decoders reach, so skimmer-ft8d needs neither a C++ compiler,
 * boost nor Qt:
 *
 *  - lib/crc14.cpp, crc13.cpp, crc10.cpp: boost::augmented_crc<W, POLY>, an
 *    MSB-first polynomial division with zero init, no reflection and no
 *    final XOR, over a message whose last W bits are the (zeroed) CRC. The
 *    gate checks crc14 against WSJT-X's own Fortran get_crc14 and against a
 *    message's published tones.
 *  - lib/shmem.cpp: Qt's QSharedMemory. ft8_decode locks it only around its
 *    "early" decodes when it runs inside WSJT-X; from disk there is nothing
 *    to share, so the lock always succeeds.
 *  - genq65 (lib/qra/q65/genq65.f90 + the Q65 C coder): ft8_a7 and ft8apset
 *    call stdcall(), which lives in qra/q65/q65_set_list.f90 beside
 *    q65_set_list() — and that one calls genq65. Nothing in FT8/FT4 calls
 *    q65_set_list, so genq65 is never reached; this aborts if it ever is.
 *
 * Copyright (C) 2026 skimmer-for-linux authors. GPL-3.0-or-later. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned augmented_crc(const unsigned char *data, int length,
                              int width, unsigned poly) {
  const unsigned top = 1u << (width - 1), mask = (1u << width) - 1u;
  unsigned rem = 0;
  for (int i = 0; i < length; i++) {
    for (int b = 7; b >= 0; b--) {
      const unsigned bit = (data[i] >> b) & 1u;
      const unsigned out = rem & top;
      rem = ((rem << 1) | bit) & mask;
      if (out) { rem ^= poly; }
    }
  }
  return rem & mask;
}

short crc14(const unsigned char *data, int length) {
  return (short)augmented_crc(data, length, 14, 0x2757);
}
bool crc14_check(const unsigned char *data, int length) {
  return augmented_crc(data, length, 14, 0x2757) == 0;
}
short crc13(const unsigned char *data, int length) {
  return (short)augmented_crc(data, length, 13, 0x15D7);
}
bool crc13_check(const unsigned char *data, int length) {
  return augmented_crc(data, length, 13, 0x15D7) == 0;
}
short crc10(const unsigned char *data, int length) {
  return (short)augmented_crc(data, length, 10, 0x08F);
}
bool crc10_check(const unsigned char *data, int length) {
  return augmented_crc(data, length, 10, 0x08F) == 0;
}

bool shmem_lock(void) { return true; }
bool shmem_unlock(void) { return true; }

void genq65_(void) {
  fputs("skimmer-ft8d: genq65 is not built (Q65 is not FT8/FT4)\n", stderr);
  abort();
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace hisense_w4a1 {

/// Turns `body`, an update frame from F4 F5 up to the checksum, into the bytes
/// to send: adds the checksum and the F4 FB end marker, and doubles every F4
/// after the start marker except the end marker's. Returns the checksum.
/// Kept free of ESPHome headers so it can be tested on a computer.
inline uint16_t encode_frame(const uint8_t *body, size_t len, std::vector<uint8_t> &wire) {
  uint32_t sum = 0;
  for (size_t i = 2; i < len; i++)
    sum += body[i];
  const uint16_t checksum = sum & 0xFFFF;

  wire.clear();
  wire.reserve(len + 8);
  wire.push_back(body[0]);
  wire.push_back(body[1]);
  auto put = [&wire](uint8_t b) {
    wire.push_back(b);
    if (b == 0xF4)
      wire.push_back(0xF4);
  };
  for (size_t i = 2; i < len; i++)
    put(body[i]);
  put(checksum >> 8);
  put(checksum & 0xFF);
  wire.push_back(0xF4);
  wire.push_back(0xFB);
  return checksum;
}

}  // namespace hisense_w4a1
}  // namespace esphome

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace hisense_w4a1 {

// Kept free of ESPHome headers so it can be tested on a computer.

/// Turns `body`, an update frame from F4 F5 up to the checksum, into the bytes
/// to send: adds the checksum and the F4 FB end marker, and doubles every F4
/// after the start marker except the end marker's. Returns the checksum.
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

/// Reads a frame typed for the send_test_frame action: the bytes of an update
/// frame from F4 F5 up to the checksum, in hex. Spaces, colons, commas and
/// dashes are ignored. Returns nullptr and fills `body` on success, or says
/// what's wrong with the input.
inline const char *parse_test_frame(const char *hex, size_t len, std::vector<uint8_t> &body) {
  body.clear();
  int high = -1;
  for (size_t i = 0; i < len; i++) {
    const char c = hex[i];
    int nibble;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      nibble = c - 'A' + 10;
    } else if (c == ' ' || c == ':' || c == ',' || c == '-') {
      continue;
    } else {
      return "it contains a character that isn't a hex digit";
    }
    if (high < 0) {
      high = nibble;
    } else {
      body.push_back(static_cast<uint8_t>(high << 4 | nibble));
      high = -1;
    }
  }
  if (high >= 0)
    return "it has an odd number of hex digits";

  const size_t n = body.size();
  if (n < 16)
    return "it's shorter than the 16-byte frame header";
  if (body[0] != 0xF4 || body[1] != 0xF5)
    return "it doesn't start with F4 F5";
  if (body[2] != 0x00)
    return "byte 2 isn't 00, which marks a frame sent to the AC";
  if (body[13] != 0x65 || body[14] != 0x00)
    return "bytes 13 and 14 aren't 65 00, which marks an update frame";
  // Byte 4 is the whole frame's length minus 9, and the checksum and end
  // marker add 4 bytes to what was typed.
  if (body[4] + 5 != n)
    return "byte 4 doesn't match the length: it must be the number of bytes typed minus 5";
  return nullptr;
}

}  // namespace hisense_w4a1
}  // namespace esphome

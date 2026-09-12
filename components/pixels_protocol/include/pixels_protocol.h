#pragma once

#include <cstddef>
#include <cstdint>

namespace pixels {

constexpr std::size_t kMaxNameLength = 13;

enum class DieType : uint8_t {
  kUnknown = 0,
  kD4 = 1,
  kD6 = 2,
  kD8 = 3,
  kD10 = 4,
  kD00 = 5,
  kD12 = 6,
  kD20 = 7,
  kD6Pipped = 8,
  kD6Fudge = 9,
};

enum class RollState : uint8_t {
  kUnknown = 0,
  kRolled = 1,
  kHandling = 2,
  kRolling = 3,
  kCrooked = 4,
  kOnFace = 5,
};

struct Advertisement {
  uint32_t pixel_id = 0;
  uint32_t firmware_timestamp = 0;
  int rssi = 0;
  uint8_t led_count = 0;
  uint8_t colorway = 0;
  uint8_t face_index = 0;
  uint8_t battery_percent = 0;
  DieType die_type = DieType::kUnknown;
  RollState roll_state = RollState::kUnknown;
  bool charging = false;
  bool has_identity = false;
  bool has_status = false;
  char name[kMaxNameLength + 1] = {};
};

bool ParseAdvertisement(const uint8_t *payload, std::size_t length, int rssi,
                        Advertisement *result);
bool ParseAdvertisementFragment(const uint8_t *payload, std::size_t length,
                                int rssi, Advertisement *result);
void MergeAdvertisement(const Advertisement &fragment,
                        Advertisement *combined);
int FaceValue(DieType type, uint8_t face_index, uint32_t firmware_timestamp);
const char *DieTypeName(DieType type);
const char *RollStateName(RollState state);

} // namespace pixels

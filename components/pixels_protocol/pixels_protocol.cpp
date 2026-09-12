#include "pixels_protocol.h"

#include <algorithm>
#include <cstring>

namespace pixels {
namespace {

constexpr uint8_t kAdTypeCompleteName = 0x09;
constexpr uint8_t kAdTypeShortName = 0x08;
constexpr uint8_t kAdTypeManufacturer = 0xff;
constexpr uint8_t kAdTypeServiceData16 = 0x16;
constexpr uint32_t kBadNormalsFirmwareCutoff = 1704150000U;

constexpr uint8_t kInformationServiceUuidLittleEndian[] = {0x0a, 0x18};

uint32_t ReadLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) |
         (static_cast<uint32_t>(data[3]) << 24U);
}

bool IsPixelsServiceData(const uint8_t *data, std::size_t length) {
  return length >= sizeof(kInformationServiceUuidLittleEndian) + 8 &&
         std::memcmp(data, kInformationServiceUuidLittleEndian,
                     sizeof(kInformationServiceUuidLittleEndian)) == 0;
}

} // namespace

bool ParseAdvertisementFragment(const uint8_t *payload, std::size_t length,
                                int rssi, Advertisement *result) {
  if (payload == nullptr || result == nullptr) {
    return false;
  }

  Advertisement parsed;
  parsed.rssi = rssi;

  const uint8_t *manufacturer = nullptr;
  std::size_t manufacturer_length = 0;
  bool found_relevant_field = false;
  std::size_t offset = 0;

  while (offset < length) {
    const uint8_t field_length = payload[offset];
    if (field_length == 0) {
      break;
    }
    if (offset + 1U + field_length > length) {
      return false;
    }

    const uint8_t type = payload[offset + 1U];
    const uint8_t *data = payload + offset + 2U;
    const std::size_t data_length = field_length - 1U;

    if ((type == kAdTypeCompleteName || type == kAdTypeShortName) &&
        parsed.name[0] == '\0') {
      const std::size_t copy_length =
          std::min(data_length, static_cast<std::size_t>(kMaxNameLength));
      std::memcpy(parsed.name, data, copy_length);
      parsed.name[copy_length] = '\0';
      found_relevant_field = true;
    } else if (type == kAdTypeManufacturer) {
      manufacturer = data;
      manufacturer_length = data_length;
      found_relevant_field = true;
    } else if (type == kAdTypeServiceData16 &&
               IsPixelsServiceData(data, data_length)) {
      parsed.pixel_id = ReadLe32(data + 2);
      parsed.firmware_timestamp = ReadLe32(data + 6);
      parsed.has_identity = parsed.pixel_id != 0;
      found_relevant_field = true;
    }

    offset += static_cast<std::size_t>(field_length) + 1U;
  }

  if (manufacturer != nullptr && manufacturer_length >= 7) {
    parsed.led_count = manufacturer[2];
    parsed.die_type = static_cast<DieType>(manufacturer[3] >> 4U);
    parsed.colorway = manufacturer[3] & 0x0fU;
    parsed.roll_state = static_cast<RollState>(manufacturer[4]);
    parsed.face_index = manufacturer[5];
    parsed.battery_percent = manufacturer[6] & 0x7fU;
    parsed.charging = (manufacturer[6] & 0x80U) != 0;
    parsed.has_status = true;
  }

  *result = parsed;
  return found_relevant_field;
}

void MergeAdvertisement(const Advertisement &fragment,
                        Advertisement *combined) {
  if (combined == nullptr) {
    return;
  }
  combined->rssi = fragment.rssi;
  if (fragment.name[0] != '\0') {
    std::memcpy(combined->name, fragment.name, sizeof(combined->name));
  }
  if (fragment.has_identity) {
    combined->pixel_id = fragment.pixel_id;
    combined->firmware_timestamp = fragment.firmware_timestamp;
    combined->has_identity = true;
  }
  if (fragment.has_status) {
    combined->led_count = fragment.led_count;
    combined->colorway = fragment.colorway;
    combined->face_index = fragment.face_index;
    combined->battery_percent = fragment.battery_percent;
    combined->die_type = fragment.die_type;
    combined->roll_state = fragment.roll_state;
    combined->charging = fragment.charging;
    combined->has_status = true;
  }
}

bool ParseAdvertisement(const uint8_t *payload, std::size_t length, int rssi,
                        Advertisement *result) {
  Advertisement parsed;
  if (!ParseAdvertisementFragment(payload, length, rssi, &parsed) ||
      !parsed.has_identity || !parsed.has_status) {
    return false;
  }
  *result = parsed;
  return true;
}

int FaceValue(DieType type, uint8_t face_index, uint32_t firmware_timestamp) {
  if (firmware_timestamp != 0 &&
      firmware_timestamp <= kBadNormalsFirmwareCutoff) {
    if (type == DieType::kD4) {
      if (face_index == 3)
        return 2;
      if (face_index == 2)
        return 3;
      if (face_index == 5)
        return 4;
      return 1;
    }
    if (type == DieType::kD6) {
      if (face_index == 4)
        return 2;
      if (face_index == 3)
        return 3;
      if (face_index == 2)
        return 4;
      if (face_index == 1)
        return 5;
      return static_cast<int>(face_index) + 1;
    }
  }

  if (type == DieType::kD10) {
    return face_index;
  }
  if (type == DieType::kD00) {
    return static_cast<int>(face_index) * 10;
  }
  return static_cast<int>(face_index) + 1;
}

const char *DieTypeName(DieType type) {
  switch (type) {
  case DieType::kD4:
    return "D4";
  case DieType::kD6:
    return "D6";
  case DieType::kD8:
    return "D8";
  case DieType::kD10:
    return "D10";
  case DieType::kD00:
    return "D00";
  case DieType::kD12:
    return "D12";
  case DieType::kD20:
    return "D20";
  case DieType::kD6Pipped:
    return "D6P";
  case DieType::kD6Fudge:
    return "D6F";
  case DieType::kUnknown:
  default:
    return "D?";
  }
}

const char *RollStateName(RollState state) {
  switch (state) {
  case RollState::kRolled:
    return "ROLLED";
  case RollState::kHandling:
    return "HANDLING";
  case RollState::kRolling:
    return "ROLLING";
  case RollState::kCrooked:
    return "CROOKED";
  case RollState::kOnFace:
    return "ON FACE";
  case RollState::kUnknown:
  default:
    return "UNKNOWN";
  }
}

} // namespace pixels

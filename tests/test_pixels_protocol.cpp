#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "pixels_protocol.h"

namespace {

void AddField(std::vector<uint8_t> *payload, uint8_t type,
              const std::vector<uint8_t> &data) {
  payload->push_back(static_cast<uint8_t>(data.size() + 1));
  payload->push_back(type);
  payload->insert(payload->end(), data.begin(), data.end());
}

std::vector<uint8_t> InformationServiceUuid() {
  return {0x0a, 0x18};
}

void TestValidAdvertisement() {
  std::vector<uint8_t> payload;
  AddField(&payload, 0x09, {'E', 'm', 'b', 'e', 'r'});
  AddField(&payload, 0xff, {0x00, 0x00, 20, 0x73, 1, 16, 0xd2});
  auto service = InformationServiceUuid();
  service.insert(service.end(),
                 {0x78, 0x56, 0x34, 0x12, 0x40, 0x30, 0x20, 0x10});
  AddField(&payload, 0x16, service);

  pixels::Advertisement result;
  assert(
      pixels::ParseAdvertisement(payload.data(), payload.size(), -48, &result));
  assert(result.pixel_id == 0x12345678);
  assert(result.firmware_timestamp == 0x10203040);
  assert(result.die_type == pixels::DieType::kD20);
  assert(result.colorway == 3);
  assert(result.face_index == 16);
  assert(result.battery_percent == 82);
  assert(result.charging);
  assert(result.rssi == -48);
  assert(std::strcmp(result.name, "Ember") == 0);
  assert(pixels::FaceValue(result.die_type, result.face_index,
                           result.firmware_timestamp) == 17);
}

void TestMalformedAdvertisement() {
  const uint8_t payload[] = {10, 0xff, 1, 2};
  pixels::Advertisement result;
  assert(!pixels::ParseAdvertisement(payload, sizeof(payload), -70, &result));
}

void TestSplitAdvertisement() {
  std::vector<uint8_t> primary;
  AddField(&primary, 0xff, {0x00, 0x00, 8, 0x31, 1, 5, 75});
  std::vector<uint8_t> response;
  AddField(&response, 0x09, {'A', 'z', 'u', 'r', 'e'});
  auto service = InformationServiceUuid();
  service.insert(service.end(),
                 {0x04, 0x03, 0x02, 0x01, 0x00, 0x10, 0x20, 0x30});
  AddField(&response, 0x16, service);

  pixels::Advertisement first;
  pixels::Advertisement second;
  pixels::Advertisement combined;
  assert(pixels::ParseAdvertisementFragment(primary.data(), primary.size(),
                                            -55, &first));
  assert(first.has_status);
  assert(!first.has_identity);
  assert(pixels::ParseAdvertisementFragment(response.data(), response.size(),
                                            -53, &second));
  assert(second.has_identity);
  pixels::MergeAdvertisement(first, &combined);
  pixels::MergeAdvertisement(second, &combined);
  assert(combined.has_identity && combined.has_status);
  assert(combined.pixel_id == 0x01020304);
  assert(combined.die_type == pixels::DieType::kD8);
  assert(pixels::FaceValue(combined.die_type, combined.face_index,
                           combined.firmware_timestamp) == 6);
  assert(std::strcmp(combined.name, "Azure") == 0);
}

void TestFaceMappings() {
  assert(pixels::FaceValue(pixels::DieType::kD10, 0, 0) == 0);
  assert(pixels::FaceValue(pixels::DieType::kD00, 9, 0) == 90);
  assert(pixels::FaceValue(pixels::DieType::kD12, 11, 0) == 12);
  assert(pixels::FaceValue(pixels::DieType::kD4, 3, 1704150000U) == 2);
  assert(pixels::FaceValue(pixels::DieType::kD6, 4, 1704150000U) == 2);
  assert(std::strcmp(pixels::RollStateName(pixels::RollState::kRolling),
                     "ROLLING") == 0);
}

} // namespace

int main() {
  TestValidAdvertisement();
  TestMalformedAdvertisement();
  TestSplitAdvertisement();
  TestFaceMappings();
  return 0;
}

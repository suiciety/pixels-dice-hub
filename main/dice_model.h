#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pixels_protocol.h"

namespace app {

constexpr std::size_t kMaxDice = 8;
constexpr std::size_t kMaxCandidates = 12;
constexpr std::size_t kRollHistorySize = 20;
constexpr uint64_t kDieOfflineMs = 120000;

enum class AggregateMode : uint8_t { kSum = 0, kHigh = 1, kLow = 2 };

struct Die {
  uint32_t pixel_id = 0;
  uint32_t firmware_timestamp = 0;
  uint32_t profile_hash = 0;
  uint32_t available_flash = 0;
  pixels::DieType type = pixels::DieType::kUnknown;
  pixels::RollState roll_state = pixels::RollState::kUnknown;
  int last_roll = 0;
  int rssi = 0;
  int16_t mcu_temperature_centi_c = 0;
  int16_t battery_temperature_centi_c = 0;
  uint64_t last_seen_ms = 0;
  uint16_t firmware_version = 0;
  uint8_t battery = 0;
  uint8_t battery_state = 0;
  uint8_t colorway = 0;
  bool charging = false;
  bool has_roll = false;
  bool has_connected_info = false;
  bool has_temperature = false;
  char name[pixels::kMaxNameLength + 1] = {};
};

struct ConnectedDieInfo {
  uint32_t firmware_timestamp = 0;
  uint32_t profile_hash = 0;
  uint32_t available_flash = 0;
  uint32_t pixel_id = 0;
  uint16_t firmware_version = 0;
  pixels::DieType type = pixels::DieType::kUnknown;
  pixels::RollState roll_state = pixels::RollState::kUnknown;
  uint8_t face_index = 0;
  uint8_t battery = 0;
  uint8_t battery_state = 0;
  uint8_t colorway = 0;
  bool has_name = false;
  char name[pixels::kMaxNameLength + 1] = {};
};

struct RollEvent {
  uint32_t pixel_id = 0;
  uint64_t timestamp_ms = 0;
  pixels::DieType type = pixels::DieType::kUnknown;
  int value = 0;
  char name[pixels::kMaxNameLength + 1] = {};
};

struct RollHistory {
  std::array<RollEvent, kRollHistorySize> events{};
  std::size_t count = 0;
};

struct Snapshot {
  std::array<Die, kMaxDice> dice{};
  std::array<Die, kMaxCandidates> candidates{};
  std::size_t dice_count = 0;
  std::size_t candidate_count = 0;
  std::array<RollEvent, kRollHistorySize> history{};
  std::size_t history_count = 0;
  AggregateMode aggregate_mode = AggregateMode::kSum;
  int64_t aggregate_value = 0;
  uint32_t aggregate_roll_count = 0;
  bool has_aggregate = false;
  uint32_t revision = 0;
};

class DiceModel {
public:
  DiceModel();
  ~DiceModel();

  void Ingest(const pixels::Advertisement &advertisement, uint64_t now_ms);
  bool Pair(uint32_t pixel_id);
  bool Unpair(uint32_t pixel_id);
  void RestorePaired(const uint32_t *pixel_ids, std::size_t count,
                     uint64_t now_ms);
  std::size_t PairedIds(uint32_t *pixel_ids, std::size_t capacity) const;
  void CycleAggregate();
  void ClearAggregate();
  void RestoreAggregate(AggregateMode mode);
  void UpdateConnectedInfo(const ConnectedDieInfo &info, uint64_t now_ms);
  void UpdateTemperature(uint32_t pixel_id, int16_t mcu_temperature_centi_c,
                         int16_t battery_temperature_centi_c);
  uint32_t Revision() const;
  Snapshot GetSnapshot(uint64_t now_ms) const;
  RollHistory GetRollHistory(uint32_t pixel_id) const;

private:
  int FindDie(uint32_t pixel_id) const;
  int FindCandidate(uint32_t pixel_id) const;
  static bool ApplyAdvertisement(Die *die,
                                 const pixels::Advertisement &advertisement,
                                 uint64_t now_ms);
  void AddRollEvent(std::size_t die_index, uint64_t now_ms);

  mutable SemaphoreHandle_t mutex_;
  std::array<Die, kMaxDice> dice_{};
  std::array<Die, kMaxCandidates> candidates_{};
  std::size_t dice_count_ = 0;
  std::size_t candidate_count_ = 0;
  std::array<RollEvent, kRollHistorySize> history_{};
  std::size_t history_count_ = 0;
  std::size_t history_next_ = 0;
  std::array<std::array<RollEvent, kRollHistorySize>, kMaxDice>
      die_history_{};
  std::array<std::size_t, kMaxDice> die_history_count_{};
  std::array<std::size_t, kMaxDice> die_history_next_{};
  std::array<pixels::RollState, kMaxDice> advertised_roll_state_{};
  std::array<bool, kMaxDice> has_advertised_roll_state_{};
  AggregateMode aggregate_mode_ = AggregateMode::kSum;
  int64_t aggregate_sum_ = 0;
  int aggregate_high_ = 0;
  int aggregate_low_ = 0;
  uint32_t aggregate_roll_count_ = 0;
  uint32_t revision_ = 1;
  uint64_t restore_grace_until_ms_ = 0;
};

} // namespace app

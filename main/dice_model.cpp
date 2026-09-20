#include "dice_model.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace app {
namespace {

constexpr uint64_t kRestoreGraceMs = 15000;
constexpr uint64_t kCandidateLifetimeMs = 15000;

class Lock {
public:
  explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(mutex_); }

private:
  SemaphoreHandle_t mutex_;
};

} // namespace

DiceModel::DiceModel() : mutex_(xSemaphoreCreateMutex()) {}

DiceModel::~DiceModel() {
  if (mutex_ != nullptr) {
    vSemaphoreDelete(mutex_);
  }
}

int DiceModel::FindDie(uint32_t pixel_id) const {
  for (std::size_t i = 0; i < dice_count_; ++i) {
    if (dice_[i].pixel_id == pixel_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int DiceModel::FindCandidate(uint32_t pixel_id) const {
  for (std::size_t i = 0; i < candidate_count_; ++i) {
    if (candidates_[i].pixel_id == pixel_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool DiceModel::ApplyAdvertisement(Die *die,
                                   const pixels::Advertisement &advertisement,
                                   uint64_t now_ms) {
  const int new_roll =
      pixels::FaceValue(advertisement.die_type, advertisement.face_index,
                        advertisement.firmware_timestamp);
  const bool completed_roll =
      advertisement.roll_state == pixels::RollState::kRolled;
  const bool changed =
      die->pixel_id != advertisement.pixel_id ||
      die->type != advertisement.die_type ||
      die->roll_state != advertisement.roll_state ||
      die->battery != advertisement.battery_percent ||
      die->colorway != advertisement.colorway ||
      die->charging != advertisement.charging ||
      std::strncmp(die->name, advertisement.name, sizeof(die->name)) != 0 ||
      (completed_roll && (!die->has_roll || die->last_roll != new_roll)) ||
      die->last_seen_ms == 0 || now_ms - die->last_seen_ms > 15000;

  die->pixel_id = advertisement.pixel_id;
  die->type = advertisement.die_type;
  die->roll_state = advertisement.roll_state;
  die->rssi = advertisement.rssi;
  die->last_seen_ms = now_ms;
  die->battery = advertisement.battery_percent;
  die->colorway = advertisement.colorway;
  die->charging = advertisement.charging;
  std::strncpy(die->name, advertisement.name, sizeof(die->name) - 1);
  die->name[sizeof(die->name) - 1] = '\0';

  if (completed_roll) {
    die->last_roll = new_roll;
    die->has_roll = true;
  }
  return changed;
}

void DiceModel::AddRollEvent(std::size_t die_index, uint64_t now_ms) {
  const Die &die = dice_[die_index];
  RollEvent &event = history_[history_next_];
  event = {};
  event.pixel_id = die.pixel_id;
  event.timestamp_ms = now_ms;
  event.type = die.type;
  event.value = die.last_roll;
  std::strncpy(event.name, die.name, sizeof(event.name) - 1);
  history_next_ = (history_next_ + 1U) % history_.size();
  history_count_ = std::min(history_count_ + 1U, history_.size());

  RollEvent &die_event = die_history_[die_index][die_history_next_[die_index]];
  die_event = event;
  die_history_next_[die_index] =
      (die_history_next_[die_index] + 1U) % kRollHistorySize;
  die_history_count_[die_index] =
      std::min(die_history_count_[die_index] + 1U, kRollHistorySize);

  aggregate_sum_ += die.last_roll;
  if (aggregate_roll_count_ == 0) {
    aggregate_high_ = die.last_roll;
    aggregate_low_ = die.last_roll;
  } else {
    aggregate_high_ = std::max(aggregate_high_, die.last_roll);
    aggregate_low_ = std::min(aggregate_low_, die.last_roll);
  }
  ++aggregate_roll_count_;

  CalculatorRoll calculator_roll;
  calculator_roll.pixel_id = die.pixel_id;
  calculator_roll.type = die.type;
  calculator_roll.value = die.last_roll;
  calculator_.Consume(calculator_roll);
}

void DiceModel::Ingest(const pixels::Advertisement &advertisement,
                       uint64_t now_ms) {
  Lock lock(mutex_);
  const int die_index = FindDie(advertisement.pixel_id);
  if (die_index >= 0) {
    Die &die = dice_[die_index];
    const std::size_t index = static_cast<std::size_t>(die_index);
    const bool first_observation_after_restore =
        !has_advertised_roll_state_[index] &&
        restore_grace_until_ms_ != 0 && now_ms >= restore_grace_until_ms_;
    const bool completed_roll =
        advertisement.roll_state == pixels::RollState::kRolled &&
        ((has_advertised_roll_state_[index] &&
          advertised_roll_state_[index] != pixels::RollState::kRolled) ||
         first_observation_after_restore);
    bool changed = ApplyAdvertisement(&die, advertisement, now_ms);
    if (completed_roll) {
      AddRollEvent(index, now_ms);
      changed = true;
    }
    if (changed) {
      ++revision_;
    }
    advertised_roll_state_[index] = advertisement.roll_state;
    has_advertised_roll_state_[index] = true;
    return;
  }

  int candidate_index = FindCandidate(advertisement.pixel_id);
  bool changed = false;
  if (candidate_index < 0) {
    if (candidate_count_ < candidates_.size()) {
      candidate_index = static_cast<int>(candidate_count_++);
      changed = true;
    } else {
      std::size_t oldest_index = 0;
      for (std::size_t i = 1; i < candidate_count_; ++i) {
        if (candidates_[i].last_seen_ms <
            candidates_[oldest_index].last_seen_ms) {
          oldest_index = i;
        }
      }
      if (now_ms - candidates_[oldest_index].last_seen_ms <=
          kCandidateLifetimeMs) {
        return;
      }
      candidate_index = static_cast<int>(oldest_index);
      changed = true;
    }
  }
  const Die previous = candidates_[candidate_index];
  changed = ApplyAdvertisement(&candidates_[candidate_index], advertisement,
                               now_ms) ||
            changed || std::abs(previous.rssi - advertisement.rssi) >= 5;
  if (changed) {
    ++revision_;
  }
}

bool DiceModel::Pair(uint32_t pixel_id) {
  Lock lock(mutex_);
  if (FindDie(pixel_id) >= 0) {
    return true;
  }
  const int candidate_index = FindCandidate(pixel_id);
  if (candidate_index < 0 || dice_count_ >= dice_.size()) {
    return false;
  }
  dice_[dice_count_] = candidates_[candidate_index];
  die_history_[dice_count_] = {};
  die_history_count_[dice_count_] = 0;
  die_history_next_[dice_count_] = 0;
  advertised_roll_state_[dice_count_] = dice_[dice_count_].roll_state;
  has_advertised_roll_state_[dice_count_] = true;
  ++dice_count_;
  for (std::size_t i = candidate_index + 1; i < candidate_count_; ++i) {
    candidates_[i - 1] = candidates_[i];
  }
  --candidate_count_;
  ++revision_;
  return true;
}

bool DiceModel::Unpair(uint32_t pixel_id) {
  Lock lock(mutex_);
  const int index = FindDie(pixel_id);
  if (index < 0) {
    return false;
  }
  calculator_.Cancel();
  for (std::size_t i = index + 1; i < dice_count_; ++i) {
    dice_[i - 1] = dice_[i];
    die_history_[i - 1] = die_history_[i];
    die_history_count_[i - 1] = die_history_count_[i];
    die_history_next_[i - 1] = die_history_next_[i];
    advertised_roll_state_[i - 1] = advertised_roll_state_[i];
    has_advertised_roll_state_[i - 1] = has_advertised_roll_state_[i];
  }
  --dice_count_;
  dice_[dice_count_] = {};
  die_history_[dice_count_] = {};
  die_history_count_[dice_count_] = 0;
  die_history_next_[dice_count_] = 0;
  advertised_roll_state_[dice_count_] = pixels::RollState::kUnknown;
  has_advertised_roll_state_[dice_count_] = false;
  ++revision_;
  return true;
}

void DiceModel::RestorePaired(const uint32_t *pixel_ids, std::size_t count,
                              uint64_t now_ms) {
  Lock lock(mutex_);
  dice_count_ = std::min(count, dice_.size());
  for (std::size_t i = 0; i < dice_count_; ++i) {
    dice_[i] = {};
    dice_[i].pixel_id = pixel_ids[i];
  }
  die_history_ = {};
  die_history_count_ = {};
  die_history_next_ = {};
  advertised_roll_state_ = {};
  has_advertised_roll_state_ = {};
  restore_grace_until_ms_ = now_ms + kRestoreGraceMs;
  ++revision_;
}

std::size_t DiceModel::PairedIds(uint32_t *pixel_ids,
                                 std::size_t capacity) const {
  Lock lock(mutex_);
  const std::size_t count = std::min(dice_count_, capacity);
  for (std::size_t i = 0; i < count; ++i) {
    pixel_ids[i] = dice_[i].pixel_id;
  }
  return count;
}

void DiceModel::CycleAggregate() {
  Lock lock(mutex_);
  aggregate_mode_ = static_cast<AggregateMode>(
      (static_cast<uint8_t>(aggregate_mode_) + 1U) % 3U);
  ++revision_;
}

void DiceModel::ClearAggregate() {
  Lock lock(mutex_);
  aggregate_sum_ = 0;
  aggregate_high_ = 0;
  aggregate_low_ = 0;
  aggregate_roll_count_ = 0;
  ++revision_;
}

void DiceModel::RestoreAggregate(AggregateMode mode) {
  Lock lock(mutex_);
  if (mode > AggregateMode::kLow) {
    return;
  }
  aggregate_mode_ = mode;
  ++revision_;
}

void DiceModel::AdjustCalculator(CalculatorSetting setting, int delta) {
  Lock lock(mutex_);
  calculator_.Adjust(setting, delta);
  ++revision_;
}

bool DiceModel::StartCalculator() {
  Lock lock(mutex_);
  if (calculator_.GetSnapshot().status == CalculatorStatus::kRolling) {
    return false;
  }
  std::array<CalculatorDie, kMaxDice> available{};
  for (std::size_t i = 0; i < dice_count_; ++i) {
    available[i].pixel_id = dice_[i].pixel_id;
    available[i].type = dice_[i].type;
    std::strncpy(available[i].name, dice_[i].name,
                 sizeof(available[i].name) - 1);
  }
  const bool started = calculator_.Start(available.data(), dice_count_);
  ++revision_;
  return started;
}

void DiceModel::CancelCalculator() {
  Lock lock(mutex_);
  calculator_.Cancel();
  ++revision_;
}

CalculatorSnapshot DiceModel::GetCalculatorSnapshot() const {
  Lock lock(mutex_);
  return calculator_.GetSnapshot();
}

void DiceModel::UpdateConnectedInfo(const ConnectedDieInfo &info,
                                    uint64_t now_ms) {
  Lock lock(mutex_);
  const int die_index = FindDie(info.pixel_id);
  if (die_index < 0) {
    return;
  }
  Die &die = dice_[die_index];
  die.firmware_timestamp = info.firmware_timestamp;
  die.firmware_version = info.firmware_version;
  die.profile_hash = info.profile_hash;
  die.available_flash = info.available_flash;
  die.type = info.type;
  die.colorway = info.colorway;
  die.roll_state = info.roll_state;
  die.battery = info.battery;
  die.battery_state = info.battery_state;
  die.charging = info.battery_state >= 2 && info.battery_state <= 4;
  die.last_seen_ms = now_ms;
  die.has_connected_info = true;
  if (info.has_name) {
    std::strncpy(die.name, info.name, sizeof(die.name) - 1);
    die.name[sizeof(die.name) - 1] = '\0';
  }
  if (info.roll_state == pixels::RollState::kRolled) {
    die.last_roll =
        pixels::FaceValue(info.type, info.face_index, info.firmware_timestamp);
    die.has_roll = true;
  }
  ++revision_;
}

void DiceModel::UpdateTemperature(uint32_t pixel_id,
                                  int16_t mcu_temperature_centi_c,
                                  int16_t battery_temperature_centi_c) {
  Lock lock(mutex_);
  const int die_index = FindDie(pixel_id);
  if (die_index < 0) {
    return;
  }
  Die &die = dice_[die_index];
  die.mcu_temperature_centi_c = mcu_temperature_centi_c;
  die.battery_temperature_centi_c = battery_temperature_centi_c;
  die.has_temperature = true;
  ++revision_;
}

uint32_t DiceModel::Revision() const {
  Lock lock(mutex_);
  return revision_;
}

Snapshot DiceModel::GetSnapshot(uint64_t now_ms) const {
  Lock lock(mutex_);
  Snapshot snapshot;
  snapshot.dice = dice_;
  snapshot.candidates = candidates_;
  snapshot.dice_count = dice_count_;
  snapshot.candidate_count = candidate_count_;
  snapshot.history_count = history_count_;
  for (std::size_t i = 0; i < history_count_; ++i) {
    const std::size_t index =
        (history_next_ + history_.size() - 1U - i) % history_.size();
    snapshot.history[i] = history_[index];
  }
  snapshot.aggregate_mode = aggregate_mode_;
  snapshot.aggregate_roll_count = aggregate_roll_count_;
  snapshot.revision = revision_;
  snapshot.has_aggregate = aggregate_roll_count_ > 0;
  snapshot.calculator = calculator_.GetSnapshot();
  switch (aggregate_mode_) {
  case AggregateMode::kSum:
    snapshot.aggregate_value = aggregate_sum_;
    break;
  case AggregateMode::kHigh:
    snapshot.aggregate_value = aggregate_high_;
    break;
  case AggregateMode::kLow:
    snapshot.aggregate_value = aggregate_low_;
    break;
  }

  std::size_t output = 0;
  for (std::size_t i = 0; i < snapshot.candidate_count; ++i) {
    if (now_ms - snapshot.candidates[i].last_seen_ms <= kCandidateLifetimeMs) {
      snapshot.candidates[output++] = snapshot.candidates[i];
    }
  }
  snapshot.candidate_count = output;
  return snapshot;
}

RollHistory DiceModel::GetRollHistory(uint32_t pixel_id) const {
  Lock lock(mutex_);
  RollHistory result;
  const int die_index = FindDie(pixel_id);
  if (die_index < 0) {
    return result;
  }
  const std::size_t index = static_cast<std::size_t>(die_index);
  result.count = die_history_count_[index];
  for (std::size_t i = 0; i < result.count; ++i) {
    const std::size_t event_index =
        (die_history_next_[index] + kRollHistorySize - 1U - i) %
        kRollHistorySize;
    result.events[i] = die_history_[index][event_index];
  }
  return result;
}

} // namespace app

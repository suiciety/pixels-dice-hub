#include "dice_model.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace app {
namespace {

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
}

void DiceModel::Ingest(const pixels::Advertisement &advertisement,
                       uint64_t now_ms) {
  Lock lock(mutex_);
  const int die_index = FindDie(advertisement.pixel_id);
  if (die_index >= 0) {
    Die &die = dice_[die_index];
    const bool completed_roll =
        advertisement.roll_state == pixels::RollState::kRolled &&
        die.last_seen_ms != 0 &&
        die.roll_state != pixels::RollState::kRolled;
    if (ApplyAdvertisement(&die, advertisement, now_ms)) {
      if (completed_roll) {
        AddRollEvent(static_cast<std::size_t>(die_index), now_ms);
      }
      ++revision_;
    }
    return;
  }

  int candidate_index = FindCandidate(advertisement.pixel_id);
  bool changed = false;
  if (candidate_index < 0) {
    if (candidate_count_ < candidates_.size()) {
      candidate_index = static_cast<int>(candidate_count_++);
      changed = true;
    } else {
      candidate_index = static_cast<int>(candidate_count_ - 1);
    }
  }
  const Die previous = candidates_[candidate_index];
  changed = ApplyAdvertisement(&candidates_[candidate_index], advertisement,
                               now_ms) ||
            changed || std::abs(previous.rssi - advertisement.rssi) >= 5;
  std::sort(
      candidates_.begin(), candidates_.begin() + candidate_count_,
      [](const Die &left, const Die &right) { return left.rssi > right.rssi; });
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
  for (std::size_t i = index + 1; i < dice_count_; ++i) {
    dice_[i - 1] = dice_[i];
    die_history_[i - 1] = die_history_[i];
    die_history_count_[i - 1] = die_history_count_[i];
    die_history_next_[i - 1] = die_history_next_[i];
  }
  --dice_count_;
  dice_[dice_count_] = {};
  die_history_[dice_count_] = {};
  die_history_count_[dice_count_] = 0;
  die_history_next_[dice_count_] = 0;
  ++revision_;
  return true;
}

void DiceModel::RestorePaired(const uint32_t *pixel_ids, std::size_t count) {
  Lock lock(mutex_);
  dice_count_ = std::min(count, dice_.size());
  for (std::size_t i = 0; i < dice_count_; ++i) {
    dice_[i] = {};
    dice_[i].pixel_id = pixel_ids[i];
  }
  die_history_ = {};
  die_history_count_ = {};
  die_history_next_ = {};
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

  constexpr uint64_t kCandidateLifetimeMs = 15000;
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

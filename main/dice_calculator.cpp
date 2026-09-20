#include "dice_calculator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace app {
namespace {

template <typename T> T Clamp(T value, T minimum, T maximum) {
  return std::max(minimum, std::min(value, maximum));
}

bool IsD20Preset(CalculatorPreset preset) {
  return preset == CalculatorPreset::kD20Check ||
         preset == CalculatorPreset::kAdvantage ||
         preset == CalculatorPreset::kDisadvantage;
}

int PoolValue(pixels::DieType type, int value) {
  if (type == pixels::DieType::kD10) {
    if (value < 0 || value > 9) {
      return -1;
    }
    return value == 0 ? 10 : value;
  }
  const int sides = DieSideCount(type);
  if (type == pixels::DieType::kD00 || sides == 0 || value < 1 ||
      value > sides) {
    return -1;
  }
  return value;
}

} // namespace

int DieSideCount(pixels::DieType type) {
  switch (type) {
  case pixels::DieType::kD4:
    return 4;
  case pixels::DieType::kD6:
  case pixels::DieType::kD6Pipped:
    return 6;
  case pixels::DieType::kD8:
    return 8;
  case pixels::DieType::kD10:
  case pixels::DieType::kD00:
    return 10;
  case pixels::DieType::kD12:
    return 12;
  case pixels::DieType::kD20:
    return 20;
  case pixels::DieType::kUnknown:
  case pixels::DieType::kD6Fudge:
  default:
    return 0;
  }
}

int NormalizeDieOutcome(pixels::DieType type, int value) {
  const int sides = DieSideCount(type);
  if (sides == 0) {
    return -1;
  }
  if (type == pixels::DieType::kD10) {
    return value >= 0 && value <= 9 ? value : -1;
  }
  if (type == pixels::DieType::kD00) {
    return value >= 0 && value <= 90 && value % 10 == 0 ? value / 10 : -1;
  }
  return value >= 1 && value <= sides ? value - 1 : -1;
}

const char *CalculatorPresetName(CalculatorPreset preset) {
  switch (preset) {
  case CalculatorPreset::kD20Check:
    return "D20 CHECK";
  case CalculatorPreset::kAdvantage:
    return "ADVANTAGE";
  case CalculatorPreset::kDisadvantage:
    return "DISADVANTAGE";
  case CalculatorPreset::kSuccessPool:
    return "SUCCESS POOL";
  case CalculatorPreset::kKeepHighest:
    return "KEEP HIGHEST";
  case CalculatorPreset::kKeepLowest:
    return "KEEP LOWEST";
  case CalculatorPreset::kDamagePool:
    return "DAMAGE";
  default:
    return "CALCULATOR";
  }
}

const char *CalculatorStatusName(CalculatorStatus status) {
  switch (status) {
  case CalculatorStatus::kIdle:
    return "IDLE";
  case CalculatorStatus::kRolling:
    return "ROLLING";
  case CalculatorStatus::kComplete:
    return "COMPLETE";
  case CalculatorStatus::kError:
    return "ERROR";
  default:
    return "UNKNOWN";
  }
}

const char *CalculatorPoolDieName(uint8_t sides) {
  switch (sides) {
  case 4:
    return "D4";
  case 6:
    return "D6";
  case 8:
    return "D8";
  case 10:
    return "D10";
  case 12:
    return "D12";
  case 20:
    return "D20";
  default:
    return "AUTO";
  }
}

const CalculatorConfig &DiceCalculator::Config() const { return config_; }

void DiceCalculator::Adjust(CalculatorSetting setting, int delta) {
  if (status_ == CalculatorStatus::kRolling) {
    return;
  }
  switch (setting) {
  case CalculatorSetting::kPreset: {
    constexpr int kPresetCount = 7;
    int value = static_cast<int>(config_.preset) + delta;
    value = (value % kPresetCount + kPresetCount) % kPresetCount;
    config_.preset = static_cast<CalculatorPreset>(value);
    break;
  }
  case CalculatorSetting::kModifier:
    config_.modifier =
        static_cast<int8_t>(Clamp<int>(config_.modifier + delta, -20, 20));
    break;
  case CalculatorSetting::kDc:
    config_.dc = static_cast<uint8_t>(Clamp<int>(config_.dc + delta, 1, 40));
    break;
  case CalculatorSetting::kDcEnabled:
    config_.dc_enabled = !config_.dc_enabled;
    break;
  case CalculatorSetting::kThreshold:
    config_.threshold =
        static_cast<uint8_t>(Clamp<int>(config_.threshold + delta, 1, 20));
    break;
  case CalculatorSetting::kPoolCount:
    config_.pool_count = static_cast<uint8_t>(
        Clamp<int>(config_.pool_count + delta, 1, kCalculatorMaxRolls));
    config_.keep_count =
        std::min(config_.keep_count, config_.pool_count);
    break;
  case CalculatorSetting::kKeepCount:
    config_.keep_count = static_cast<uint8_t>(
        Clamp<int>(config_.keep_count + delta, 1, config_.pool_count));
    break;
  case CalculatorSetting::kCritical:
    config_.critical = !config_.critical;
    break;
  case CalculatorSetting::kPoolSides: {
    constexpr std::array<uint8_t, 7> kChoices = {0, 4, 6, 8, 10, 12, 20};
    std::size_t index = 0;
    while (index + 1 < kChoices.size() &&
           kChoices[index] != config_.pool_sides) {
      ++index;
    }
    const int count = static_cast<int>(kChoices.size());
    int next = static_cast<int>(index) + delta;
    next = (next % count + count) % count;
    config_.pool_sides = kChoices[static_cast<std::size_t>(next)];
    break;
  }
  }
  status_ = CalculatorStatus::kIdle;
  has_result_ = false;
  has_check_ = false;
  UpdateText();
}

bool DiceCalculator::BuildD20Recipe(const CalculatorDie *dice,
                                    std::size_t count) {
  std::array<CalculatorDie, kCalculatorMaxDice> d20_dice{};
  std::size_t d20_count = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (dice[i].type == pixels::DieType::kD20) {
      d20_dice[d20_count++] = dice[i];
    }
  }
  if (d20_count != 0) {
    recipe_[0] = {d20_dice[0], 20};
    recipe_count_ = 1;
    direct_d20_pool_ = true;
    pool_die_count_ = static_cast<uint8_t>(d20_count);
    pool_sides_ = 20;
    for (uint8_t i = 0; i < pool_die_count_; ++i) {
      pool_dice_[i] = d20_dice[i];
    }
    return true;
  }

  uint32_t best_product = 0;
  uint32_t best_accept = 0;
  uint8_t best_count = 0;
  std::array<std::size_t, 3> best_indices{};
  for (uint8_t length = 2; length <= 3; ++length) {
    const std::size_t combinations =
        length == 2 ? count * count : count * count * count;
    for (std::size_t combination = 0; combination < combinations;
         ++combination) {
      std::size_t encoded = combination;
      uint32_t product = 1;
      std::array<std::size_t, 3> indices{};
      bool supported = true;
      for (uint8_t step = 0; step < length; ++step) {
        indices[length - 1 - step] = encoded % count;
        encoded /= count;
        const int sides = DieSideCount(dice[indices[length - 1 - step]].type);
        if (sides == 0) {
          supported = false;
          break;
        }
        product *= static_cast<uint32_t>(sides);
      }
      if (!supported || product < 20) {
        continue;
      }
      const uint32_t accepted = (product / 20U) * 20U;
      const uint64_t expected_numerator =
          static_cast<uint64_t>(length) * product;
      const uint64_t best_expected_numerator =
          static_cast<uint64_t>(best_count) * best_product;
      if (best_count == 0 ||
          expected_numerator * best_accept <
              best_expected_numerator * accepted ||
          (expected_numerator * best_accept ==
               best_expected_numerator * accepted &&
           accepted * best_product > best_accept * product)) {
        best_count = length;
        best_product = product;
        best_accept = accepted;
        best_indices = indices;
      }
    }
  }
  if (best_count == 0) {
    return false;
  }
  recipe_count_ = best_count;
  for (uint8_t i = 0; i < recipe_count_; ++i) {
    recipe_[i] = {dice[best_indices[i]],
                  static_cast<uint8_t>(
                      DieSideCount(dice[best_indices[i]].type))};
  }
  return true;
}

bool DiceCalculator::BuildPool(const CalculatorDie *dice, std::size_t count) {
  int selected_sides = config_.pool_sides;
  std::size_t selected_count = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const int sides = DieSideCount(dice[i].type);
    if (sides == 0 || dice[i].type == pixels::DieType::kD00) {
      continue;
    }
    if (config_.pool_sides != 0 && sides != config_.pool_sides) {
      continue;
    }
    if (config_.preset == CalculatorPreset::kSuccessPool &&
        sides < config_.threshold) {
      continue;
    }
    std::size_t equivalent_count = 0;
    for (std::size_t candidate = 0; candidate < count; ++candidate) {
      if (dice[candidate].type != pixels::DieType::kD00 &&
          DieSideCount(dice[candidate].type) == sides) {
        ++equivalent_count;
      }
    }
    if (equivalent_count > selected_count ||
        (equivalent_count == selected_count && sides > selected_sides)) {
      selected_sides = sides;
      selected_count = equivalent_count;
    }
  }
  if (selected_count == 0) {
    return false;
  }
  logical_count_ = config_.pool_count;
  for (std::size_t i = 0; i < count; ++i) {
    if (dice[i].type != pixels::DieType::kD00 &&
        DieSideCount(dice[i].type) == selected_sides) {
      pool_dice_[pool_die_count_++] = dice[i];
    }
  }
  pool_sides_ = static_cast<uint8_t>(selected_sides);
  return true;
}

bool DiceCalculator::Start(const CalculatorDie *dice, std::size_t count) {
  status_ = CalculatorStatus::kIdle;
  recipe_count_ = 0;
  recipe_index_ = 0;
  logical_index_ = 0;
  mixed_value_ = 0;
  mixed_product_ = 1;
  values_ = {};
  pool_die_count_ = 0;
  pool_sides_ = 0;
  direct_d20_pool_ = false;
  has_result_ = false;
  has_check_ = false;
  last_attempt_rejected_ = false;
  count = std::min(count, kCalculatorMaxDice);

  if (dice == nullptr || count == 0) {
    status_ = CalculatorStatus::kError;
    std::snprintf(instruction_, sizeof(instruction_), "Pair a supported die");
    return false;
  }
  if (IsD20Preset(config_.preset)) {
    logical_count_ =
        config_.preset == CalculatorPreset::kD20Check ? 1 : 2;
    if (!BuildD20Recipe(dice, count)) {
      status_ = CalculatorStatus::kError;
      std::snprintf(instruction_, sizeof(instruction_),
                    "Need D20 or D4/D6/D8/D10/D00/D12");
      return false;
    }
  } else if (!BuildPool(dice, count)) {
    status_ = CalculatorStatus::kError;
    if (config_.pool_sides != 0) {
      std::snprintf(instruction_, sizeof(instruction_),
                    "No usable paired %s dice",
                    CalculatorPoolDieName(config_.pool_sides));
    } else if (config_.preset == CalculatorPreset::kSuccessPool) {
      std::snprintf(instruction_, sizeof(instruction_),
                    "No paired die can reach target %u", config_.threshold);
    } else {
      std::snprintf(instruction_, sizeof(instruction_),
                    "No supported paired dice");
    }
    return false;
  }
  status_ = CalculatorStatus::kRolling;
  BeginLogicalRoll();
  return true;
}

void DiceCalculator::Cancel() {
  status_ = CalculatorStatus::kIdle;
  logical_index_ = 0;
  recipe_index_ = 0;
  has_result_ = false;
  has_check_ = false;
  last_attempt_rejected_ = false;
  UpdateText();
}

void DiceCalculator::BeginLogicalRoll() {
  recipe_index_ = 0;
  mixed_value_ = 0;
  mixed_product_ = 1;
  UpdateText();
}

bool DiceCalculator::Consume(const CalculatorRoll &roll) {
  if (status_ != CalculatorStatus::kRolling) {
    return false;
  }
  if (!IsD20Preset(config_.preset) || direct_d20_pool_) {
    return ConsumeFlexibleRoll(roll);
  }
  const CalculatorDie &expected = recipe_[recipe_index_].die;
  if (roll.pixel_id != expected.pixel_id || roll.type != expected.type) {
    return false;
  }

  const int normalized = NormalizeDieOutcome(roll.type, roll.value);
  if (normalized < 0) {
    return false;
  }
  mixed_value_ =
      mixed_value_ * recipe_[recipe_index_].sides +
      static_cast<uint32_t>(normalized);
  mixed_product_ *= recipe_[recipe_index_].sides;
  ++recipe_index_;
  if (recipe_index_ < recipe_count_) {
    last_attempt_rejected_ = false;
    UpdateText();
    return true;
  }

  const uint32_t accepted_states = (mixed_product_ / 20U) * 20U;
  if (mixed_value_ >= accepted_states) {
    recipe_index_ = 0;
    mixed_value_ = 0;
    mixed_product_ = 1;
    last_attempt_rejected_ = true;
    UpdateText();
    return true;
  }
  last_attempt_rejected_ = false;
  CompleteLogicalRoll(static_cast<int>(mixed_value_ % 20U) + 1);
  return true;
}

bool DiceCalculator::ConsumeFlexibleRoll(const CalculatorRoll &roll) {
  if (pool_die_count_ == 0 || logical_index_ >= logical_count_) {
    return false;
  }
  
  // When pool contains multiple die types, accept any type that can accept the value
  // Otherwise require exact pixel_id and type match
  const bool multi_type_pool = pool_die_count_ > 1;
  
  // First pass: try to find an exact match for pixel_id + type
  for (uint8_t i = 0; i < pool_die_count_; ++i) {
    const bool pixel_matches =
        roll.pixel_id == pool_dice_[i].pixel_id || roll.pixel_id == 0;
    const bool type_matches = pool_dice_[i].type == roll.type;
    
    if (!pixel_matches || !type_matches) {
      continue;
    }
    
    const int value = PoolValue(roll.type, roll.value);
    if (value < 1) {
      if (!multi_type_pool) {
        return false;  // Single-type: this is the only die we could use
      }
      continue;  // Multi-type: try other types
    }
    
    values_[logical_index_++] = value;
    if (logical_index_ >= logical_count_) {
      CompleteSession();
    } else {
      UpdateText();
    }
    return true;
  }
  
  // If no exact match found in multi-type pool, try any type that accepts the value
  if (multi_type_pool) {
    for (uint8_t i = 0; i < pool_die_count_; ++i) {
      const int value = PoolValue(pool_dice_[i].type, roll.value);
      if (value < 1) {
        continue;  // This die type can't accept this value
      }
      
      values_[logical_index_++] = value;
      if (logical_index_ >= logical_count_) {
        CompleteSession();
      } else {
        UpdateText();
      }
      return true;
    }
  }
  
  return false;
}

const CalculatorDie *DiceCalculator::FirstPoolDie() const {
  if (pool_die_count_ == 0 || logical_index_ >= logical_count_) {
    return nullptr;
  }
  return &pool_dice_[0];
}

void DiceCalculator::CompleteLogicalRoll(int value) {
  values_[logical_index_++] = value;
  if (logical_index_ >= logical_count_) {
    CompleteSession();
  } else {
    BeginLogicalRoll();
  }
}

void DiceCalculator::CompleteSession() {
  int dice_total = 0;
  for (uint8_t i = 0; i < logical_count_; ++i) {
    dice_total += values_[i];
  }
  switch (config_.preset) {
  case CalculatorPreset::kD20Check:
    result_ = values_[0] + config_.modifier;
    if (config_.dc_enabled) {
      has_check_ = true;
      check_passed_ = result_ >= config_.dc;
    }
    break;
  case CalculatorPreset::kAdvantage:
    result_ = std::max(values_[0], values_[1]) + config_.modifier;
    break;
  case CalculatorPreset::kDisadvantage:
    result_ = std::min(values_[0], values_[1]) + config_.modifier;
    break;
  case CalculatorPreset::kSuccessPool:
    result_ = 0;
    for (uint8_t i = 0; i < logical_count_; ++i) {
      if (values_[i] >= config_.threshold) {
        ++result_;
      }
    }
    break;
  case CalculatorPreset::kKeepHighest:
  case CalculatorPreset::kKeepLowest: {
    std::sort(values_.begin(), values_.begin() + logical_count_);
    result_ = 0;
    const uint8_t keep = std::min(config_.keep_count, logical_count_);
    for (uint8_t i = 0; i < keep; ++i) {
      result_ += config_.preset == CalculatorPreset::kKeepHighest
                     ? values_[logical_count_ - 1U - i]
                     : values_[i];
    }
    break;
  }
  case CalculatorPreset::kDamagePool:
    result_ = dice_total * (config_.critical ? 2 : 1) + config_.modifier;
    break;
  }
  status_ = CalculatorStatus::kComplete;
  has_result_ = true;
  UpdateText();
}

void DiceCalculator::UpdateText() {
  instruction_[0] = '\0';
  result_text_[0] = '\0';
  if (status_ == CalculatorStatus::kComplete) {
    if (has_check_) {
      std::snprintf(result_text_, sizeof(result_text_), "%d - %s DC %u",
                    result_, check_passed_ ? "SUCCESS" : "FAIL", config_.dc);
    } else {
      std::snprintf(result_text_, sizeof(result_text_), "%d", result_);
    }
    std::snprintf(instruction_, sizeof(instruction_), "Round complete");
    return;
  }
  if (status_ != CalculatorStatus::kRolling) {
    std::snprintf(instruction_, sizeof(instruction_), "Configure and start");
    return;
  }

  const bool batch_roll = !IsD20Preset(config_.preset) || direct_d20_pool_;
  const CalculatorDie *pending =
      batch_roll ? FirstPoolDie() : &recipe_[recipe_index_].die;
  if (pending == nullptr) {
    return;
  }
  const CalculatorDie &die = *pending;
  const char *name = die.name[0] == '\0' ? "Pixel" : die.name;
  if (!batch_roll) {
    std::snprintf(
        instruction_, sizeof(instruction_), "%sRoll %s %s - D20 %u/%u, step %u/%u",
        last_attempt_rejected_ ? "Rejected; reroll. " : "",
        pixels::DieTypeName(die.type), name, logical_index_ + 1U,
        logical_count_, recipe_index_ + 1U, recipe_count_);
  } else if (pool_die_count_ > 1) {
    const uint8_t remaining =
        static_cast<uint8_t>(logical_count_ - logical_index_);
    const uint8_t maximum =
        std::min<uint8_t>(pool_die_count_, remaining);
    std::snprintf(instruction_, sizeof(instruction_),
                  "Roll 1-%u D%u dice - %u/%u complete", maximum,
                  pool_sides_, logical_index_, logical_count_);
  } else {
    std::snprintf(instruction_, sizeof(instruction_), "Roll %s %s - %u/%u",
                  pixels::DieTypeName(die.type), name, logical_index_ + 1U,
                  logical_count_);
  }
}

CalculatorSnapshot DiceCalculator::GetSnapshot() const {
  CalculatorSnapshot snapshot;
  snapshot.config = config_;
  snapshot.status = status_;
  snapshot.completed_rolls = logical_index_;
  snapshot.total_rolls = logical_count_;
  snapshot.recipe_step = recipe_index_;
  snapshot.recipe_steps = recipe_count_;
  snapshot.result = result_;
  snapshot.has_result = has_result_;
  snapshot.check_passed = check_passed_;
  snapshot.has_check = has_check_;
  snapshot.last_attempt_rejected = last_attempt_rejected_;
  if (status_ == CalculatorStatus::kRolling) {
    const bool batch_roll = !IsD20Preset(config_.preset) || direct_d20_pool_;
    const CalculatorDie *die =
        batch_roll ? FirstPoolDie() : &recipe_[recipe_index_].die;
    if (die != nullptr) {
      snapshot.expected_pixel_id =
          batch_roll && pool_die_count_ > 1 ? 0 : die->pixel_id;
      snapshot.expected_type = die->type;
      std::strncpy(snapshot.expected_name, die->name,
                   sizeof(snapshot.expected_name) - 1);
    }
  }
  std::strncpy(snapshot.instruction, instruction_,
               sizeof(snapshot.instruction) - 1);
  std::strncpy(snapshot.result_text, result_text_,
               sizeof(snapshot.result_text) - 1);
  return snapshot;
}

} // namespace app

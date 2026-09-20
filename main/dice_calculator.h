#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "pixels_protocol.h"

namespace app {

constexpr std::size_t kCalculatorMaxDice = 8;
constexpr std::size_t kCalculatorMaxRolls = 16;

enum class CalculatorPreset : uint8_t {
  kD20Check = 0,
  kAdvantage,
  kDisadvantage,
  kSuccessPool,
  kKeepHighest,
  kKeepLowest,
  kDamagePool,
};

enum class CalculatorStatus : uint8_t {
  kIdle = 0,
  kRolling,
  kComplete,
  kError,
};

enum class CalculatorSetting : uint8_t {
  kPreset = 0,
  kModifier,
  kDc,
  kDcEnabled,
  kThreshold,
  kPoolCount,
  kKeepCount,
  kCritical,
  kPoolSides,
};

struct CalculatorConfig {
  CalculatorPreset preset = CalculatorPreset::kD20Check;
  int8_t modifier = 0;
  uint8_t dc = 10;
  uint8_t threshold = 5;
  uint8_t pool_count = 1;
  uint8_t keep_count = 1;
  uint8_t pool_sides = 0;
  bool dc_enabled = false;
  bool critical = false;
};

struct CalculatorDie {
  uint32_t pixel_id = 0;
  pixels::DieType type = pixels::DieType::kUnknown;
  char name[pixels::kMaxNameLength + 1] = {};
};

struct CalculatorRoll {
  uint32_t pixel_id = 0;
  pixels::DieType type = pixels::DieType::kUnknown;
  int value = 0;
};

struct CalculatorSnapshot {
  CalculatorConfig config{};
  CalculatorStatus status = CalculatorStatus::kIdle;
  uint8_t completed_rolls = 0;
  uint8_t total_rolls = 0;
  uint8_t recipe_step = 0;
  uint8_t recipe_steps = 0;
  uint32_t expected_pixel_id = 0;
  pixels::DieType expected_type = pixels::DieType::kUnknown;
  int result = 0;
  bool has_result = false;
  bool check_passed = false;
  bool has_check = false;
  bool last_attempt_rejected = false;
  char expected_name[pixels::kMaxNameLength + 1] = {};
  char instruction[128] = {};
  char result_text[96] = {};
};

int DieSideCount(pixels::DieType type);
int NormalizeDieOutcome(pixels::DieType type, int value);
const char *CalculatorPresetName(CalculatorPreset preset);
const char *CalculatorStatusName(CalculatorStatus status);
const char *CalculatorPoolDieName(uint8_t sides);

class DiceCalculator {
public:
  const CalculatorConfig &Config() const;
  void Adjust(CalculatorSetting setting, int delta);
  bool Start(const CalculatorDie *dice, std::size_t count);
  void Cancel();
  bool Consume(const CalculatorRoll &roll);
  CalculatorSnapshot GetSnapshot() const;

private:
  struct RecipeStep {
    CalculatorDie die{};
    uint8_t sides = 0;
  };

  bool BuildD20Recipe(const CalculatorDie *dice, std::size_t count);
  bool BuildPool(const CalculatorDie *dice, std::size_t count);
  bool ConsumeFlexibleRoll(const CalculatorRoll &roll);
  const CalculatorDie *FirstPoolDie() const;
  void BeginLogicalRoll();
  void CompleteLogicalRoll(int value);
  void CompleteSession();
  void UpdateText();

  CalculatorConfig config_{};
  CalculatorStatus status_ = CalculatorStatus::kIdle;
  std::array<RecipeStep, 3> recipe_{};
  uint8_t recipe_count_ = 0;
  uint8_t recipe_index_ = 0;
  uint32_t mixed_value_ = 0;
  uint32_t mixed_product_ = 1;
  std::array<CalculatorDie, kCalculatorMaxDice> pool_dice_{};
  std::array<int, kCalculatorMaxRolls> values_{};
  uint8_t pool_die_count_ = 0;
  uint8_t pool_sides_ = 0;
  uint8_t logical_count_ = 0;
  uint8_t logical_index_ = 0;
  bool direct_d20_pool_ = false;
  int result_ = 0;
  bool has_result_ = false;
  bool check_passed_ = false;
  bool has_check_ = false;
  bool last_attempt_rejected_ = false;
  char instruction_[128] = "Configure and start";
  char result_text_[96] = {};
};

} // namespace app

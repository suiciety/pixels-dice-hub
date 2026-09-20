#include <cassert>
#include <cstring>

#include "dice_calculator.h"

namespace {

app::CalculatorDie Die(uint32_t id, pixels::DieType type, const char *name) {
  app::CalculatorDie die;
  die.pixel_id = id;
  die.type = type;
  std::strncpy(die.name, name, sizeof(die.name) - 1);
  return die;
}

void SetPreset(app::DiceCalculator *calculator,
               app::CalculatorPreset preset) {
  while (calculator->Config().preset != preset) {
    calculator->Adjust(app::CalculatorSetting::kPreset, 1);
  }
}

void SetValue(app::DiceCalculator *calculator,
              app::CalculatorSetting setting, int current, int desired) {
  calculator->Adjust(setting, desired - current);
}

void RollExpected(app::DiceCalculator *calculator, int value) {
  const app::CalculatorSnapshot snapshot = calculator->GetSnapshot();
  assert(snapshot.status == app::CalculatorStatus::kRolling);
  assert(calculator->Consume(
      {snapshot.expected_pixel_id, snapshot.expected_type, value}));
}

} // namespace

int main() {
  using pixels::DieType;
  assert(app::DieSideCount(DieType::kD4) == 4);
  assert(app::DieSideCount(DieType::kD6) == 6);
  assert(app::DieSideCount(DieType::kD6Pipped) == 6);
  assert(app::DieSideCount(DieType::kD8) == 8);
  assert(app::DieSideCount(DieType::kD10) == 10);
  assert(app::DieSideCount(DieType::kD00) == 10);
  assert(app::DieSideCount(DieType::kD12) == 12);
  assert(app::DieSideCount(DieType::kD20) == 20);
  assert(app::DieSideCount(DieType::kD6Fudge) == 0);
  assert(app::NormalizeDieOutcome(DieType::kD4, 1) == 0);
  assert(app::NormalizeDieOutcome(DieType::kD4, 4) == 3);
  assert(app::NormalizeDieOutcome(DieType::kD10, 0) == 0);
  assert(app::NormalizeDieOutcome(DieType::kD10, 9) == 9);
  assert(app::NormalizeDieOutcome(DieType::kD00, 90) == 9);
  assert(app::NormalizeDieOutcome(DieType::kD6Fudge, 1) == -1);

  {
    app::DiceCalculator calculator;
    const auto d4 = Die(1, DieType::kD4, "Blue");
    assert(calculator.Start(&d4, 1));
    auto snapshot = calculator.GetSnapshot();
    assert(snapshot.recipe_steps == 3);
    RollExpected(&calculator, 4);
    RollExpected(&calculator, 4);
    RollExpected(&calculator, 4);
    snapshot = calculator.GetSnapshot();
    assert(snapshot.status == app::CalculatorStatus::kRolling);
    assert(snapshot.last_attempt_rejected);
    assert(snapshot.completed_rolls == 0);
    RollExpected(&calculator, 1);
    RollExpected(&calculator, 1);
    RollExpected(&calculator, 1);
    snapshot = calculator.GetSnapshot();
    assert(snapshot.status == app::CalculatorStatus::kComplete);
    assert(snapshot.result == 1);
  }

  {
    app::DiceCalculator calculator;
    const auto d20 = Die(20, DieType::kD20, "Red");
    calculator.Adjust(app::CalculatorSetting::kDcEnabled, 1);
    SetValue(&calculator, app::CalculatorSetting::kDc,
             calculator.Config().dc, 15);
    SetValue(&calculator, app::CalculatorSetting::kModifier,
             calculator.Config().modifier, 2);
    assert(calculator.Start(&d20, 1));
    assert(!calculator.Consume({99, DieType::kD20, 18}));
    assert(calculator.GetSnapshot().completed_rolls == 0);
    assert(calculator.Consume({20, DieType::kD20, 13}));
    assert(calculator.GetSnapshot().result == 15);
    assert(calculator.GetSnapshot().has_check);
    assert(calculator.GetSnapshot().check_passed);
    assert(!calculator.Consume({20, DieType::kD20, 2}));
  }

  {
    app::DiceCalculator calculator;
    const app::CalculatorDie dice[] = {
        Die(6, DieType::kD6, "D6"), Die(10, DieType::kD10, "D10")};
    assert(calculator.Start(dice, 2));
    assert(calculator.GetSnapshot().recipe_steps == 2);
    const auto first = calculator.GetSnapshot();
    RollExpected(&calculator, first.expected_type == DieType::kD10 ? 0 : 1);
    const auto second = calculator.GetSnapshot();
    RollExpected(&calculator, second.expected_type == DieType::kD10 ? 0 : 1);
    assert(calculator.GetSnapshot().status ==
           app::CalculatorStatus::kComplete);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kAdvantage);
    SetValue(&calculator, app::CalculatorSetting::kModifier,
             calculator.Config().modifier, 2);
    const auto d20 = Die(20, DieType::kD20, "Red");
    assert(calculator.Start(&d20, 1));
    RollExpected(&calculator, 7);
    RollExpected(&calculator, 16);
    assert(calculator.GetSnapshot().result == 18);

    calculator.Cancel();
    SetPreset(&calculator, app::CalculatorPreset::kDisadvantage);
    assert(calculator.Start(&d20, 1));
    RollExpected(&calculator, 7);
    RollExpected(&calculator, 16);
    assert(calculator.GetSnapshot().result == 9);
  }

  const app::CalculatorDie pool[] = {
      Die(1, DieType::kD6, "One"), Die(2, DieType::kD8, "Two")};
  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kSuccessPool);
    SetValue(&calculator, app::CalculatorSetting::kPoolCount,
             calculator.Config().pool_count, 3);
    SetValue(&calculator, app::CalculatorSetting::kThreshold,
             calculator.Config().threshold, 5);
    assert(calculator.Start(pool, 2));
    RollExpected(&calculator, 5);
    RollExpected(&calculator, 4);
    RollExpected(&calculator, 6);
    assert(calculator.GetSnapshot().result == 2);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kSuccessPool);
    SetValue(&calculator, app::CalculatorSetting::kPoolCount,
             calculator.Config().pool_count, 2);
    SetValue(&calculator, app::CalculatorSetting::kThreshold,
             calculator.Config().threshold, 5);
    const app::CalculatorDie d6s[] = {
        Die(61, DieType::kD6, "Blue"), Die(62, DieType::kD6, "Red")};
    assert(calculator.Start(d6s, 2));
    assert(std::strstr(calculator.GetSnapshot().instruction,
                       "Roll 1-2 D6") != nullptr);
    assert(calculator.Consume({62, DieType::kD6, 6}));
    assert(calculator.Consume({61, DieType::kD6, 4}));
    assert(calculator.GetSnapshot().status ==
           app::CalculatorStatus::kComplete);
    assert(calculator.GetSnapshot().result == 1);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kSuccessPool);
    SetValue(&calculator, app::CalculatorSetting::kPoolCount,
             calculator.Config().pool_count, 8);
    const app::CalculatorDie dice[] = {
        Die(61, DieType::kD6, "Blue"), Die(62, DieType::kD6, "Red"),
        Die(8, DieType::kD8, "Other")};
    assert(calculator.Start(dice, 3));
    assert(calculator.Consume({62, DieType::kD6, 6}));
    assert(calculator.Consume({61, DieType::kD6, 5}));
    assert(calculator.GetSnapshot().completed_rolls == 2);
    assert(calculator.GetSnapshot().status ==
           app::CalculatorStatus::kRolling);
    assert(!calculator.Consume({8, DieType::kD8, 8}));
    assert(calculator.Consume({61, DieType::kD6, 4}));
    assert(calculator.Consume({62, DieType::kD6, 3}));
    assert(calculator.Consume({61, DieType::kD6, 2}));
    assert(calculator.Consume({61, DieType::kD6, 6}));
    assert(calculator.Consume({62, DieType::kD6, 1}));
    assert(calculator.Consume({62, DieType::kD6, 5}));
    assert(calculator.GetSnapshot().status ==
           app::CalculatorStatus::kComplete);
    assert(calculator.GetSnapshot().result == 4);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kSuccessPool);
    SetValue(&calculator, app::CalculatorSetting::kPoolCount,
             calculator.Config().pool_count, 1);
    SetValue(&calculator, app::CalculatorSetting::kThreshold,
             calculator.Config().threshold, 7);
    const app::CalculatorDie dice[] = {
        Die(61, DieType::kD6, "Blue"), Die(62, DieType::kD6, "Red"),
        Die(8, DieType::kD8, "Green")};
    assert(calculator.Start(dice, 3));
    assert(!calculator.Consume({61, DieType::kD6, 6}));
    assert(calculator.Consume({8, DieType::kD8, 7}));
    assert(calculator.GetSnapshot().result == 1);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kDamagePool);
    calculator.Adjust(app::CalculatorSetting::kPoolSides, 2);
    const app::CalculatorDie dice[] = {
        Die(61, DieType::kD6, "Blue"), Die(20, DieType::kD20, "Red"),
        Die(21, DieType::kD20, "Green")};
    assert(calculator.Start(dice, 3));
    assert(!calculator.Consume({20, DieType::kD20, 20}));
    assert(calculator.Consume({61, DieType::kD6, 6}));
    assert(calculator.GetSnapshot().result == 6);

    calculator.Cancel();
    calculator.Adjust(app::CalculatorSetting::kPoolSides, 1);
    assert(!calculator.Start(dice, 3));
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kDamagePool);
    const auto d6 = Die(6, DieType::kD6, "D6");
    assert(calculator.Start(&d6, 1));
    assert(!calculator.Consume({6, DieType::kD6, 7}));
    assert(calculator.GetSnapshot().completed_rolls == 0);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kAdvantage);
    const app::CalculatorDie d20s[] = {
        Die(201, DieType::kD20, "Blue"), Die(202, DieType::kD20, "Red")};
    assert(calculator.Start(d20s, 2));
    assert(std::strstr(calculator.GetSnapshot().instruction,
                       "Roll 1-2 D20") != nullptr);
    assert(calculator.Consume({202, DieType::kD20, 17}));
    assert(calculator.Consume({201, DieType::kD20, 8}));
    assert(calculator.GetSnapshot().result == 17);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kKeepHighest);
    SetValue(&calculator, app::CalculatorSetting::kPoolCount,
             calculator.Config().pool_count, 3);
    SetValue(&calculator, app::CalculatorSetting::kKeepCount,
             calculator.Config().keep_count, 2);
    assert(calculator.Start(pool, 2));
    RollExpected(&calculator, 2);
    RollExpected(&calculator, 7);
    RollExpected(&calculator, 5);
    assert(calculator.GetSnapshot().result == 12);

    calculator.Cancel();
    SetPreset(&calculator, app::CalculatorPreset::kKeepLowest);
    assert(calculator.Start(pool, 2));
    RollExpected(&calculator, 2);
    RollExpected(&calculator, 7);
    RollExpected(&calculator, 5);
    assert(calculator.GetSnapshot().result == 7);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kDamagePool);
    SetValue(&calculator, app::CalculatorSetting::kPoolCount,
             calculator.Config().pool_count, 2);
    SetValue(&calculator, app::CalculatorSetting::kModifier,
             calculator.Config().modifier, 3);
    calculator.Adjust(app::CalculatorSetting::kCritical, 1);
    assert(calculator.Start(pool, 2));
    RollExpected(&calculator, 4);
    RollExpected(&calculator, 6);
    assert(calculator.GetSnapshot().result == 23);
  }

  {
    app::DiceCalculator calculator;
    const auto d6 = Die(6, DieType::kD6, "D6");
    assert(calculator.Start(&d6, 1));
    assert(calculator.GetSnapshot().recipe_steps == 3);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kSuccessPool);
    SetValue(&calculator, app::CalculatorSetting::kPoolCount,
             calculator.Config().pool_count, 1);
    SetValue(&calculator, app::CalculatorSetting::kThreshold,
             calculator.Config().threshold, 10);
    const auto d10 = Die(10, DieType::kD10, "D10");
    assert(calculator.Start(&d10, 1));
    RollExpected(&calculator, 0);
    assert(calculator.GetSnapshot().result == 1);
  }

  {
    app::DiceCalculator calculator;
    SetPreset(&calculator, app::CalculatorPreset::kSuccessPool);
    const auto d00 = Die(100, DieType::kD00, "D00");
    assert(!calculator.Start(&d00, 1));
  }

  return 0;
}

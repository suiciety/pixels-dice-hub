#pragma once

#include "dice_model.h"

namespace app {

void RestorePreferences(DiceModel *model);
bool SavePreferences(const DiceModel &model);

} // namespace app

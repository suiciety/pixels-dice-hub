#pragma once

#include "board.h"
#include "dice_model.h"
#include "esp_err.h"

namespace app {

esp_err_t StartUi(board::Hardware *hardware, DiceModel *model);

} // namespace app

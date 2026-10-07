// Ui.h - the touchscreen: a setup screen to choose cycle counts, and a run screen to watch and
// pause/resume the servos. All drawing and all touch handling lives in Ui.cpp; the cycling logic
// knows nothing about the screen, so the UI can be redesigned without touching it.
#pragma once
#include <Arduino.h>
#include "CycleChannel.h"

namespace Ui {
  void begin(CycleController& controller);   // call once from setup(): starts the display and touch controller
  void update(uint32_t now);                 // call every pass of loop(): reads touches, redraws what changed
}

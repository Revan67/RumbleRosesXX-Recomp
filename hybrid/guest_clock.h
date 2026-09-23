#pragma once
#include "context.h"
#include <rex/chrono/clock.h>
// Applied only to generated units that read the guest timebase. No generated
// source edits; host diagnostics keep the real CPU timestamp intrinsic.
#define __rdtsc() rex::chrono::Clock::QueryGuestTickCount()

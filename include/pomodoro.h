#pragma once
#include <stdint.h>

// Uses elapsed milliseconds, so timer progress is independent of clock sync
// and continues while another function page is open. Safe across millis wrap.
class Pomodoro {
 public:
  uint16_t workMinutes = 25;
  uint16_t breakMinutes = 5;
  uint32_t completed = 0;
  bool running = false;
  bool resting = false;
  bool started = false;
  uint32_t phaseRemainingMs = 25UL * 60000;
  uint32_t completedAt = 0;

  void tick(uint32_t now) {
    uint32_t elapsed = now - lastTick_;
    lastTick_ = now;
    if (!running) return;
    while (elapsed >= phaseRemainingMs) {
      elapsed -= phaseRemainingMs;
      if (!resting) ++completed;
      resting = !resting;
      phaseRemainingMs = fullDurationMs();
      completedAt = now;
    }
    phaseRemainingMs -= elapsed;
  }

  void toggle(uint32_t now) {
    tick(now);
    running = !running;
    started = true;
  }
  void reset(uint32_t now) {
    running = resting = started = false;
    phaseRemainingMs = uint32_t(workMinutes) * 60000;
    lastTick_ = now;
    completedAt = 0;
  }
  void adjust(int minutes, uint32_t now) {
    if (started) return;
    const int adjusted = int(workMinutes) + minutes;
    workMinutes = adjusted < 5 ? 5 : adjusted > 90 ? 90 : adjusted;
    reset(now);
  }
  uint32_t fullDurationMs() const {
    return uint32_t(resting ? breakMinutes : workMinutes) * 60000;
  }
  uint32_t secondsLeft() const { return (phaseRemainingMs + 999) / 1000; }
  float progress() const { return 1.0f - float(phaseRemainingMs) / fullDurationMs(); }

 private:
  uint32_t lastTick_ = 0;
};

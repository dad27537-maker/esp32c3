#pragma once
#include <stdint.h>

class DebouncedButton {
 public:
  bool pressed() const { return stable_; }
  // A key held during boot must be released before it can trigger a shortcut.
  void begin(bool pressed, uint32_t now) {
    raw_ = stable_ = pressed;
    changedAt_ = now;
    armed_ = !pressed;
  }

  bool update(bool pressed, uint32_t now, uint32_t debounceMs) {
    if (pressed != raw_) {
      raw_ = pressed;
      changedAt_ = now;
    }
    if (stable_ == raw_ || uint32_t(now - changedAt_) < debounceMs) return false;
    stable_ = raw_;
    if (!stable_) {
      armed_ = true;
      return false;
    }
    if (!armed_) return false;
    armed_ = false;
    return true;
  }

 private:
  bool raw_ = false;
  bool stable_ = false;
  bool armed_ = true;
  uint32_t changedAt_ = 0;
};

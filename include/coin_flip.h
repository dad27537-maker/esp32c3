#pragma once
#include <stdint.h>
#include <esp_system.h>

class CoinFlip {
 public:
  enum class Face : uint8_t { None, Heads, Tails };
  using RandomWord = uint32_t (*)();
  explicit CoinFlip(RandomWord randomWord = esp_random) : randomWord_(randomWord) {}

  void toss() {
    // Exactly half of all uint32_t values have bit 0 set: no mapping bias.
    // Each toss gets fresh hardware entropy; never force an alternating result.
    face = (randomWord_() & 1U) ? Face::Heads : Face::Tails;
    ++tosses;
  }

  Face face = Face::None;
  uint32_t tosses = 0;

 private:
  RandomWord randomWord_;
};

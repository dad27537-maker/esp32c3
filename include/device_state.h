#pragma once
#include <stdint.h>

enum class KeyId : uint8_t { Up, Center, Down, Left, Right };
enum class Page : uint8_t { Menu, Clipboard, Pomodoro, Hardware, Clock, Volume, Media, Coin, Settings };
enum class ConnectionMode : uint8_t { Wifi, Usb, Bluetooth };
inline const char* connectionName(ConnectionMode mode) {
  switch (mode) {
    case ConnectionMode::Usb: return "usb";
    case ConnectionMode::Bluetooth: return "ble";
    default: return "wifi";
  }
}
enum class Action : uint8_t { None, Copy, Paste, VolumeUp, VolumeDown, Mute,
                              PlayPause, NextTrack, PreviousTrack, SetWifi, SetUsb, SetBluetooth };
enum class Outcome : uint8_t { Confirmed, Unknown, Rejected, Offline, Expired, Denied, Failed };

inline const char* actionName(Action action) {
  switch (action) {
    case Action::Copy: return "copy";
    case Action::Paste: return "paste";
    case Action::VolumeUp: return "volume_up";
    case Action::VolumeDown: return "volume_down";
    case Action::Mute: return "volume_mute";
    case Action::PlayPause: return "media_play_pause";
    case Action::NextTrack: return "media_next";
    case Action::PreviousTrack: return "media_previous";
    case Action::SetWifi: return "set_wifi";
    case Action::SetUsb: return "set_usb";
    case Action::SetBluetooth: return "set_ble";
    default: return "none";
  }
}

struct PcSnapshot {
  bool online = false;
  bool metricsAvailable = false;
  bool muted = false;
  int16_t volume = -1;
  float cpu = -1;
  float ram = -1;
  float gpu = -1;
  float disk = -1;
  float cpuTemp = -1;
  float gpuTemp = -1;
  float vram = -1;
  uint32_t epoch = 0;
  uint16_t epochFractionMs = 0;
  uint32_t receivedAt = 0;
  uint32_t sampleAgeMs = 0;
};

struct Event {
  uint32_t sequence;
  uint32_t createdAt;
  Action action;
};
struct Result {
  uint32_t sequence;
  Action action;
  Outcome outcome;
};

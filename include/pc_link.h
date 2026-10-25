#pragma once
#include <Arduino.h>
#include "device_state.h"

class PcLink {
 public:
  struct Config {
    bool wifiConfigured;
    const char* ssid;
    const char* password;
    const char* host;
    uint16_t port;
    const char* token;
  };
  bool begin(const Config& config);
  ConnectionMode mode() const;
  bool select(ConnectionMode mode);
  bool online() const;
  bool connected() const;
  bool send(const Event& event);
  bool takeSnapshot(PcSnapshot& snapshot);
  bool takeResult(Result& result);
  void serialLine(const String& line);
  void pollSerialTx();

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

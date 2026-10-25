#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_system.h>
#include <sys/time.h>
#include "board_config.h"
#include "debounced_button.h"
#include "device_state.h"
#include "device_ui.h"
#include "pc_link.h"

#if __has_include("secrets.h")
#include "secrets.h"
constexpr bool configured = true;
#else
#include "secrets.example.h"
constexpr bool configured = false;
#endif

namespace {
TFT_eSPI display;
DeviceUI ui(display);
PcLink pcLink;
PcSnapshot latestSnapshot;
uint32_t sequence = 0;

struct Key {
  uint8_t pin;
  KeyId id;
  const char* name;
  DebouncedButton debounce;
  uint32_t repeatAt;
};
Key keys[] = {
    {Board::keyUp, KeyId::Up, "UP", {}, 0},
    {Board::keyCenter, KeyId::Center, "CENTER", {}, 0},
    {Board::keyDown, KeyId::Down, "DOWN", {}, 0},
    {Board::keyLeft, KeyId::Left, "LEFT", {}, 0},
    {Board::keyRight, KeyId::Right, "RIGHT", {}, 0},
};

void onKey(KeyId id, const char* name) {
  const uint32_t now = millis();
  const Action action = ui.key(id, now);
  Serial.printf("KEY %s page=%u selected=%u action=%s\n", name,
                static_cast<unsigned>(ui.page()), ui.selected(), actionName(action));
  if (action == Action::SetWifi || action == Action::SetUsb || action == Action::SetBluetooth) {
    const ConnectionMode mode = action == Action::SetWifi ? ConnectionMode::Wifi :
                                action == Action::SetUsb ? ConnectionMode::Usb : ConnectionMode::Bluetooth;
    if (pcLink.select(mode)) {
      latestSnapshot = PcSnapshot{};
      ui.snapshot(latestSnapshot);
      ui.visibleSequence = 0;
      ui.setConnection(mode, pcLink.connected());
      Serial.printf("LINK mode=%s saved=1\n", connectionName(mode));
    }
    return;
  }
  if (action == Action::None) return;
  if (!pcLink.online()) {
    ui.offline(action, configured, now);
    return;
  }
  const Event event{++sequence, now, action};
  if (!pcLink.send(event)) ui.busy(now);
  else ui.queued(action, event.sequence, now);
}

void syncClock(const PcSnapshot& snapshot) {
  if (!snapshot.online || snapshot.epoch < 1700000000) return;
  static uint32_t lastSync = 0;
  const time_t current = time(nullptr);
  if (current < 1700000000 || llabs(int64_t(current) - snapshot.epoch) > 2 ||
      (pcLink.mode() != ConnectionMode::Wifi && uint32_t(millis() - lastSync) > 1000) ||
      uint32_t(millis() - lastSync) > 60000) {
    struct timeval value{static_cast<time_t>(snapshot.epoch), static_cast<suseconds_t>(snapshot.epochFractionMs) * 1000};
    settimeofday(&value, nullptr);
    lastSync = millis();
  }
}

// Local visual diagnostics. Credentials are never exposed.
void serialCommand(const String& command) {
  if (command == "UI TEST SETTINGS") {
    DeviceUI isolated(display);
    isolated.setConnection(ConnectionMode::Wifi, false);
    isolated.debugPage(8, 100);
    const bool wifi = isolated.key(KeyId::Center, 100) == Action::SetWifi;
    isolated.key(KeyId::Down, 101);
    const bool usb = isolated.key(KeyId::Center, 102) == Action::SetUsb;
    isolated.key(KeyId::Down, 103);
    const bool ble = isolated.key(KeyId::Center, 104) == Action::SetBluetooth;
    isolated.key(KeyId::Down, 105);
    const bool wrap = isolated.selectedConnection() == 0;
    isolated.key(KeyId::Up, 106);
    const bool reverse = isolated.selectedConnection() == 2;
    isolated.key(KeyId::Left, 107);
    const bool back = isolated.page() == Page::Menu;
    Serial.printf("SETTINGS_TEST %s wifi=%u usb=%u ble=%u wrap=%u back=%u\n",
                  wifi && usb && ble && wrap && reverse && back ? "PASS" : "FAIL",
                  wifi, usb, ble, wrap && reverse, back);
  } else if (command == "UI TEST COIN") {
    // Deterministic edge cases exercise the same mapping as real tosses.
    // Production CoinFlip objects use esp_random; only this probe substitutes it.
    CoinFlip even([]() -> uint32_t { return 0; });
    CoinFlip odd([]() -> uint32_t { return UINT32_MAX; });
    even.toss(); odd.toss();
    const bool mapping = even.face == CoinFlip::Face::Tails && odd.face == CoinFlip::Face::Heads;
    odd.toss();
    const bool streak = odd.face == CoinFlip::Face::Heads && odd.tosses == 2;
    DeviceUI isolated(display);
    isolated.debugPage(7, 100);
    const bool enter = isolated.coin.face == CoinFlip::Face::None && isolated.coin.tosses == 0;
    const bool local = isolated.key(KeyId::Center, 100) == Action::None &&
                       isolated.coin.tosses == 1 && isolated.coin.face != CoinFlip::Face::None &&
                       isolated.coinSpinning();
    isolated.key(KeyId::Center, 200);
    const bool repeated = isolated.coin.tosses == 1;
    isolated.tick(100 + DeviceUI::coinAnimationMs - 1);
    const bool spinning = isolated.coinSpinning();
    isolated.tick(100 + DeviceUI::coinAnimationMs);
    const bool landed = !isolated.coinSpinning();
    isolated.key(KeyId::Up, 1400);
    isolated.key(KeyId::Down, 1400);
    isolated.key(KeyId::Right, 1400);
    const bool otherKeys = isolated.coin.tosses == 1;
    isolated.key(KeyId::Center, 2000);
    isolated.key(KeyId::Left, 2100);
    const bool back = isolated.page() == Page::Menu && isolated.coinSpinning();
    isolated.tick(3200);
    const bool background = !isolated.coinSpinning() && isolated.coin.tosses == 2;
    isolated.debugPage(7, 0xffffff00UL);
    isolated.key(KeyId::Center, 0xffffff00UL);
    isolated.tick(0x100);
    const bool wrapPending = isolated.coinSpinning();
    isolated.tick(0x500);
    const bool wrap = wrapPending && !isolated.coinSpinning();
    const bool animation = repeated && spinning && landed && background && wrap;
    CoinFlip samples;
    unsigned heads = 0;
    for (unsigned i = 0; i < 256; ++i) {
      samples.toss();
      if (samples.face == CoinFlip::Face::Heads) ++heads;
    }
    // Smoke check only: a finite sample is not a proof of randomness.
    const bool sampling = heads > 0 && heads < 256;
    Serial.printf("COIN_TEST %s mapping=%u streak=%u enter=%u local=%u other=%u back=%u animation=%u heads=%u tails=%u\n",
                  mapping && streak && enter && local && otherKeys && back && animation && sampling ? "PASS" : "FAIL",
                  mapping, streak, enter, local, otherKeys, back, animation, heads, 256 - heads);
  } else if (command == "UI TEST MEDIA") {
    DeviceUI isolated(display);
    isolated.debugPage(6, millis());
    const bool actions = isolated.key(KeyId::Center, 0) == Action::PlayPause &&
                         isolated.key(KeyId::Down, 0) == Action::NextTrack &&
                         isolated.key(KeyId::Up, 0) == Action::PreviousTrack &&
                         isolated.key(KeyId::Right, 0) == Action::None;
    const bool left = isolated.key(KeyId::Left, 0) == Action::None;
    const bool back = isolated.page() == Page::Menu;
    Serial.printf("MEDIA_TEST %s actions=%u back=%u\n",
                  actions && left && back ? "PASS" : "FAIL", actions, left && back);
  } else if (command == "UI TEST TIMER") {
    Pomodoro probe;
    probe.reset(100);
    probe.toggle(100);
    probe.tick(1500100);
    const bool breakWorks = probe.resting && probe.running && probe.completed == 1 && probe.secondsLeft() == 300;
    probe.tick(1800100);
    const bool nextFocus = !probe.resting && probe.running && probe.secondsLeft() == 1500;
    probe.toggle(1800100);
    probe.tick(1805100);
    const bool pauseWorks = !probe.running && probe.secondsLeft() == 1500;
    probe.reset(0xffffff00UL);
    probe.toggle(0xffffff00UL);
    probe.tick(0x100);
    const bool wrapWorks = probe.phaseRemainingMs == 1500000UL - 512;
    Serial.printf("TIMER_TEST %s break=%u focus=%u pause=%u wrap=%u\n",
                  breakWorks && nextFocus && pauseWorks && wrapWorks ? "PASS" : "FAIL",
                  breakWorks, nextFocus, pauseWorks, wrapWorks);
  } else if (command == "UI FRAME") ui.dumpFrame(Serial, millis());
  else if (command.startsWith("UI PAGE ")) {
    ui.debugPage(command.substring(8).toInt(), millis());
    ui.render(millis(), true);
    Serial.println("UI PAGE OK");
  } else if (command == "UI STATE") {
    Serial.printf("UI_STATE page=%u selected=%u pc=%u cpu=%.1f ram=%.1f gpu=%.1f volume=%d "
                  "pomo_seconds=%lu running=%u coin=%u tosses=%lu coin_anim=%u mode=%s conn_selected=%u heap=%u\n", static_cast<unsigned>(ui.page()),
                  ui.selected(), latestSnapshot.online, latestSnapshot.cpu, latestSnapshot.ram,
                  latestSnapshot.gpu, latestSnapshot.volume,
                  static_cast<unsigned long>(ui.pomodoro.secondsLeft()), ui.pomodoro.running,
                  static_cast<unsigned>(ui.coin.face), static_cast<unsigned long>(ui.coin.tosses),
                  ui.coinSpinning(),
                  connectionName(pcLink.mode()), ui.selectedConnection(),
                  ESP.getFreeHeap());
  } else if (command.startsWith("UI KEY ")) {
    const String name = command.substring(7);
    for (const auto& key : keys) {
      if (name == key.name) { onKey(key.id, key.name); break; }
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("[BOOT] Controller UI v6: WiFi / USB / BLE connections");
  Serial.printf("[TFT] MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%u SPI=0x%08lx\n",
                TFT_MOSI, TFT_SCLK, TFT_CS, TFT_DC, TFT_RST, Board::backlight,
                static_cast<unsigned long>(REG_SPI_BASE(SPI2_HOST)));
  pinMode(Board::backlight, OUTPUT);
  digitalWrite(Board::backlight, LOW);
  display.init();
  display.setRotation(Board::rotation);
  display.invertDisplay(Board::invertColors);
  display.fillScreen(TFT_BLACK);
  digitalWrite(Board::backlight, HIGH);
  if (!ui.begin()) {
    display.setTextColor(TFT_RED, TFT_BLACK);
    display.println("UI memory error");
    while (true) delay(1000);
  }
  for (auto& key : keys) {
    pinMode(key.pin, INPUT_PULLUP);
    key.debounce.begin(digitalRead(key.pin) == LOW, millis());
  }
  setenv("TZ", "CST-8", 1);
  tzset();
  ui.pomodoro.reset(millis());
  ui.render(millis(), true);
  const PcLink::Config config{configured && strlen(WIFI_SSID) > 0, WIFI_SSID, WIFI_PASSWORD,
                              PC_HOST, PC_PORT, API_TOKEN};
  if (!pcLink.begin(config)) {
    Serial.println("[ERROR] Connection manager initialization failed");
    while (true) delay(1000);
  }
  ui.setConnection(pcLink.mode(), pcLink.connected());
  Serial.printf("[LINK] Saved mode: %s; USB serial / BLE / WiFi supported\n", connectionName(pcLink.mode()));
  Serial.println("[RNG] Hardware RNG with initialized RF entropy");
  Serial.printf("[KEYS] UP=%u DOWN=%u CENTER=%u LEFT=%u RIGHT=%u\n",
                Board::keyUp, Board::keyDown, Board::keyCenter, Board::keyLeft, Board::keyRight);
  Serial.println("[BOOT] Menu ready: UP/DOWN select, CENTER enter, LEFT back");
}

void loop() {
  const uint32_t now = millis();
  ui.tick(now);
  PcSnapshot snapshot;
  if (pcLink.takeSnapshot(snapshot)) {
    latestSnapshot = snapshot;
    ui.snapshot(snapshot);
    syncClock(snapshot);
  }
  for (auto& key : keys) {
    if (key.debounce.update(digitalRead(key.pin) == LOW, now, Board::debounceMs)) {
      onKey(key.id, key.name);
      key.repeatAt = now + Board::repeatDelayMs;
    } else if (key.debounce.pressed() &&
               (ui.page() == Page::Menu || ui.page() == Page::Volume) &&
               (key.id == KeyId::Up || key.id == KeyId::Down) &&
               int32_t(now - key.repeatAt) >= 0) {
      onKey(key.id, key.name);
      key.repeatAt = now + Board::repeatIntervalMs;
    }
  }
  Result result;
  while (pcLink.takeResult(result)) {
    ui.result(result, now);
    Serial.printf("EVENT %lu action=%s result=%u\n", static_cast<unsigned long>(result.sequence),
                  actionName(result.action), static_cast<unsigned>(result.outcome));
  }
  ui.setConnection(pcLink.mode(), pcLink.connected());
  ui.render(now);
  static String command;
  static bool overflow = false;
  unsigned budget = 512;
  while (Serial.available() && budget--) {
    const char value = Serial.read();
    if (value == '\n') {
      if (!overflow) {
        command.trim();
        if (command.startsWith("{")) pcLink.serialLine(command);
        else serialCommand(command);
      }
      command = "";
      overflow = false;
    } else if (value != '\r' && !overflow) {
      if (command.length() < 2047) command += value;
      else { command = ""; overflow = true; }
    }
  }
  pcLink.pollSerialTx();
  delay(2);
}

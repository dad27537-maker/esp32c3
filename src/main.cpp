#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <atomic>
#include <esp_system.h>
#include <sys/time.h>
#include "board_config.h"
#include "debounced_button.h"
#include "device_state.h"
#include "device_ui.h"

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
std::atomic<bool> pcOnline{false};
QueueHandle_t events;
QueueHandle_t results;
QueueHandle_t snapshots;
PcSnapshot latestSnapshot;
char sessionId[17];
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

struct Reply { int code; String body; };

// Only the worker uses HTTP. A slow PC never blocks button scans or the timer.
Reply request(const char* path, const String* payload = nullptr) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(450);
  http.setTimeout(700);
  http.setReuse(false);
  const String url = String("http://") + PC_HOST + ":" + PC_PORT + path;
  if (!http.begin(client, url)) return {-1, ""};
  http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
  int code;
  if (payload) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(*payload);
  } else code = http.GET();
  String body = code > 0 ? http.getString() : String();
  http.end();
  return {code, body};
}

float metric(JsonVariantConst value) {
  if (value.isNull() || !value.is<float>()) return -1;
  const float result = value.as<float>();
  return isfinite(result) && result >= 0 ? result : -1;
}

PcSnapshot readSnapshot() {
  PcSnapshot snapshot;
  snapshot.receivedAt = millis();
  if (WiFi.status() != WL_CONNECTED) return snapshot;
  const Reply reply = request("/status");
  if (reply.code == 200) {
    StaticJsonDocument<1536> document;
    if (!deserializeJson(document, reply.body) && document["ok"] == true) {
      snapshot.online = true;
      snapshot.epoch = document["epoch"] | 0UL;
      const JsonObjectConst stats = document["stats"].as<JsonObjectConst>();
      snapshot.metricsAvailable = stats["available"] | false;
      snapshot.sampleAgeMs = stats["age_ms"] | 0UL;
      snapshot.cpu = metric(stats["cpu"]);
      snapshot.ram = metric(stats["ram"]);
      snapshot.gpu = metric(stats["gpu"]);
      snapshot.disk = metric(stats["disk"]);
      snapshot.cpuTemp = metric(stats["cpu_temp"]);
      snapshot.gpuTemp = metric(stats["gpu_temp"]);
      snapshot.vram = metric(stats["vram"]);
      const JsonObjectConst audio = document["audio"].as<JsonObjectConst>();
      if (audio["available"] == true && !audio["volume"].isNull()) {
        const int volume = audio["volume"].as<int>();
        snapshot.volume = volume >= 0 && volume <= 100 ? volume : -1;
        snapshot.muted = audio["muted"] | false;
      }
      snapshot.receivedAt = millis();
      return snapshot;
    }
  }
  // An older receiver can still handle copy/paste; telemetry remains unavailable.
  const Reply health = request("/health");
  snapshot.online = health.code == 200 && health.body == "{\"ok\":true}";
  snapshot.receivedAt = millis();
  return snapshot;
}

void networkTask(void*) {
  uint32_t lastPoll = millis() - Board::statusPollMs;
  bool refresh = true;
  bool networkClockStarted = false;
  for (;;) {
    if (!networkClockStarted && WiFi.status() == WL_CONNECTED) {
      configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org");
      networkClockStarted = true;
    }
    Event event;
    if (xQueueReceive(events, &event, pdMS_TO_TICKS(15)) == pdTRUE) {
      Result result{event.sequence, event.action, Outcome::Offline};
      if (uint32_t(millis() - event.createdAt) > Board::eventMaxAgeMs) {
        result.outcome = Outcome::Expired;
      } else if (WiFi.status() == WL_CONNECTED) {
        char payload[160];
        snprintf(payload, sizeof(payload), "{\"id\":\"%s-%lu\",\"action\":\"%s\"}",
                 sessionId, static_cast<unsigned long>(event.sequence), actionName(event.action));
        const String body(payload);
        const Reply reply = request("/event", &body);
        if (reply.code == 200 && reply.body == "{\"ok\":true}") result.outcome = Outcome::Confirmed;
        else if (reply.code == 403) result.outcome = Outcome::Denied;
        else if (reply.code == 400 || reply.code == 401 || reply.code == 409) result.outcome = Outcome::Rejected;
        else if (reply.code > 0) result.outcome = Outcome::Failed;
        else { result.outcome = Outcome::Unknown; pcOnline.store(false); }
        // Never retry an action: a response may be lost after a successful paste.
      }
      xQueueSend(results, &result, portMAX_DELAY);
      refresh = true;
    }
    if (refresh || uint32_t(millis() - lastPoll) >= Board::statusPollMs) {
      PcSnapshot snapshot = readSnapshot();
      pcOnline.store(snapshot.online);
      xQueueOverwrite(snapshots, &snapshot);
      lastPoll = millis();
      refresh = false;
    }
  }
}

void onKey(KeyId id, const char* name) {
  const uint32_t now = millis();
  const Action action = ui.key(id, now);
  Serial.printf("KEY %s page=%u selected=%u action=%s\n", name,
                static_cast<unsigned>(ui.page()), ui.selected(), actionName(action));
  if (action == Action::None) return;
  if (!configured || WiFi.status() != WL_CONNECTED || !pcOnline.load()) {
    ui.offline(action, configured, now);
    return;
  }
  Event event{++sequence, now, action};
  if (xQueueSend(events, &event, 0) != pdTRUE) ui.busy(now);
  else ui.queued(action, event.sequence, now);
}

void syncClock(const PcSnapshot& snapshot) {
  if (!snapshot.online || snapshot.epoch < 1700000000) return;
  static uint32_t lastSync = 0;
  const time_t current = time(nullptr);
  if (current < 1700000000 || llabs(int64_t(current) - snapshot.epoch) > 2 ||
      uint32_t(millis() - lastSync) > 60000) {
    struct timeval value{static_cast<time_t>(snapshot.epoch), 0};
    settimeofday(&value, nullptr);
    lastSync = millis();
  }
}

// Local visual diagnostics. Credentials are never exposed.
void serialCommand(const String& command) {
  if (command == "UI TEST MEDIA") {
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
                  "pomo_seconds=%lu running=%u heap=%u\n", static_cast<unsigned>(ui.page()),
                  ui.selected(), latestSnapshot.online, latestSnapshot.cpu, latestSnapshot.ram,
                  latestSnapshot.gpu, latestSnapshot.volume,
                  static_cast<unsigned long>(ui.pomodoro.secondsLeft()), ui.pomodoro.running,
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
  Serial.println("[BOOT] Six-function controller UI v3 (10px Chinese, media keys)");
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
  snprintf(sessionId, sizeof(sessionId), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  events = xQueueCreate(4, sizeof(Event));
  results = xQueueCreate(8, sizeof(Result));
  snapshots = xQueueCreate(1, sizeof(PcSnapshot));
  if (!events || !results || !snapshots) {
    Serial.println("[ERROR] Queue allocation failed");
    while (true) delay(1000);
  }
  ui.pomodoro.reset(millis());
  ui.render(millis(), true);
  if (configured) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    if (xTaskCreate(networkTask, "pc-link", 8192, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("[ERROR] Network task allocation failed");
      while (true) delay(1000);
    }
    Serial.printf("[PC] Endpoint http://%s:%u (token hidden)\n", PC_HOST, PC_PORT);
  } else Serial.println("[CONFIG] Run python tools/configure.py and rebuild/upload.");
  Serial.printf("[KEYS] UP=%u DOWN=%u CENTER=%u LEFT=%u RIGHT=%u\n",
                Board::keyUp, Board::keyDown, Board::keyCenter, Board::keyLeft, Board::keyRight);
  Serial.println("[BOOT] Menu ready: UP/DOWN select, CENTER enter, LEFT back");
}

void loop() {
  const uint32_t now = millis();
  ui.tick(now);
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
  PcSnapshot snapshot;
  if (xQueueReceive(snapshots, &snapshot, 0) == pdTRUE) {
    latestSnapshot = snapshot;
    ui.snapshot(snapshot);
    syncClock(snapshot);
  }
  Result result;
  while (xQueueReceive(results, &result, 0) == pdTRUE) {
    ui.result(result, now);
    Serial.printf("EVENT %lu action=%s result=%u\n", static_cast<unsigned long>(result.sequence),
                  actionName(result.action), static_cast<unsigned>(result.outcome));
  }
  ui.setWifi(WiFi.status() == WL_CONNECTED);
  ui.render(now);
  static String command;
  while (Serial.available()) {
    const char value = Serial.read();
    if (value == '\n') {
      command.trim();
      serialCommand(command);
      command = "";
    } else if (value != '\r' && command.length() < 64) command += value;
  }
  delay(2);
}

#include "pc_link.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include <algorithm>
#include <mbedtls/md.h>
#include <sys/time.h>
#include "board_config.h"

namespace {
constexpr const char* serviceUuid = "fb7b0001-2c92-4cc5-aaca-5c65d5e8a301";
constexpr const char* rxUuid = "fb7b0002-2c92-4cc5-aaca-5c65d5e8a301";
constexpr const char* txUuid = "fb7b0003-2c92-4cc5-aaca-5c65d5e8a301";
constexpr const char* deviceName = "ESP32C3-Macropad";
constexpr uint32_t staleMs = 5000;
struct WireMessage { ConnectionMode source; char body[2048]; };
struct WireOut { uint32_t generation; char body[512]; };
struct QueuedEvent { Event event; uint32_t generation; ConnectionMode mode; };
struct TaggedSnapshot { PcSnapshot value; uint32_t generation; };
struct TaggedResult { Result value; uint32_t generation; };

float metric(JsonVariantConst value) {
  if (value.isNull() || !value.is<float>()) return -1;
  const float result = value.as<float>();
  return isfinite(result) && result >= 0 ? result : -1;
}
PcSnapshot parseSnapshot(JsonObjectConst document) {
  PcSnapshot snapshot;
  snapshot.online = true;
  snapshot.receivedAt = millis();
  snapshot.epoch = document["epoch"] | 0UL;
  if (document["epoch_ms"].is<uint64_t>()) {
    const uint64_t epochMs = document["epoch_ms"];
    snapshot.epoch = epochMs / 1000;
    snapshot.epochFractionMs = epochMs % 1000;
  }
  const JsonObjectConst stats = document["stats"];
  snapshot.metricsAvailable = stats["available"] | false;
  snapshot.sampleAgeMs = stats["age_ms"] | 0UL;
  snapshot.cpu = metric(stats["cpu"]);
  snapshot.ram = metric(stats["ram"]);
  snapshot.gpu = metric(stats["gpu"]);
  snapshot.disk = metric(stats["disk"]);
  snapshot.cpuTemp = metric(stats["cpu_temp"]);
  snapshot.gpuTemp = metric(stats["gpu_temp"]);
  snapshot.vram = metric(stats["vram"]);
  const JsonObjectConst audio = document["audio"];
  if (audio["available"] == true && audio["volume"].is<int>()) {
    const int volume = audio["volume"];
    snapshot.volume = volume >= 0 && volume <= 100 ? volume : -1;
    snapshot.muted = audio["muted"] | false;
  }
  return snapshot;
}
Outcome outcome(int code) {
  if (code == 200) return Outcome::Confirmed;
  if (code == 403) return Outcome::Denied;
  if (code == 408) return Outcome::Expired;
  if (code == 400 || code == 401 || code == 409) return Outcome::Rejected;
  return code > 0 ? Outcome::Failed : Outcome::Unknown;
}
}  // namespace

struct PcLink::Impl {
  Config config;
  Preferences preferences;
  // Mode and generation change atomically, so events cannot observe a mixed pair.
  std::atomic<uint32_t> state{4};
  ConnectionMode mode() const { return static_cast<ConnectionMode>(state.load() & 3U); }
  uint32_t generation() const { return state.load() >> 2; }
  std::atomic<uint32_t> onlineGeneration{0};
  std::atomic<bool> pcOnline{false};
  std::atomic<uint32_t> lastSeen{0};
  std::atomic<bool> bleConnected{false}, bleEncrypted{false};
  std::atomic<uint16_t> bleHandle{0xffff};
  NimBLEServer* server = nullptr;
  NimBLECharacteristic* tx = nullptr;
  QueueHandle_t events = nullptr, snapshots = nullptr, results = nullptr;
  QueueHandle_t incoming = nullptr, usbOutgoing = nullptr;
  char session[17];
  WireMessage bleInput{};
  size_t bleInputLength = 0;
  bool bleDiscard = false;

  bool current(uint32_t value, ConnectionMode mode) const {
    return state.load() == ((value << 2) | static_cast<uint32_t>(mode));
  }
  String proof(const char* role, uint32_t value, ConnectionMode mode) const {
    char message[128];
    snprintf(message, sizeof(message), "macropad-v1|%s|%s|%lu|%s", role, session,
             static_cast<unsigned long>(value), connectionName(mode));
    uint8_t digest[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    reinterpret_cast<const uint8_t*>(config.token), strlen(config.token),
                    reinterpret_cast<const uint8_t*>(message), strlen(message), digest);
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return String(hex);
  }
  bool authorized(JsonObjectConst doc, uint32_t value, ConnectionMode mode) const {
    if (doc["v"] != 1 || doc["link"] != value ||
        strcmp(doc["mode"] | "", connectionName(mode)) || strcmp(doc["session"] | "", session)) return false;
    const char* supplied = doc["proof"] | "";
    if (strlen(supplied) != 64) return false;
    const String expected = proof("pc", value, mode);
    uint8_t difference = 0;
    for (unsigned i = 0; i < 64; ++i) difference |= uint8_t(supplied[i] ^ expected[i]);
    return difference == 0;
  }
  void envelope(JsonDocument& doc, const char* type, uint32_t value, ConnectionMode mode) {
    doc["v"] = 1;
    doc["type"] = type;
    doc["session"] = session;
    doc["link"] = value;
    doc["mode"] = connectionName(mode);
  }
  void publish(const PcSnapshot& snapshot, uint32_t value, ConnectionMode mode) {
    if (!current(value, mode)) return;
    pcOnline.store(snapshot.online);
    onlineGeneration.store(value);
    if (snapshot.online) lastSeen.store(millis());
    const TaggedSnapshot item{snapshot, value};
    xQueueOverwrite(snapshots, &item);
  }
  void result(const QueuedEvent& queued, Outcome status) {
    if (!current(queued.generation, queued.mode)) return;
    const TaggedResult item{{queued.event.sequence, queued.event.action, status}, queued.generation};
    xQueueSend(results, &item, 0);
  }
  struct Reply { int code; String body; };
  Reply request(const char* path, const String* payload = nullptr) {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(450);
    http.setTimeout(700);
    http.setReuse(false);
    const String url = String("http://") + config.host + ":" + config.port + path;
    if (!http.begin(client, url)) return {-1, ""};
    http.addHeader("Authorization", String("Bearer ") + config.token);
    int code;
    if (payload) {
      http.addHeader("Content-Type", "application/json");
      code = http.POST(*payload);
    } else code = http.GET();
    String body = code > 0 ? http.getString() : String();
    http.end();
    return {code, body};
  }
  PcSnapshot wifiSnapshot() {
    PcSnapshot snapshot;
    snapshot.receivedAt = millis();
    if (!config.wifiConfigured || WiFi.status() != WL_CONNECTED) return snapshot;
    const Reply reply = request("/status");
    if (reply.code == 200) {
      StaticJsonDocument<2048> doc;
      if (!deserializeJson(doc, reply.body) && doc["ok"] == true)
        return parseSnapshot(doc.as<JsonObjectConst>());
    }
    const Reply health = request("/health");
    snapshot.online = health.code == 200 && health.body == "{\"ok\":true}";
    return snapshot;
  }
  void bleBytes(const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      if (data[i] == '\n') {
        if (bleInputLength && !bleDiscard && this->mode() == ConnectionMode::Bluetooth) {
          bleInput.body[bleInputLength] = 0;
          bleInput.source = ConnectionMode::Bluetooth;
          xQueueSend(incoming, &bleInput, 0);
        }
        bleInputLength = 0;
        bleDiscard = false;
      } else if (!bleDiscard) {
        if (bleInputLength < sizeof(bleInput.body) - 1) bleInput.body[bleInputLength++] = data[i];
        else { bleInputLength = 0; bleDiscard = true; }
      }
    }
  }
  struct ServerCallbacks : NimBLEServerCallbacks {
    Impl* owner;
    explicit ServerCallbacks(Impl* value) : owner(value) {}
    void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
      owner->bleHandle.store(info.getConnHandle());
      owner->bleConnected.store(true);
      owner->bleEncrypted.store(info.isEncrypted());
      server->updateConnParams(info.getConnHandle(), 12, 24, 0, 200);
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
      owner->bleEncrypted.store(info.isEncrypted());
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
      owner->bleConnected.store(false);
      owner->bleEncrypted.store(false);
      owner->bleInputLength = 0;
      owner->bleDiscard = false;
      if (owner->mode() == ConnectionMode::Bluetooth) NimBLEDevice::startAdvertising();
    }
  };
  struct ReceiveCallbacks : NimBLECharacteristicCallbacks {
    Impl* owner;
    explicit ReceiveCallbacks(Impl* value) : owner(value) {}
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
      if (!info.isEncrypted() || owner->mode() != ConnectionMode::Bluetooth) return;
      const auto value = characteristic->getValue();
      owner->bleBytes(value.data(), value.size());
    }
  };
  bool beginBle() {
    if (!NimBLEDevice::init(deviceName)) return false;
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setSecurityAuth(true, false, true);  // Bonded encrypted BLE; app proof checks the peer.
    server = NimBLEDevice::createServer();
    if (!server) return false;
    server->setCallbacks(new ServerCallbacks(this));
    server->advertiseOnDisconnect(false);
    NimBLEService* service = server->createService(serviceUuid);
    tx = service->createCharacteristic(txUuid,
         NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY, 512);
    auto* rx = service->createCharacteristic(rxUuid,
         NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC, 512);
    rx->setCallbacks(new ReceiveCallbacks(this));
    service->start();
    auto* advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(serviceUuid);
    advertising->enableScanResponse(true);
    advertising->setName(deviceName);
    return true;
  }
  void changeMode(ConnectionMode mode) {
    NimBLEDevice::stopAdvertising();
    if (mode != ConnectionMode::Bluetooth && bleConnected.load()) server->disconnect(bleHandle.load());
    // Keep WiFi/RF initialized for the hardware RNG in every mode. USB and BLE
    // do not need WiFi credentials or a router; desktop traffic uses one link.
    WiFi.disconnect(false, false);
    if (mode == ConnectionMode::Wifi && config.wifiConfigured) WiFi.begin(config.ssid, config.password);
    if (mode == ConnectionMode::Bluetooth) NimBLEDevice::startAdvertising();
  }
  bool transmit(JsonDocument& doc, uint32_t value, ConnectionMode mode) {
    if (!current(value, mode)) return false;
    String body;
    serializeJson(doc, body);
    if (mode == ConnectionMode::Usb) {
      if (body.length() >= sizeof(WireOut::body)) return false;
      WireOut item{};
      item.generation = value;
      body.toCharArray(item.body, sizeof(item.body));
      return xQueueSend(usbOutgoing, &item, 0) == pdTRUE;
    }
    if (!bleConnected.load() || !bleEncrypted.load()) return false;
    const uint16_t handle = bleHandle.load();
    const uint16_t mtu = server->getPeerMTU(handle);
    const size_t chunk = mtu > 3 ? std::min<size_t>(180, mtu - 3) : 20;
    body = String("\n") + body + "\n";
    for (size_t start = 0; start < body.length(); start += chunk) {
      if (!current(value, mode) || !bleEncrypted.load()) return false;
      const size_t count = std::min(chunk, body.length() - start);
      if (!tx->notify(reinterpret_cast<const uint8_t*>(body.c_str() + start), count, handle)) return false;
      vTaskDelay(pdMS_TO_TICKS(3));
    }
    return true;
  }
  void run() {
    uint32_t activeGeneration = 0;
    uint32_t lastPoll = 0, lastHello = 0;
    bool pending = false, ntpStarted = false;
    QueuedEvent waiting{};
    uint32_t sentAt = 0;
    for (;;) {
      const uint32_t state = this->state.load();
      const auto mode = static_cast<ConnectionMode>(state & 3U);
      const auto value = state >> 2;
      if (value != activeGeneration) {
        pending = false;
        changeMode(mode);
        activeGeneration = value;
        lastPoll = millis() - Board::statusPollMs;
        lastHello = millis() - 1000;
        publish(PcSnapshot{}, value, mode);
      }
      if (mode == ConnectionMode::Wifi && !ntpStarted && WiFi.status() == WL_CONNECTED) {
        configTzTime("CST-8", "ntp.aliyun.com", "pool.ntp.org");
        ntpStarted = true;
      }
      WireMessage message;
      while (xQueueReceive(incoming, &message, 0) == pdTRUE) {
        if (mode == ConnectionMode::Wifi || message.source != mode) continue;
        StaticJsonDocument<2560> doc;
        if (deserializeJson(doc, message.body) || !authorized(doc.as<JsonObjectConst>(), value, mode)) continue;
        const char* type = doc["type"] | "";
        if (!strcmp(type, "status")) publish(parseSnapshot(doc.as<JsonObjectConst>()), value, mode);
        if (pending && !strcmp(type, "ack")) {
          char expected[40];
          snprintf(expected, sizeof(expected), "%s-%lu", session,
                   static_cast<unsigned long>(waiting.event.sequence));
          if (!strcmp(doc["id"] | "", expected)) {
            result(waiting, outcome(doc["code"] | 0));
            pending = false;
          }
        }
      }
      const uint32_t now = millis();
      if (pending && uint32_t(now - sentAt) >= 1200) {
        result(waiting, Outcome::Unknown);
        pending = false;
      }
      QueuedEvent queued;
      if (!pending && xQueueReceive(events, &queued, 0) == pdTRUE) {
        if (!current(queued.generation, queued.mode)) continue;
        if (uint32_t(now - queued.event.createdAt) > Board::eventMaxAgeMs) {
          result(queued, Outcome::Expired);
        } else if (!pcOnline.load() || uint32_t(now - lastSeen.load()) > staleMs) {
          result(queued, Outcome::Offline);
        } else {
          StaticJsonDocument<512> doc;
          char id[40];
          snprintf(id, sizeof(id), "%s-%lu", session, static_cast<unsigned long>(queued.event.sequence));
          doc["id"] = id;
          doc["action"] = actionName(queued.event.action);
          if (mode == ConnectionMode::Wifi) {
            String body;
            serializeJson(doc, body);
            const Reply reply = request("/event", &body);
            result(queued, outcome(reply.code == 200 && reply.body != "{\"ok\":true}" ? -1 : reply.code));
            lastPoll = millis() - Board::statusPollMs;
          } else {
            envelope(doc, "event", value, mode);
            struct timeval time;
            gettimeofday(&time, nullptr);
            const int64_t epochMs = int64_t(time.tv_sec) * 1000 + time.tv_usec / 1000;
            doc["created_ms"] = epochMs - uint32_t(millis() - queued.event.createdAt);
            if (transmit(doc, value, mode)) {
              waiting = queued;
              pending = true;
              sentAt = millis();
            } else result(queued, Outcome::Unknown);
          }
        }
      }
      if (mode == ConnectionMode::Wifi && uint32_t(millis() - lastPoll) >= Board::statusPollMs) {
        publish(wifiSnapshot(), value, mode);
        lastPoll = millis();
      }
      if (mode != ConnectionMode::Wifi) {
        if (uint32_t(millis() - lastHello) >= 1000) {
          StaticJsonDocument<384> doc;
          envelope(doc, "hello", value, mode);
          doc["proof"] = proof("device", value, mode);
          transmit(doc, value, mode);
          lastHello = millis();
        }
        if (pcOnline.load() && uint32_t(millis() - lastSeen.load()) > staleMs)
          publish(PcSnapshot{}, value, mode);
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
};

bool PcLink::begin(const Config& config) {
  impl_ = new Impl;
  impl_->config = config;
  impl_->events = xQueueCreate(4, sizeof(QueuedEvent));
  impl_->snapshots = xQueueCreate(1, sizeof(TaggedSnapshot));
  impl_->results = xQueueCreate(8, sizeof(TaggedResult));
  impl_->incoming = xQueueCreate(4, sizeof(WireMessage));
  impl_->usbOutgoing = xQueueCreate(4, sizeof(WireOut));
  if (!impl_->events || !impl_->snapshots || !impl_->results || !impl_->incoming || !impl_->usbOutgoing) return false;
  if (impl_->preferences.begin("macropad", false)) {
    const uint8_t saved = impl_->preferences.getUChar("link", 0);
    if (saved <= 2) impl_->state.store((1U << 2) | saved);
  }
  snprintf(impl_->session, sizeof(impl_->session), "%08lx%08lx",
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) return false;
  WiFi.setAutoReconnect(true);
  if (!impl_->beginBle()) return false;
  return xTaskCreate([](void* context) { static_cast<Impl*>(context)->run(); }, "pc-link", 12288,
                     impl_, 1, nullptr) == pdPASS;
}
ConnectionMode PcLink::mode() const { return impl_->mode(); }
bool PcLink::select(ConnectionMode mode) {
  if (static_cast<uint8_t>(mode) > 2) return false;
  if (mode == this->mode()) return true;
  impl_->pcOnline.store(false);
  const uint32_t nextGeneration = impl_->generation() + 1;
  impl_->state.store((nextGeneration << 2) | static_cast<uint32_t>(mode));
  impl_->preferences.putUChar("link", static_cast<uint8_t>(mode));
  return true;
}
bool PcLink::online() const {
  return impl_->onlineGeneration.load() == impl_->generation() && impl_->pcOnline.load() &&
         uint32_t(millis() - impl_->lastSeen.load()) < staleMs;
}
bool PcLink::connected() const {
  switch (mode()) {
    case ConnectionMode::Wifi: return WiFi.status() == WL_CONNECTED;
    case ConnectionMode::Bluetooth: return impl_->bleConnected.load();
    default: return true;  // CH343 has no physical cable-status input on this board.
  }
}
bool PcLink::send(const Event& event) {
  const uint32_t state = impl_->state.load();
  const QueuedEvent item{event, state >> 2, static_cast<ConnectionMode>(state & 3U)};
  return xQueueSend(impl_->events, &item, 0) == pdTRUE;
}
bool PcLink::takeSnapshot(PcSnapshot& snapshot) {
  TaggedSnapshot item;
  if (xQueueReceive(impl_->snapshots, &item, 0) != pdTRUE || item.generation != impl_->generation()) return false;
  snapshot = item.value;
  return true;
}
bool PcLink::takeResult(Result& result) {
  TaggedResult item;
  while (xQueueReceive(impl_->results, &item, 0) == pdTRUE) {
    if (item.generation == impl_->generation()) { result = item.value; return true; }
  }
  return false;
}
void PcLink::serialLine(const String& line) {
  if (mode() != ConnectionMode::Usb || line.length() >= sizeof(WireMessage::body)) return;
  WireMessage item{};
  item.source = ConnectionMode::Usb;
  line.toCharArray(item.body, sizeof(item.body));
  xQueueSend(impl_->incoming, &item, 0);
}
void PcLink::pollSerialTx() {
  WireOut item;
  // Serial output is owned by the Arduino loop, preventing log/frame interleaving.
  if (xQueueReceive(impl_->usbOutgoing, &item, 0) == pdTRUE &&
      impl_->current(item.generation, ConnectionMode::Usb)) {
    Serial.print('\n');
    Serial.println(item.body);
  }
}

#pragma once
#include <TFT_eSPI.h>
#include <math.h>
#include <time.h>
#include "device_state.h"
#include "pomodoro.h"
#include "ui_font.h"

namespace UiColor {
constexpr uint16_t rgb(uint32_t c) {
  return uint16_t(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}
constexpr uint16_t background = rgb(0x080d17);
constexpr uint16_t card = rgb(0x142034);
constexpr uint16_t line = rgb(0x26364d);
constexpr uint16_t text = rgb(0xf2f6ff);
constexpr uint16_t muted = rgb(0x8294ad);
constexpr uint16_t green = rgb(0x39dfb3);
constexpr uint16_t yellow = rgb(0xf6c661);
constexpr uint16_t red = rgb(0xff6b7a);
constexpr uint16_t accents[] = {rgb(0x35ddc5), rgb(0xff8968), rgb(0xb69aff),
                               rgb(0x78b6ff), rgb(0xf6cd68), rgb(0xff83bb)};
}

// Original 160x80 adaptation of a multifunction controller interface: cards,
// vector icons, progress rings, and a subset of legible Chinese glyphs.
class DeviceUI {
 public:
  explicit DeviceUI(TFT_eSPI& display) : display_(display), canvas_(&display) {}

  bool begin() {
    canvas_.setColorDepth(16);
    if (!canvas_.createSprite(160, 80)) return false;
    canvas_.setTextDatum(TL_DATUM);
    return true;
  }

  Page page() const { return page_; }
  uint8_t selected() const { return selected_; }
  uint32_t visibleSequence = 0;
  Pomodoro pomodoro;

  void snapshot(const PcSnapshot& value) { pc_ = value; dirty_ = true; }
  void setWifi(bool connected) {
    if (wifi_ != connected) { wifi_ = connected; dirty_ = true; }
  }
  void debugPage(uint8_t value, uint32_t now) {
    if (value > functionCount) return;
    page_ = static_cast<Page>(value);
    if (value) selected_ = value - 1;
    animating_ = false;
    noticeUntil_ = 0;
    visibleSequence = 0;
    dirty_ = true;
    pomodoro.tick(now);
  }

  Action key(KeyId key, uint32_t now) {
    dirty_ = true;
    if (key == KeyId::Left) {
      page_ = Page::Menu;
      visibleSequence = 0;
      noticeUntil_ = 0;
      animating_ = false;
      return Action::None;
    }
    if (page_ == Page::Menu) {
      if (key == KeyId::Up || key == KeyId::Down) {
        previous_ = selected_;
        direction_ = key == KeyId::Down ? 1 : -1;
        selected_ = uint8_t((int(selected_) + direction_ + functionCount) % functionCount);
        animationStart_ = now;
        animating_ = true;
      } else if (key == KeyId::Center) {
        page_ = static_cast<Page>(selected_ + 1);
        noticeUntil_ = 0;
        animating_ = false;
      }
    } else if (page_ == Page::Clipboard) {
      if (key == KeyId::Up) return Action::Copy;
      if (key == KeyId::Center) return Action::Paste;
    } else if (page_ == Page::Pomodoro) {
      if (key == KeyId::Center) pomodoro.toggle(now);
      if (key == KeyId::Right) pomodoro.reset(now);
      if (key == KeyId::Up) pomodoro.adjust(5, now);
      if (key == KeyId::Down) pomodoro.adjust(-5, now);
    } else if (page_ == Page::Hardware) {
      if (key == KeyId::Center || key == KeyId::Down) hardwarePage_ = (hardwarePage_ + 1) % 3;
      if (key == KeyId::Up) hardwarePage_ = (hardwarePage_ + 2) % 3;
    } else if (page_ == Page::Clock) {
      if (key == KeyId::Center) twelveHour_ = !twelveHour_;
    } else if (page_ == Page::Volume) {
      if (key == KeyId::Up) return Action::VolumeUp;
      if (key == KeyId::Down) return Action::VolumeDown;
      if (key == KeyId::Center) return Action::Mute;
    } else if (page_ == Page::Media) {
      if (key == KeyId::Center) return Action::PlayPause;
      if (key == KeyId::Down) return Action::NextTrack;
      if (key == KeyId::Up) return Action::PreviousTrack;
    }
    return Action::None;
  }

  void queued(Action action, uint32_t sequence, uint32_t now) {
    visibleSequence = sequence;
    lastAction_ = action;
    notice("正在发送", UiColor::yellow, now);
  }
  void busy(uint32_t now) { visibleSequence = 0; notice("队列忙", UiColor::yellow, now); }
  void result(const Result& result, uint32_t now) {
    if (result.sequence != visibleSequence) return;
    lastAction_ = result.action;
    switch (result.outcome) {
      case Outcome::Confirmed: notice("已发送", UiColor::green, now); break;
      case Outcome::Offline: notice("电脑离线", UiColor::red, now); break;
      case Outcome::Rejected: notice("认证错误", UiColor::red, now); break;
      case Outcome::Denied: notice("权限受限", UiColor::red, now); break;
      case Outcome::Expired: notice("过期取消", UiColor::yellow, now); break;
      case Outcome::Unknown: notice("结果未知", UiColor::yellow, now); break;
      case Outcome::Failed: notice("发送失败", UiColor::red, now); break;
    }
  }
  void offline(Action action, bool configured, uint32_t now) {
    visibleSequence = 0;
    lastAction_ = action;
    notice(configured ? "电脑离线" : "未配置网络", UiColor::red, now);
  }

  void tick(uint32_t now) { pomodoro.tick(now); }

  void render(uint32_t now, bool force = false) {
    bool activeNotice = noticeUntil_ && int32_t(noticeUntil_ - now) > 0;
    if (activeNotice != previousNotice_) { dirty_ = true; previousNotice_ = activeNotice; }
    if (animating_ && uint32_t(now - animationStart_) >= 160) {
      animating_ = false;
      dirty_ = true;
    }
    const bool phaseFlash = page_ == Page::Pomodoro && pomodoro.completedAt &&
                            uint32_t(now - pomodoro.completedAt) < 2500;
    if (!force && !dirty_ && !animating_ && !phaseFlash && lastSecond_ == now / 1000) return;
    if (!force && uint32_t(now - lastDraw_) < 35) return;
    lastDraw_ = now;
    lastSecond_ = now / 1000;
    dirty_ = false;
    canvas_.fillSprite(UiColor::background);
    switch (page_) {
      case Page::Menu: drawMenu(now); break;
      case Page::Clipboard: drawClipboard(now); break;
      case Page::Pomodoro: drawPomodoro(now); break;
      case Page::Hardware: drawHardware(now); break;
      case Page::Clock: drawClock(); break;
      case Page::Volume: drawVolume(now); break;
      case Page::Media: drawMedia(now); break;
    }
    if (phaseFlash && (now / 200) % 2 == 0) canvas_.drawRoundRect(1, 1, 158, 78, 6, UiColor::accents[1]);
    canvas_.pushSprite(0, 0);
  }

  void dumpFrame(Stream& serial, uint32_t now) {
    render(now, true);
    serial.printf("FRAME 160 80 25600\n");
    uint8_t row[320];
    for (int y = 0; y < 80; ++y) {
      for (int x = 0; x < 160; ++x) {
        const uint16_t color = canvas_.readPixel(x, y);
        row[x * 2] = color >> 8;
        row[x * 2 + 1] = color & 0xff;
      }
      serial.write(row, sizeof(row));
    }
    serial.print("\nEND_FRAME\n");
  }

 private:
  static constexpr uint8_t functionCount = 6;
  TFT_eSPI& display_;
  TFT_eSprite canvas_;
  Page page_ = Page::Menu;
  PcSnapshot pc_;
  uint8_t selected_ = 0;
  uint8_t previous_ = 0;
  uint8_t hardwarePage_ = 0;
  int8_t direction_ = 1;
  bool wifi_ = false;
  bool twelveHour_ = false;
  bool dirty_ = true;
  bool animating_ = false;
  bool previousNotice_ = false;
  uint32_t animationStart_ = 0;
  uint32_t lastDraw_ = 0;
  uint32_t lastSecond_ = 0;
  uint32_t noticeUntil_ = 0;
  const char* noticeText_ = "";
  uint16_t noticeColor_ = UiColor::muted;
  Action lastAction_ = Action::None;

  static uint32_t nextCodepoint(const char*& value) {
    const uint8_t first = uint8_t(*value++);
    if (first < 0x80) return first;
    if ((first & 0xe0) == 0xc0) {
      const uint8_t second = uint8_t(*value++);
      return ((first & 0x1f) << 6) | (second & 0x3f);
    }
    if ((first & 0xf0) == 0xe0) {
      const uint8_t second = uint8_t(*value++), third = uint8_t(*value++);
      return ((first & 0x0f) << 12) | ((second & 0x3f) << 6) | (third & 0x3f);
    }
    return '?';
  }
  static int textWidth(const char* value) {
    int width = 0;
    while (*value) width += nextCodepoint(value) < 128 ? 6 : uiGlyphWidth;
    return width;
  }
  void text(int x, int y, const char* value, uint16_t color) {
    canvas_.setTextFont(1);
    canvas_.setTextSize(1);
    canvas_.setTextColor(color);
    while (*value) {
      const uint32_t codepoint = nextCodepoint(value);
      if (codepoint < 128) {
        canvas_.setCursor(x, y + (uiGlyphHeight - 8) / 2);
        canvas_.write(uint8_t(codepoint));
        x += 6;
      } else {
        unsigned low = 0, high = uiGlyphCount;
        while (low < high) {
          const unsigned middle = (low + high) / 2;
          if (pgm_read_dword(&uiGlyphs[middle].codepoint) < codepoint) low = middle + 1;
          else high = middle;
        }
        if (low < uiGlyphCount && pgm_read_dword(&uiGlyphs[low].codepoint) == codepoint) {
          for (int row = 0; row < uiGlyphHeight; ++row) {
            for (int column = 0; column < uiGlyphWidth; ++column) {
              const uint8_t bits = pgm_read_byte(&uiGlyphs[low].rows[row * 2 + column / 8]);
              if (bits & (0x80 >> (column % 8))) canvas_.drawPixel(x + column, y + row, color);
            }
          }
        } else {
          canvas_.drawRect(x + 1, y + 1, uiGlyphWidth - 2, uiGlyphHeight - 2, color);
        }
        x += uiGlyphWidth;
      }
    }
  }
  void centered(int center, int y, const char* value, uint16_t color) {
    text(center - textWidth(value) / 2, y, value, color);
  }
  void big(int center, int y, const char* value, uint16_t color, uint8_t size = 2) {
    canvas_.setTextColor(color);
    canvas_.setTextFont(2);
    canvas_.setTextSize(size);
    canvas_.drawCentreString(value, center, y, 2);
    canvas_.setTextFont(1);
    canvas_.setTextSize(1);
  }
  void header(const char* title, uint16_t accent) {
    text(5, 1, title, accent);
    canvas_.drawFastHLine(5, 15, 150, UiColor::line);
    canvas_.fillCircle(151, 7, 3, pc_.online ? UiColor::green : wifi_ ? UiColor::yellow : UiColor::red);
  }
  void footer(const char* hint) { centered(80, 67, hint, UiColor::muted); }
  void notice(const char* value, uint16_t color, uint32_t now) {
    noticeText_ = value;
    noticeColor_ = color;
    noticeUntil_ = now + 1800;
    dirty_ = true;
  }
  bool showingNotice(uint32_t now) const { return noticeUntil_ && int32_t(noticeUntil_ - now) > 0; }
  void actionFooter(uint32_t now, const char* hint) {
    if (!showingNotice(now)) { footer(hint); return; }
    text(4, 67, "←返回", UiColor::muted);
    centered(108, 67, noticeText_, noticeColor_);
  }
  void arc(int x, int y, int radius, float begin, float end, uint16_t color) {
    int previousX = x + lroundf(cosf(begin * 0.0174532925f) * radius);
    int previousY = y + lroundf(sinf(begin * 0.0174532925f) * radius);
    for (float angle = begin + 3; angle <= end + 2; angle += 3) {
      const float limited = angle > end ? end : angle;
      const int nextX = x + lroundf(cosf(limited * 0.0174532925f) * radius);
      const int nextY = y + lroundf(sinf(limited * 0.0174532925f) * radius);
      canvas_.drawLine(previousX, previousY, nextX, nextY, color);
      previousX = nextX; previousY = nextY;
    }
  }
  void icon(uint8_t index, int x, int y, uint16_t color) {
    switch (index) {
      case 0:
        canvas_.drawRoundRect(x + 3, y + 7, 18, 23, 3, color);
        canvas_.fillRoundRect(x + 9, y + 2, 19, 23, 3, color);
        canvas_.fillRoundRect(x + 11, y + 4, 15, 19, 2, UiColor::card);
        canvas_.drawFastHLine(x + 14, y + 10, 9, color);
        canvas_.drawFastHLine(x + 14, y + 15, 7, color);
        break;
      case 1:
        canvas_.drawCircle(x + 16, y + 19, 12, color);
        canvas_.drawCircle(x + 16, y + 19, 11, color);
        canvas_.drawLine(x + 16, y + 6, x + 16, y + 1, color);
        canvas_.drawLine(x + 16, y + 4, x + 24, y + 1, color);
        canvas_.drawLine(x + 16, y + 4, x + 9, y + 1, color);
        canvas_.drawLine(x + 16, y + 18, x + 16, y + 11, color);
        canvas_.drawLine(x + 16, y + 18, x + 21, y + 21, color);
        break;
      case 2:
        canvas_.drawRoundRect(x + 7, y + 7, 19, 19, 3, color);
        canvas_.drawRect(x + 12, y + 12, 9, 9, color);
        for (int i = 0; i < 4; ++i) {
          canvas_.drawFastVLine(x + 10 + i * 4, y + 2, 4, color);
          canvas_.drawFastVLine(x + 10 + i * 4, y + 27, 4, color);
          canvas_.drawFastHLine(x + 2, y + 10 + i * 4, 4, color);
          canvas_.drawFastHLine(x + 27, y + 10 + i * 4, 4, color);
        }
        break;
      case 3:
        canvas_.drawCircle(x + 16, y + 16, 14, color);
        canvas_.drawCircle(x + 16, y + 16, 13, color);
        canvas_.drawLine(x + 16, y + 16, x + 16, y + 7, color);
        canvas_.drawLine(x + 16, y + 16, x + 23, y + 20, color);
        canvas_.fillCircle(x + 16, y + 16, 2, color);
        break;
      case 4:
        canvas_.fillRect(x + 2, y + 12, 7, 10, color);
        canvas_.fillTriangle(x + 8, y + 12, x + 17, y + 5, x + 17, y + 29, color);
        arc(x + 16, y + 17, 8, -60, 60, color);
        arc(x + 16, y + 17, 13, -55, 55, color);
        break;
      case 5:
        canvas_.drawCircle(x + 16, y + 16, 14, color);
        canvas_.fillTriangle(x + 12, y + 8, x + 12, y + 24, x + 23, y + 16, color);
        break;
    }
  }
  static const char* title(uint8_t index) {
    switch (index) {
      case 0: return "复制粘贴";
      case 1: return "番茄时钟";
      case 2: return "硬件监控";
      case 3: return "时钟";
      case 4: return "电脑音量";
      default: return "媒体控制";
    }
  }
  void hero(uint8_t index, int y) {
    static const char* captions[] = {"COPY / PASTE", "FOCUS & BREAK", "CPU  RAM  GPU",
                                     "TIME & DATE", "SYSTEM AUDIO", "PLAY / PAUSE"};
    canvas_.fillRoundRect(4, y, 152, 45, 7, UiColor::card);
    canvas_.drawRoundRect(4, y, 152, 45, 7, UiColor::line);
    canvas_.fillRoundRect(4, y + 11, 3, 23, 1, UiColor::accents[index]);
    icon(index, 12, y + 6, UiColor::accents[index]);
    text(51, y + 7, title(index), UiColor::text);
    text(51, y + 25, captions[index], UiColor::muted);
    char number[4]; snprintf(number, sizeof(number), "%u", index + 1);
    text(140, y + 7, number, UiColor::accents[index]);
  }
  void drawMenu(uint32_t now) {
    header("功能菜单", UiColor::text);
    char count[8]; snprintf(count, sizeof(count), "%u / %u", selected_ + 1, functionCount);
    text(111, 1, count, UiColor::muted);
    canvas_.setViewport(0, 18, 160, 46);
    if (animating_) {
      float elapsed = float(uint32_t(now - animationStart_)) / 160.0f;
      if (elapsed > 1) elapsed = 1;
      const float eased = 1 - powf(1 - elapsed, 3);
      hero(previous_, -direction_ * lroundf(eased * 48));
      hero(selected_, direction_ * lroundf((1 - eased) * 48));
    } else hero(selected_, 0);
    canvas_.resetViewport();
    footer("↑↓选择  中键进入");
  }
  void drawClipboard(uint32_t now) {
    header("复制粘贴", UiColor::accents[0]);
    const bool active = showingNotice(now);
    const uint16_t copyColor = active && lastAction_ == Action::Copy ? noticeColor_ : UiColor::accents[0];
    const uint16_t pasteColor = active && lastAction_ == Action::Paste ? noticeColor_ : UiColor::accents[3];
    canvas_.fillRoundRect(4, 21, 74, 42, 6, UiColor::card);
    canvas_.fillRoundRect(82, 21, 74, 42, 6, UiColor::card);
    canvas_.drawRoundRect(4, 21, 74, 42, 6, copyColor);
    canvas_.drawRoundRect(82, 21, 74, 42, 6, pasteColor);
    centered(41, 25, "复制", copyColor);
    centered(119, 25, "粘贴", pasteColor);
    centered(41, 46, "↑ Ctrl+C", UiColor::text);
    centered(119, 46, "中 Ctrl+V", UiColor::text);
    actionFooter(now, "←返回  上复制 中粘贴");
  }
  void drawPomodoro(uint32_t) {
    const uint16_t accent = pomodoro.resting ? UiColor::green : UiColor::accents[1];
    header("番茄时钟", accent);
    char rounds[12]; snprintf(rounds, sizeof(rounds), "%lu", static_cast<unsigned long>(pomodoro.completed));
    text(118, 1, rounds, UiColor::muted);
    canvas_.drawCircle(29, 41, 23, UiColor::line);
    canvas_.drawCircle(29, 41, 22, UiColor::line);
    canvas_.drawCircle(29, 41, 21, UiColor::line);
    if (pomodoro.progress() > 0.001f) {
      for (int radius = 21; radius <= 23; ++radius)
        arc(29, 41, radius, -90, -90 + pomodoro.progress() * 360, accent);
    }
    centered(29, 35, pomodoro.resting ? "休息" : "专注", accent);
    text(64, 18, pomodoro.running ? "正在计时" : pomodoro.started ? "已暂停" : "↑↓调时", UiColor::muted);
    const uint32_t seconds = pomodoro.secondsLeft();
    char value[12]; snprintf(value, sizeof(value), "%02lu:%02lu",
                            static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60));
    big(108, 31, value, UiColor::text);
    footer(pomodoro.running ? "←返回 中暂停 右重置" : "←返回 中开始 右重置");
  }
  void metric(int y, const char* name, float value, float maximum, const char* suffix, uint16_t color) {
    text(6, y, name, UiColor::muted);
    canvas_.fillRoundRect(33, y + 3, 75, 6, 3, UiColor::line);
    if (value >= 0) {
      const float clamped = value > maximum ? maximum : value;
      int width = lroundf(75 * clamped / maximum);
      if (width > 0) canvas_.fillRoundRect(33, y + 3, width, 6, width < 6 ? width / 2 : 3, color);
    }
    char formatted[14];
    if (value < 0) snprintf(formatted, sizeof(formatted), "--");
    else snprintf(formatted, sizeof(formatted), "%3d%s", int(lroundf(value)), suffix);
    text(116, y, formatted, value < 0 ? UiColor::muted : UiColor::text);
  }
  void drawHardware(uint32_t now) {
    header("硬件监控", UiColor::accents[2]);
    char current[8]; snprintf(current, sizeof(current), "%u/3", hardwarePage_ + 1);
    text(124, 1, current, UiColor::muted);
    const bool fresh = pc_.online && pc_.metricsAvailable &&
                       uint32_t(now - pc_.receivedAt) + pc_.sampleAgeMs < 5000;
    if (!fresh) {
      centered(80, 30, pc_.online ? "等待硬件数据" : "电脑离线", UiColor::yellow);
      centered(80, 47, "PC RECEIVER", UiColor::muted);
    } else if (hardwarePage_ == 0) {
      metric(21, "CPU", pc_.cpu, 100, "%", UiColor::accents[0]);
      metric(36, "RAM", pc_.ram, 100, "%", UiColor::accents[3]);
      metric(51, "GPU", pc_.gpu, 100, "%", UiColor::accents[2]);
    } else if (hardwarePage_ == 1) {
      metric(21, "CPU", pc_.cpuTemp, 100, "C", UiColor::accents[1]);
      metric(36, "GPU", pc_.gpuTemp, 100, "C", UiColor::accents[2]);
      metric(51, "C:", pc_.disk, 100, "%", UiColor::accents[3]);
    } else {
      metric(21, "VRAM", pc_.vram, 100, "%", UiColor::accents[2]);
      metric(36, "RAM", pc_.ram, 100, "%", UiColor::accents[0]);
      metric(51, "DISK", pc_.disk, 100, "%", UiColor::accents[3]);
    }
    footer("←返回  ↑↓切页");
  }
  void drawClock() {
    header("时钟", UiColor::accents[3]);
    const time_t now = time(nullptr);
    if (now < 1700000000) {
      big(80, 24, "--:--", UiColor::muted);
      centered(80, 54, "等待网络校时", UiColor::yellow);
    } else {
      struct tm local;
      localtime_r(&now, &local);
      int hour = local.tm_hour;
      if (twelveHour_) {
        text(120, 1, hour >= 12 ? "PM" : "AM", UiColor::muted);
        hour = hour % 12 ? hour % 12 : 12;
      } else text(118, 1, "24H", UiColor::muted);
      char value[12]; snprintf(value, sizeof(value), "%02d:%02d", hour, local.tm_min);
      big(73, 20, value, UiColor::text);
      char seconds[6]; snprintf(seconds, sizeof(seconds), "%02d", local.tm_sec);
      text(132, 39, seconds, UiColor::accents[3]);
      static const char* days[] = {"日", "一", "二", "三", "四", "五", "六"};
      char date[40]; snprintf(date, sizeof(date), "%02d月%02d日 周%s", local.tm_mon + 1,
                             local.tm_mday, days[local.tm_wday]);
      centered(80, 53, date, UiColor::muted);
    }
    footer("←返回 中键12/24");
  }
  void drawVolume(uint32_t now) {
    header("电脑音量", UiColor::accents[4]);
    const bool available = pc_.online && pc_.volume >= 0 && uint32_t(now - pc_.receivedAt) < 5000;
    icon(4, 8, 26, pc_.muted && available ? UiColor::red : UiColor::accents[4]);
    char value[12];
    if (available) snprintf(value, sizeof(value), "%d%%", pc_.volume);
    else snprintf(value, sizeof(value), "--");
    big(103, 23, value, available ? UiColor::text : UiColor::muted);
    if (available && pc_.muted) text(116, 1, "静音", UiColor::red);
    canvas_.fillRoundRect(47, 56, 105, 6, 3, UiColor::line);
    if (available && pc_.volume > 0) {
      const int width = lroundf(pc_.volume * 1.05f);
      canvas_.fillRoundRect(47, 56, width, 6, width < 6 ? width / 2 : 3, UiColor::accents[4]);
    }
    actionFooter(now, "←返回 ↑↓音量 中静音");
  }
  void drawMedia(uint32_t now) {
    const uint16_t accent = UiColor::accents[5];
    header("媒体控制", accent);
    canvas_.fillRoundRect(4, 20, 43, 34, 5, UiColor::card);
    canvas_.fillRoundRect(51, 20, 58, 34, 5, UiColor::card);
    canvas_.fillRoundRect(113, 20, 43, 34, 5, UiColor::card);
    canvas_.drawRoundRect(51, 20, 58, 34, 5, accent);
    canvas_.fillRect(15, 29, 3, 15, accent);
    canvas_.fillTriangle(34, 29, 34, 44, 21, 37, accent);
    canvas_.fillTriangle(67, 28, 67, 46, 80, 37, accent);
    canvas_.fillRect(86, 29, 3, 16, accent);
    canvas_.fillRect(92, 29, 3, 16, accent);
    canvas_.fillTriangle(125, 29, 125, 44, 138, 37, accent);
    canvas_.fillRect(141, 29, 3, 15, accent);
    centered(80, 55, showingNotice(now) ? noticeText_ : "左键返回", 
             showingNotice(now) ? noticeColor_ : UiColor::muted);
    footer("↑上一首 中播放/暂停 ↓下一首");
  }
};

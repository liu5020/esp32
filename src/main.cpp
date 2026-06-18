#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <driver/i2s.h>
#include <string.h>

#include "secrets.h"

// ST7789 TFT wiring, same as the screen setup that was already working.
static constexpr int TFT_SCLK_PIN = 18;
static constexpr int TFT_MOSI_PIN = 16;
static constexpr int TFT_CS_PIN = 5;
static constexpr int TFT_DC_PIN = 17;
static constexpr int TFT_RST_PIN = 4;
static constexpr int TFT_BL_PIN = 2;

// INMP441 wiring. These pins avoid the TFT screen pins already used in this workspace.
static constexpr int MIC_SCK_PIN = 10; // INMP441 SCK / BCLK
static constexpr int MIC_WS_PIN = 11;  // INMP441 WS / LRCLK
static constexpr int MIC_SD_PIN = 12;  // INMP441 SD / DOUT

// Button wiring: connect one side to GPIO13 and the other side to GND.
static constexpr int BUTTON_PIN = 13;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;

// Speaker wiring for a MAX98357A-style I2S amplifier module.
// Do not connect a bare speaker directly to ESP32 GPIO pins.
static constexpr bool SPEAKER_ENABLED = false;
static constexpr i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_1;
static constexpr int SPEAKER_BCLK_PIN = 14;
static constexpr int SPEAKER_LRC_PIN = 15;
static constexpr int SPEAKER_DIN_PIN = 21;
static constexpr uint32_t SPEAKER_SAMPLE_RATE = 16000;
static constexpr int16_t SPEAKER_AMPLITUDE = 4500;

// L/R wired to GND means the microphone speaks on the left I2S channel.
static constexpr i2s_channel_fmt_t MIC_CHANNEL_FORMAT = I2S_CHANNEL_FMT_ONLY_LEFT;

static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr uint16_t WAV_BITS_PER_SAMPLE = 16;
static constexpr uint16_t WAV_CHANNELS = 1;
static constexpr uint32_t RECORD_SECONDS = 5;
static constexpr size_t DMA_SAMPLES = 512;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t STT_RESPONSE_TIMEOUT_MS = 20000;
static constexpr size_t BLE_RX_BUFFER_LIMIT = 1024;
static constexpr uint16_t COLOR_DARK_GREY = 0x4208;
static constexpr uint16_t SKETCH_WIDTH = 160;
static constexpr uint16_t SKETCH_HEIGHT = 160;
static constexpr size_t SKETCH_BITMAP_BYTES = (SKETCH_WIDTH * SKETCH_HEIGHT) / 8;

static const char *BLE_SERVICE_UUID = "7a8f0001-7d2a-4f2c-8f9d-0a1b2c3d4e5f";
static const char *BLE_CONFIG_WRITE_UUID = "7a8f0002-7d2a-4f2c-8f9d-0a1b2c3d4e5f";
static const char *BLE_STATUS_NOTIFY_UUID = "7a8f0003-7d2a-4f2c-8f9d-0a1b2c3d4e5f";
static const char *CONFIG_NAMESPACE = "voicecfg";

// INMP441 sends 24-bit samples in a 32-bit I2S slot. Lower this value if the
// recording is too quiet; raise it if the waveform clips.
static constexpr int SAMPLE_SHIFT = 14;

static int32_t rawBuffer[DMA_SAMPLES];
static int16_t pcmBuffer[DMA_SAMPLES];
static bool volumeMeterEnabled = true;
static uint32_t lastDisplayUpdateMs = 0;
static bool wifiEverTested = false;
static bool wifiIsConnected = false;
static int32_t wifiLastRssi = -127;
static String wifiLastIp = "-";
static bool speakerReady = false;
static bool buttonStableState = HIGH;
static bool buttonLastReading = HIGH;
static uint32_t buttonLastChangeMs = 0;
static uint8_t sketchBitmap[SKETCH_BITMAP_BYTES];
static String configuredWifiSsid;
static String configuredWifiPassword;
static String configuredBackendBaseUrl;
static bool hasStoredDeviceConfig = false;
static bool bleClientConnected = false;
static String bleRxBuffer;
static bool pendingBleConfig = false;
static String pendingBleWifiSsid;
static String pendingBleWifiPassword;
static String pendingBleBackendBaseUrl;
static BLECharacteristic *bleStatusCharacteristic = nullptr;

static Adafruit_ST7789 tft =
    Adafruit_ST7789(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN, TFT_RST_PIN);

struct HttpEndpoint {
  String host;
  uint16_t port = 80;
  String path = "/";
};

static String trimTrailingSlashes(String value) {
  value.trim();
  while (value.endsWith("/") && value.length() > String("http://").length()) {
    value.remove(value.length() - 1);
  }
  return value;
}

static String normalizeBackendBaseUrl(String value) {
  value = trimTrailingSlashes(value);
  if (value.endsWith("/stt")) {
    value.remove(value.length() - 4);
  }
  return trimTrailingSlashes(value);
}

static String fallbackBackendBaseUrl() {
  String value = STT_SERVER_URL;
  int sttIndex = value.indexOf("/stt");
  if (sttIndex > 0) {
    value = value.substring(0, sttIndex);
  } else {
    int schemeIndex = value.indexOf("://");
    int pathIndex = schemeIndex >= 0 ? value.indexOf('/', schemeIndex + 3) : -1;
    if (pathIndex > 0) {
      value = value.substring(0, pathIndex);
    }
  }
  return trimTrailingSlashes(value);
}

static String activeWifiSsid() {
  return configuredWifiSsid.length() > 0 ? configuredWifiSsid : String(WIFI_SSID);
}

static String activeWifiPassword() {
  return configuredWifiPassword.length() > 0 ? configuredWifiPassword : String(WIFI_PASSWORD);
}

static String activeBackendBaseUrl() {
  return configuredBackendBaseUrl.length() > 0 ? configuredBackendBaseUrl : fallbackBackendBaseUrl();
}

static String activeSttServerUrl() {
  return activeBackendBaseUrl() + "/stt";
}

static bool loadDeviceConfig() {
  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, true)) {
    Serial.println("NVS config open failed; using compile-time fallback.");
    return false;
  }

  configuredWifiSsid = prefs.getString("ssid", "");
  configuredWifiPassword = prefs.getString("pass", "");
  configuredBackendBaseUrl = normalizeBackendBaseUrl(prefs.getString("backend", ""));
  prefs.end();

  hasStoredDeviceConfig = configuredWifiSsid.length() > 0 && configuredBackendBaseUrl.length() > 0;
  if (hasStoredDeviceConfig) {
    Serial.printf("Loaded BLE/NVS config: ssid=%s backend=%s\n",
                  configuredWifiSsid.c_str(),
                  configuredBackendBaseUrl.c_str());
  } else {
    Serial.println("No BLE/NVS config found; using include/secrets.h fallback.");
  }
  return hasStoredDeviceConfig;
}

static bool saveDeviceConfig(const String &ssid, const String &password, const String &backendBaseUrl) {
  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    Serial.println("NVS config write failed: open error.");
    return false;
  }

  String cleanBackend = normalizeBackendBaseUrl(backendBaseUrl);
  size_t ssidWritten = prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  size_t backendWritten = prefs.putString("backend", cleanBackend);
  bool ok = ssidWritten > 0 && backendWritten > 0;
  prefs.end();

  if (ok) {
    configuredWifiSsid = ssid;
    configuredWifiPassword = password;
    configuredBackendBaseUrl = cleanBackend;
    hasStoredDeviceConfig = true;
  }
  return ok;
}

static bool clearDeviceConfig() {
  Preferences prefs;
  if (!prefs.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }
  bool ok = prefs.clear();
  prefs.end();

  if (ok) {
    configuredWifiSsid = "";
    configuredWifiPassword = "";
    configuredBackendBaseUrl = "";
    hasStoredDeviceConfig = false;
  }
  return ok;
}

static void printDeviceConfig() {
  Serial.println("Active device config:");
  Serial.printf("  source: %s\n", hasStoredDeviceConfig ? "BLE/NVS" : "include/secrets.h fallback");
  Serial.printf("  ssid: %s\n", activeWifiSsid().c_str());
  Serial.printf("  backend: %s\n", activeBackendBaseUrl().c_str());
  Serial.printf("  stt: %s\n", activeSttServerUrl().c_str());
  Serial.println("  password: <hidden>");
}

static void putLe16(uint8_t *dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFF);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static void putLe32(uint8_t *dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFF);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

static void makeWavHeader(uint8_t header[44], uint32_t dataBytes) {
  memset(header, 0, 44);
  memcpy(header + 0, "RIFF", 4);
  putLe32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  putLe32(header + 16, 16);
  putLe16(header + 20, 1); // PCM
  putLe16(header + 22, WAV_CHANNELS);
  putLe32(header + 24, SAMPLE_RATE);
  putLe32(header + 28, SAMPLE_RATE * WAV_CHANNELS * (WAV_BITS_PER_SAMPLE / 8));
  putLe16(header + 32, WAV_CHANNELS * (WAV_BITS_PER_SAMPLE / 8));
  putLe16(header + 34, WAV_BITS_PER_SAMPLE);
  memcpy(header + 36, "data", 4);
  putLe32(header + 40, dataBytes);
}

class Base64StreamEncoder {
public:
  void write(const uint8_t *data, size_t length) {
    while (length > 0) {
      carry[carryLength++] = *data++;
      length--;

      if (carryLength == 3) {
        writeTriple(carry, 3);
        carryLength = 0;
      }
    }
  }

  void finish() {
    if (carryLength > 0) {
      uint8_t validBytes = carryLength;
      while (carryLength < 3) {
        carry[carryLength++] = 0;
      }
      writeTriple(carry, validBytes);
      carryLength = 0;
    }
    if (lineLength != 0) {
      Serial.println();
      lineLength = 0;
    }
  }

private:
  uint8_t carry[3] = {0, 0, 0};
  uint8_t carryLength = 0;
  uint8_t lineLength = 0;

  void writeChar(char c) {
    Serial.write(c);
    lineLength++;
    if (lineLength >= 76) {
      Serial.println();
      lineLength = 0;
    }
  }

  void writeTriple(const uint8_t in[3], uint8_t validBytes) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    writeChar(table[in[0] >> 2]);
    writeChar(table[((in[0] & 0x03) << 4) | (in[1] >> 4)]);
    writeChar(validBytes > 1 ? table[((in[1] & 0x0F) << 2) | (in[2] >> 6)] : '=');
    writeChar(validBytes > 2 ? table[in[2] & 0x3F] : '=');
  }
};

static int16_t convertRawSample(int32_t rawSample) {
  int32_t scaled = rawSample >> SAMPLE_SHIFT;
  if (scaled > 32767) {
    scaled = 32767;
  } else if (scaled < -32768) {
    scaled = -32768;
  }
  return static_cast<int16_t>(scaled);
}

static size_t readPcm16Block(int16_t *out, size_t maxSamples) {
  size_t bytesRead = 0;
  esp_err_t err = i2s_read(
      I2S_PORT,
      rawBuffer,
      sizeof(rawBuffer),
      &bytesRead,
      portMAX_DELAY);

  if (err != ESP_OK || bytesRead == 0) {
    return 0;
  }

  size_t samplesRead = bytesRead / sizeof(rawBuffer[0]);
  if (samplesRead > maxSamples) {
    samplesRead = maxSamples;
  }

  for (size_t i = 0; i < samplesRead; i++) {
    out[i] = convertRawSample(rawBuffer[i]);
  }

  return samplesRead;
}

static uint32_t averageAbs(const int16_t *samples, size_t count) {
  if (count == 0) {
    return 0;
  }

  uint64_t sum = 0;
  for (size_t i = 0; i < count; i++) {
    int32_t sample = samples[i];
    sum += static_cast<uint32_t>(sample < 0 ? -sample : sample);
  }

  return static_cast<uint32_t>(sum / count);
}

static void printVolumeBar(uint32_t level) {
  uint8_t bars = static_cast<uint8_t>(level / 300);
  if (bars > 40) {
    bars = 40;
  }

  Serial.printf("mic level %5lu |", static_cast<unsigned long>(level));
  for (uint8_t i = 0; i < 40; i++) {
    Serial.write(i < bars ? '#' : '.');
  }
  Serial.println('|');
}

static bool initSpeakerI2S() {
  if (!SPEAKER_ENABLED) {
    return false;
  }

  i2s_config_t i2sConfig;
  memset(&i2sConfig, 0, sizeof(i2sConfig));
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = SPEAKER_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = 0;
  i2sConfig.dma_buf_count = 4;
  i2sConfig.dma_buf_len = 128;
  i2sConfig.use_apll = false;

  esp_err_t err = i2s_driver_install(SPEAKER_I2S_PORT, &i2sConfig, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("speaker i2s_driver_install failed: %d\n", err);
    return false;
  }

  i2s_pin_config_t pinConfig;
  memset(&pinConfig, 0, sizeof(pinConfig));
  pinConfig.bck_io_num = SPEAKER_BCLK_PIN;
  pinConfig.ws_io_num = SPEAKER_LRC_PIN;
  pinConfig.data_out_num = SPEAKER_DIN_PIN;
  pinConfig.data_in_num = -1;

  err = i2s_set_pin(SPEAKER_I2S_PORT, &pinConfig);
  if (err != ESP_OK) {
    Serial.printf("speaker i2s_set_pin failed: %d\n", err);
    return false;
  }

  i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
  return true;
}

static void writeSpeakerSilence(uint32_t durationMs) {
  if (!speakerReady || durationMs == 0) {
    return;
  }

  static int16_t silence[128 * 2];
  memset(silence, 0, sizeof(silence));

  uint32_t framesRemaining = (SPEAKER_SAMPLE_RATE * durationMs) / 1000;
  while (framesRemaining > 0) {
    uint32_t frames = framesRemaining > 128 ? 128 : framesRemaining;
    size_t bytesWritten = 0;
    i2s_write(
        SPEAKER_I2S_PORT,
        silence,
        frames * 2 * sizeof(int16_t),
        &bytesWritten,
        portMAX_DELAY);
    framesRemaining -= frames;
  }
}

static void playTone(uint16_t frequencyHz, uint32_t durationMs) {
  if (!speakerReady || frequencyHz == 0 || durationMs == 0) {
    return;
  }

  static int16_t frames[128 * 2];
  uint32_t phase = 0;
  uint32_t phaseStep = (static_cast<uint32_t>(frequencyHz) << 16) / SPEAKER_SAMPLE_RATE;
  uint32_t framesRemaining = (SPEAKER_SAMPLE_RATE * durationMs) / 1000;

  while (framesRemaining > 0) {
    uint32_t frameCount = framesRemaining > 128 ? 128 : framesRemaining;
    for (uint32_t i = 0; i < frameCount; i++) {
      int16_t sample = (phase & 0x8000) ? SPEAKER_AMPLITUDE : -SPEAKER_AMPLITUDE;
      frames[i * 2] = sample;
      frames[i * 2 + 1] = sample;
      phase += phaseStep;
    }

    size_t bytesWritten = 0;
    i2s_write(
        SPEAKER_I2S_PORT,
        frames,
        frameCount * 2 * sizeof(int16_t),
        &bytesWritten,
        portMAX_DELAY);
    framesRemaining -= frameCount;
  }

  writeSpeakerSilence(25);
}

static void playReadySound() {
  playTone(880, 70);
  writeSpeakerSilence(30);
  playTone(1320, 70);
}

static void playStartSound() {
  playTone(660, 90);
}

static void playOkSound() {
  playTone(880, 70);
  writeSpeakerSilence(25);
  playTone(1175, 100);
}

static void playErrorSound() {
  playTone(220, 180);
}

static void drawCenteredText(const char *text, int16_t y, uint16_t color, uint8_t size) {
  int16_t x1;
  int16_t y1;
  uint16_t w;
  uint16_t h;

  tft.setTextSize(size);
  tft.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  tft.setCursor((tft.width() - w) / 2, y);
  tft.setTextColor(color);
  tft.print(text);
}

static void initDisplay() {
  pinMode(TFT_BL_PIN, OUTPUT);
  digitalWrite(TFT_BL_PIN, HIGH);

  tft.init(240, 320, SPI_MODE0);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);
  drawCenteredText("VOICE MIC", 28, ST77XX_CYAN, 3);
  drawCenteredText("booting...", 82, ST77XX_WHITE, 2);

  tft.drawFastHLine(18, 122, 204, ST77XX_BLUE);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(20, 152);
  tft.print("SCK 10");
  tft.setCursor(20, 180);
  tft.print("WS  11");
  tft.setCursor(20, 208);
  tft.print("SD  12");
}

static void showReadyScreen() {
  tft.fillRect(0, 76, 240, 54, ST77XX_BLACK);
  drawCenteredText("mic ready", 84, ST77XX_GREEN, 2);
  tft.drawRect(20, 264, 200, 24, ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(20, 294);
  tft.print("BTN13=talk  b=beep");
}

static void showStatus(const char *status, uint16_t color) {
  tft.fillRect(0, 76, 240, 54, ST77XX_BLACK);
  drawCenteredText(status, 84, color, 2);
}

static void showSketchBitmap(const uint8_t *bitmap, uint16_t width, uint16_t height) {
  int16_t x0 = (tft.width() - width) / 2;
  int16_t y0 = 92;

  tft.fillScreen(ST77XX_BLACK);
  drawCenteredText("AI SKETCH", 18, ST77XX_CYAN, 2);
  drawCenteredText("preview", 48, ST77XX_WHITE, 1);

  tft.fillRect(x0 - 3, y0 - 3, width + 6, height + 6, COLOR_DARK_GREY);
  tft.fillRect(x0, y0, width, height, ST77XX_WHITE);

  for (uint16_t y = 0; y < height; y++) {
    for (uint16_t x = 0; x < width; x++) {
      size_t bitIndex = static_cast<size_t>(y) * width + x;
      uint8_t mask = 1 << (7 - (bitIndex % 8));
      if ((bitmap[bitIndex / 8] & mask) != 0) {
        tft.drawPixel(x0 + x, y0 + y, ST77XX_BLACK);
      }
    }
  }

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(20, 274);
  tft.print("BTN13=new sketch");
  tft.setCursor(20, 292);
  tft.print("d=demo  v=meter");
}

static uint8_t wifiBarsFromRssi(int32_t rssi) {
  if (rssi >= -55) {
    return 4;
  }
  if (rssi >= -67) {
    return 3;
  }
  if (rssi >= -75) {
    return 2;
  }
  if (rssi >= -85) {
    return 1;
  }
  return 0;
}

static void drawWifiIcon(int16_t x, int16_t y, uint8_t bars, uint16_t color) {
  static constexpr int16_t barWidth = 6;
  static constexpr int16_t gap = 3;
  static constexpr int16_t maxHeight = 30;

  for (uint8_t i = 0; i < 4; i++) {
    int16_t height = 8 + i * 7;
    int16_t bx = x + i * (barWidth + gap);
    int16_t by = y + maxHeight - height;
    uint16_t outline = i < bars ? color : COLOR_DARK_GREY;

    tft.drawRect(bx, by, barWidth, height, outline);
    if (i < bars) {
      tft.fillRect(bx + 1, by + 1, barWidth - 2, height - 2, color);
    }
  }
}

static String shortSsid() {
  String ssid = activeWifiSsid();
  if (ssid.length() > 17) {
    ssid = ssid.substring(0, 15) + "..";
  }
  return ssid;
}

static void drawWifiPanel(const char *stateText, uint16_t color, int32_t rssi, const String &detail) {
  tft.fillRect(10, 132, 220, 86, ST77XX_BLACK);
  tft.drawRoundRect(12, 134, 216, 82, 6, COLOR_DARK_GREY);

  uint8_t bars = wifiIsConnected ? wifiBarsFromRssi(rssi) : 0;
  drawWifiIcon(24, 166, bars, color);

  tft.setTextSize(2);
  tft.setTextColor(color);
  tft.setCursor(72, 146);
  tft.print(stateText);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(72, 172);
  tft.print(shortSsid());

  tft.setCursor(72, 190);
  tft.print(detail);
}

static void updateDisplayVolume(uint32_t level) {
  uint32_t now = millis();
  if (now - lastDisplayUpdateMs < 180) {
    return;
  }
  lastDisplayUpdateMs = now;

  int barWidth = static_cast<int>(level / 60);
  if (barWidth > 196) {
    barWidth = 196;
  }

  tft.fillRect(22, 266, 196, 20, ST77XX_BLACK);
  uint16_t barColor = level > 5000 ? ST77XX_RED : (level > 1200 ? ST77XX_YELLOW : ST77XX_GREEN);
  tft.fillRect(22, 266, barWidth, 20, barColor);

  tft.fillRect(20, 238, 200, 18, ST77XX_BLACK);
  char levelText[24];
  snprintf(levelText, sizeof(levelText), "level %lu", static_cast<unsigned long>(level));
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(20, 238);
  tft.print(levelText);
}

static bool initMicI2S() {
  i2s_config_t i2sConfig;
  memset(&i2sConfig, 0, sizeof(i2sConfig));
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2sConfig.channel_format = MIC_CHANNEL_FORMAT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = 0;
  i2sConfig.dma_buf_count = 8;
  i2sConfig.dma_buf_len = DMA_SAMPLES;
  i2sConfig.use_apll = false;

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("i2s_driver_install failed: %d\n", err);
    return false;
  }

  i2s_pin_config_t pinConfig;
  memset(&pinConfig, 0, sizeof(pinConfig));
  pinConfig.bck_io_num = MIC_SCK_PIN;
  pinConfig.ws_io_num = MIC_WS_PIN;
  pinConfig.data_out_num = -1;
  pinConfig.data_in_num = MIC_SD_PIN;

  err = i2s_set_pin(I2S_PORT, &pinConfig);
  if (err != ESP_OK) {
    Serial.printf("i2s_set_pin failed: %d\n", err);
    return false;
  }

  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

static void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  h - show this help");
  Serial.println("  p - print active WiFi/backend config");
  Serial.println("  c - clear BLE/NVS config and use secrets.h fallback");
  Serial.println("  v - toggle live volume meter");
  Serial.println("  w - test WiFi and update the screen");
  Serial.println("  b - test speaker beep");
  Serial.println("  d - fetch a demo sketch and show it on the screen");
  Serial.println("  r - record 5 seconds and print WAV Base64");
  Serial.println("  s - record 5 seconds and send to STT server");
  Serial.println("Button:");
  Serial.println("  GPIO13 -> GND triggers speech-to-text recording");
  Serial.println();
}

static bool isPlaceholderConfig() {
  return activeWifiSsid() == "YOUR_WIFI_SSID" ||
         activeBackendBaseUrl() == fallbackBackendBaseUrl() &&
             String(STT_SERVER_URL) == "http://192.168.1.100:8787/stt";
}

static bool parseHttpUrl(const String &url, HttpEndpoint &endpoint) {
  String value(url);
  const String prefix = "http://";
  if (!value.startsWith(prefix)) {
    Serial.println("Only http:// backend URLs are supported by this firmware starter.");
    return false;
  }

  value.remove(0, prefix.length());
  int slashIndex = value.indexOf('/');
  String hostPort = slashIndex >= 0 ? value.substring(0, slashIndex) : value;
  endpoint.path = slashIndex >= 0 ? value.substring(slashIndex) : "/";

  int colonIndex = hostPort.indexOf(':');
  if (colonIndex >= 0) {
    endpoint.host = hostPort.substring(0, colonIndex);
    endpoint.port = static_cast<uint16_t>(hostPort.substring(colonIndex + 1).toInt());
  } else {
    endpoint.host = hostPort;
    endpoint.port = 80;
  }

  return endpoint.host.length() > 0 && endpoint.port > 0;
}

static bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiEverTested = true;
    wifiIsConnected = true;
    wifiLastRssi = WiFi.RSSI();
    wifiLastIp = WiFi.localIP().toString();
    drawWifiPanel("WIFI OK", ST77XX_GREEN, wifiLastRssi, wifiLastIp);
    return true;
  }

  String ssid = activeWifiSsid();
  String password = activeWifiPassword();
  if (ssid == "YOUR_WIFI_SSID" || ssid.length() == 0) {
    Serial.println("WiFi is not configured. Use BLE mini program setup or edit include/secrets.h.");
    wifiEverTested = true;
    wifiIsConnected = false;
    wifiLastRssi = -127;
    wifiLastIp = "-";
    showStatus("wifi config?", ST77XX_RED);
    drawWifiPanel("NO CONFIG", ST77XX_RED, wifiLastRssi, "use miniapp");
    return false;
  }

  showStatus("wifi...", ST77XX_YELLOW);
  drawWifiPanel("WIFI...", ST77XX_YELLOW, -127, "connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  uint32_t startMs = millis();
  uint32_t lastUiMs = 0;
  uint8_t dotCount = 0;
  Serial.printf("Connecting WiFi: %s", ssid.c_str());
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print('.');
    if (millis() - lastUiMs > 700) {
      lastUiMs = millis();
      dotCount = (dotCount + 1) % 4;
      String detail = "connecting";
      for (uint8_t i = 0; i < dotCount; i++) {
        detail += ".";
      }
      drawWifiPanel("WIFI...", ST77XX_YELLOW, -127, detail);
    }
    delay(300);
  }
  Serial.println();

  wifiEverTested = true;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed.");
    wifiIsConnected = false;
    wifiLastRssi = -127;
    wifiLastIp = "-";
    showStatus("wifi failed", ST77XX_RED);
    drawWifiPanel("FAILED", ST77XX_RED, wifiLastRssi, "check SSID/pass");
    return false;
  }

  wifiIsConnected = true;
  wifiLastRssi = WiFi.RSSI();
  wifiLastIp = WiFi.localIP().toString();

  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("WiFi RSSI: %ld dBm\n", static_cast<long>(wifiLastRssi));
  showStatus("wifi ok", ST77XX_GREEN);
  drawWifiPanel("WIFI OK", ST77XX_GREEN, wifiLastRssi, wifiLastIp);
  delay(500);
  return true;
}

static void runWifiScreenTest() {
  volumeMeterEnabled = false;
  bool connected = connectWiFi();
  if (connected) {
    String detail = wifiLastIp + "  " + String(wifiLastRssi) + "dBm";
    drawWifiPanel("WIFI OK", ST77XX_GREEN, wifiLastRssi, detail);
  }
  delay(1400);
  showReadyScreen();
  Serial.println("Volume meter stays off. Press v to enable it again.");
}

static String readHttpResponse(WiFiClient &client, uint32_t timeoutMs) {
  String response;
  uint32_t deadline = millis() + timeoutMs;

  while (millis() < deadline) {
    while (client.available() > 0) {
      response += static_cast<char>(client.read());
      deadline = millis() + 1500;
    }

    if (!client.connected() && client.available() == 0) {
      break;
    }
    delay(10);
  }

  return response;
}

static String httpBodyFromResponse(const String &response) {
  int bodyIndex = response.indexOf("\r\n\r\n");
  if (bodyIndex < 0) {
    return response;
  }
  return response.substring(bodyIndex + 4);
}

static bool showSketchFromJson(const String &body);

static void fetchDemoSketch() {
  volumeMeterEnabled = false;

  if (isPlaceholderConfig()) {
    Serial.println("Configure WiFi/backend by BLE mini program first, or edit include/secrets.h.");
    showStatus("set config", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  if (!connectWiFi()) {
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  HttpEndpoint endpoint;
  if (!parseHttpUrl(activeBackendBaseUrl(), endpoint)) {
    showStatus("bad url", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  endpoint.path = "/draw?text=cat";
  showStatus("draw demo", ST77XX_YELLOW);

  WiFiClient client;
  if (!client.connect(endpoint.host.c_str(), endpoint.port)) {
    Serial.println("Could not connect to sketch server.");
    showStatus("draw offline", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  client.printf("GET %s HTTP/1.1\r\n", endpoint.path.c_str());
  client.printf("Host: %s:%u\r\n", endpoint.host.c_str(), endpoint.port);
  client.print("Accept: application/json\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");

  String response = readHttpResponse(client, STT_RESPONSE_TIMEOUT_MS);
  client.stop();

  String body = httpBodyFromResponse(response);
  body.trim();
  Serial.println("Sketch demo response:");
  Serial.println(body);

  if (showSketchFromJson(body)) {
    Serial.println("Demo sketch is on the screen. Press BTN13 or s for a new voice sketch.");
    playOkSound();
  } else {
    showStatus("draw error", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
  }

  Serial.println("Volume meter stays off. Press v to enable it again.");
}

static String extractJsonStringField(const String &body, const char *fieldName) {
  String key = "\"";
  key += fieldName;
  key += "\"";

  int keyIndex = body.indexOf(key);
  if (keyIndex < 0) {
    return "";
  }

  int colonIndex = body.indexOf(':', keyIndex);
  int firstQuote = body.indexOf('"', colonIndex + 1);
  if (colonIndex < 0 || firstQuote < 0) {
    return "";
  }

  String out;
  bool escaping = false;
  for (int i = firstQuote + 1; i < body.length(); i++) {
    char c = body[i];
    if (escaping) {
      switch (c) {
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      default:
        out += c;
        break;
      }
      escaping = false;
    } else if (c == '\\') {
      escaping = true;
    } else if (c == '"') {
      break;
    } else {
      out += c;
    }
  }

  return out;
}

static int extractJsonIntField(const String &body, const char *fieldName, int fallback) {
  String key = "\"";
  key += fieldName;
  key += "\"";

  int keyIndex = body.indexOf(key);
  if (keyIndex < 0) {
    return fallback;
  }

  int colonIndex = body.indexOf(':', keyIndex);
  if (colonIndex < 0) {
    return fallback;
  }

  int index = colonIndex + 1;
  while (index < body.length() && (body[index] == ' ' || body[index] == '\t')) {
    index++;
  }

  bool negative = false;
  if (index < body.length() && body[index] == '-') {
    negative = true;
    index++;
  }

  int value = 0;
  bool foundDigit = false;
  while (index < body.length() && body[index] >= '0' && body[index] <= '9') {
    foundDigit = true;
    value = value * 10 + (body[index] - '0');
    index++;
  }

  if (!foundDigit) {
    return fallback;
  }
  return negative ? -value : value;
}

static String extractJsonTextField(const String &body) {
  return extractJsonStringField(body, "text");
}

static int8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + c - 'a';
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + c - 'A';
  }
  return -1;
}

static bool decodeHexBitmap(const String &hex, uint8_t *out, size_t outLength) {
  if (hex.length() != static_cast<int>(outLength * 2)) {
    Serial.printf("Sketch bitmap length mismatch: got %d hex chars, expected %u\n",
                  hex.length(),
                  static_cast<unsigned>(outLength * 2));
    return false;
  }

  for (size_t i = 0; i < outLength; i++) {
    int8_t high = hexNibble(hex[i * 2]);
    int8_t low = hexNibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      Serial.println("Sketch bitmap contains a non-hex character.");
      return false;
    }
    out[i] = static_cast<uint8_t>((high << 4) | low);
  }

  return true;
}

static bool showSketchFromJson(const String &body) {
  int width = extractJsonIntField(body, "width", 0);
  int height = extractJsonIntField(body, "height", 0);
  String bitmapHex = extractJsonStringField(body, "bitmap");

  if (width != SKETCH_WIDTH || height != SKETCH_HEIGHT || bitmapHex.length() == 0) {
    Serial.printf("No usable sketch in response. width=%d height=%d bitmap chars=%d\n",
                  width,
                  height,
                  bitmapHex.length());
    return false;
  }

  if (!decodeHexBitmap(bitmapHex, sketchBitmap, sizeof(sketchBitmap))) {
    return false;
  }

  showSketchBitmap(sketchBitmap, SKETCH_WIDTH, SKETCH_HEIGHT);
  return true;
}

static String bleDeviceName() {
  uint16_t suffix = static_cast<uint16_t>(ESP.getEfuseMac() & 0xFFFF);
  char name[24];
  snprintf(name, sizeof(name), "VoiceSketch-%04X", suffix);
  return String(name);
}

static void sendBleStatus(const char *event, const char *message) {
  if (bleStatusCharacteristic == nullptr) {
    return;
  }

  String payload = "{";
  payload += "\"event\":\"";
  payload += event;
  payload += "\",\"message\":\"";
  payload += message;
  payload += "\",\"configured\":";
  payload += hasStoredDeviceConfig ? "true" : "false";
  payload += ",\"wifi\":\"";
  payload += wifiIsConnected ? "connected" : "disconnected";
  payload += "\",\"ip\":\"";
  payload += wifiLastIp;
  payload += "\",\"rssi\":";
  payload += String(wifiLastRssi);
  payload += ",\"backend\":\"";
  payload += activeBackendBaseUrl();
  payload += "\"}";

  bleStatusCharacteristic->setValue(payload.c_str());
  if (bleClientConnected) {
    bleStatusCharacteristic->notify();
  }
}

static void queueBleConfigMessage(const String &message) {
  String ssid = extractJsonStringField(message, "ssid");
  String password = extractJsonStringField(message, "password");
  String backendBaseUrl = extractJsonStringField(message, "base_url");
  if (backendBaseUrl.length() == 0) {
    backendBaseUrl = extractJsonStringField(message, "backend_url");
  }

  backendBaseUrl = normalizeBackendBaseUrl(backendBaseUrl);
  if (ssid.length() == 0 || backendBaseUrl.length() == 0) {
    Serial.println("BLE config rejected: missing ssid or backend base URL.");
    sendBleStatus("config_error", "missing ssid or backend");
    return;
  }

  if (!backendBaseUrl.startsWith("http://")) {
    Serial.println("BLE config rejected: only http:// backend URLs are supported.");
    sendBleStatus("config_error", "backend must start with http://");
    return;
  }

  pendingBleWifiSsid = ssid;
  pendingBleWifiPassword = password;
  pendingBleBackendBaseUrl = backendBaseUrl;
  pendingBleConfig = true;

  Serial.printf("BLE config received: ssid=%s backend=%s\n",
                ssid.c_str(),
                backendBaseUrl.c_str());
  sendBleStatus("config_received", "saving");
}

class VoiceSketchBleServerCallbacks : public BLEServerCallbacks {
public:
  void onConnect(BLEServer *server) override {
    (void)server;
    bleClientConnected = true;
    Serial.println("BLE client connected.");
    sendBleStatus("connected", "ble connected");
  }

  void onDisconnect(BLEServer *server) override {
    (void)server;
    bleClientConnected = false;
    Serial.println("BLE client disconnected; advertising again.");
    BLEDevice::startAdvertising();
  }
};

class VoiceSketchConfigWriteCallbacks : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *characteristic) override {
    auto raw = characteristic->getValue();
    if (raw.length() == 0) {
      return;
    }

    for (size_t i = 0; i < raw.length(); i++) {
      char c = raw[i];
      if (c == '\n' || c == '\r') {
        String message = bleRxBuffer;
        message.trim();
        bleRxBuffer = "";
        if (message.length() > 0) {
          queueBleConfigMessage(message);
        }
      } else {
        bleRxBuffer += c;
        if (bleRxBuffer.length() > BLE_RX_BUFFER_LIMIT) {
          bleRxBuffer = "";
          Serial.println("BLE config rejected: message too long.");
          sendBleStatus("config_error", "message too long");
        }
      }
    }
  }
};

static void initBleConfigService() {
  String name = bleDeviceName();
  BLEDevice::init(name.c_str());

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new VoiceSketchBleServerCallbacks());

  BLEService *service = server->createService(BLE_SERVICE_UUID);
  BLECharacteristic *writeCharacteristic = service->createCharacteristic(
      BLE_CONFIG_WRITE_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  writeCharacteristic->setCallbacks(new VoiceSketchConfigWriteCallbacks());

  bleStatusCharacteristic = service->createCharacteristic(
      BLE_STATUS_NOTIFY_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleStatusCharacteristic->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf("BLE config service advertising as %s\n", name.c_str());
  sendBleStatus("advertising", "ready");
}

static void handlePendingBleConfig() {
  if (!pendingBleConfig) {
    return;
  }

  String ssid = pendingBleWifiSsid;
  String password = pendingBleWifiPassword;
  String backendBaseUrl = pendingBleBackendBaseUrl;
  pendingBleConfig = false;

  showStatus("ble config", ST77XX_CYAN);
  bool saved = saveDeviceConfig(ssid, password, backendBaseUrl);
  if (!saved) {
    Serial.println("BLE config save failed.");
    sendBleStatus("config_error", "save failed");
    showStatus("save error", ST77XX_RED);
    delay(1200);
    showReadyScreen();
    return;
  }

  Serial.println("BLE config saved to NVS.");
  printDeviceConfig();
  sendBleStatus("config_saved", "wifi connecting");

  WiFi.disconnect();
  delay(250);
  bool connected = connectWiFi();
  sendBleStatus(connected ? "wifi_ok" : "wifi_failed", connected ? "connected" : "check ssid/password");
  delay(1000);
  showReadyScreen();
}

static bool streamWavToClient(WiFiClient &client, uint32_t seconds) {
  uint32_t totalSamples = SAMPLE_RATE * seconds;
  uint32_t dataBytes = totalSamples * sizeof(int16_t);
  uint8_t wavHeader[44];
  makeWavHeader(wavHeader, dataBytes);

  if (client.write(wavHeader, sizeof(wavHeader)) != sizeof(wavHeader)) {
    return false;
  }

  uint32_t samplesWritten = 0;
  while (samplesWritten < totalSamples) {
    size_t remaining = totalSamples - samplesWritten;
    size_t samplesToWrite = remaining > DMA_SAMPLES ? DMA_SAMPLES : remaining;
    size_t samplesRead = readPcm16Block(pcmBuffer, samplesToWrite);
    if (samplesRead == 0) {
      continue;
    }

    size_t bytesToWrite = samplesRead * sizeof(int16_t);
    if (client.write(reinterpret_cast<const uint8_t *>(pcmBuffer), bytesToWrite) != bytesToWrite) {
      return false;
    }
    samplesWritten += samplesRead;
  }

  return true;
}

static void recordAndSendToStt(uint32_t seconds) {
  volumeMeterEnabled = false;

  if (isPlaceholderConfig()) {
    Serial.println("Configure WiFi/backend by BLE mini program first, or edit include/secrets.h.");
    showStatus("set config", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  if (!connectWiFi()) {
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  HttpEndpoint endpoint;
  String sttServerUrl = activeSttServerUrl();
  if (!parseHttpUrl(sttServerUrl, endpoint)) {
    showStatus("bad url", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  showStatus("connect stt", ST77XX_YELLOW);
  WiFiClient client;
  if (!client.connect(endpoint.host.c_str(), endpoint.port)) {
    Serial.println("Could not connect to STT server.");
    showStatus("stt offline", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  uint32_t contentLength = 44 + (SAMPLE_RATE * seconds * sizeof(int16_t));
  client.printf("POST %s HTTP/1.1\r\n", endpoint.path.c_str());
  client.printf("Host: %s:%u\r\n", endpoint.host.c_str(), endpoint.port);
  client.print("Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %lu\r\n", static_cast<unsigned long>(contentLength));
  client.print("Connection: close\r\n");
  client.print("\r\n");

  Serial.printf("Recording and uploading %lu seconds to %s\n",
                static_cast<unsigned long>(seconds),
                sttServerUrl.c_str());
  showStatus("speak now", ST77XX_CYAN);
  playStartSound();

  bool writeOk = streamWavToClient(client, seconds);
  if (!writeOk) {
    Serial.println("Upload failed while streaming audio.");
    client.stop();
    showStatus("send failed", ST77XX_RED);
    playErrorSound();
    delay(1200);
    showReadyScreen();
    return;
  }

  showStatus("thinking", ST77XX_YELLOW);
  String response = readHttpResponse(client, STT_RESPONSE_TIMEOUT_MS);
  client.stop();

  String body = httpBodyFromResponse(response);
  body.trim();
  Serial.println("STT response:");
  Serial.println(body);

  String text = extractJsonTextField(body);
  bool sketchShown = false;
  if (text.length() > 0) {
    Serial.print("Transcript: ");
    Serial.println(text);
    sketchShown = showSketchFromJson(body);
    if (!sketchShown) {
      showStatus("stt ok", ST77XX_GREEN);
    }
    playOkSound();
  } else {
    showStatus("stt error", ST77XX_RED);
    playErrorSound();
  }

  if (!sketchShown) {
    delay(1500);
    showReadyScreen();
  } else {
    Serial.println("Sketch preview stays on screen. Press BTN13 or s for a new voice sketch.");
  }
  Serial.println("Volume meter stays off. Press v to enable it again.");
}

static void recordWavBase64(uint32_t seconds) {
  uint32_t totalSamples = SAMPLE_RATE * seconds;
  uint32_t dataBytes = totalSamples * sizeof(int16_t);
  uint8_t wavHeader[44];
  makeWavHeader(wavHeader, dataBytes);

  Serial.println();
  Serial.printf("Recording %lu seconds. Keep quiet until you start speaking.\n",
                static_cast<unsigned long>(seconds));
  tft.fillRect(0, 76, 240, 54, ST77XX_BLACK);
  drawCenteredText("recording", 84, ST77XX_YELLOW, 2);
  delay(300);

  Serial.println("-----BEGIN WAV BASE64-----");
  Base64StreamEncoder encoder;
  encoder.write(wavHeader, sizeof(wavHeader));

  uint32_t samplesWritten = 0;
  while (samplesWritten < totalSamples) {
    size_t remaining = totalSamples - samplesWritten;
    size_t samplesToWrite = remaining > DMA_SAMPLES ? DMA_SAMPLES : remaining;
    size_t samplesRead = readPcm16Block(pcmBuffer, samplesToWrite);
    if (samplesRead == 0) {
      continue;
    }

    encoder.write(reinterpret_cast<const uint8_t *>(pcmBuffer), samplesRead * sizeof(int16_t));
    samplesWritten += samplesRead;
  }

  encoder.finish();
  Serial.println("-----END WAV BASE64-----");
  Serial.printf("Done. WAV: %lu Hz, %u-bit, mono, %lu bytes audio data.\n",
                static_cast<unsigned long>(SAMPLE_RATE),
                WAV_BITS_PER_SAMPLE,
                static_cast<unsigned long>(dataBytes));
  showReadyScreen();
}

static void initButton() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  buttonStableState = digitalRead(BUTTON_PIN);
  buttonLastReading = buttonStableState;
  buttonLastChangeMs = millis();
}

static void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != buttonLastReading) {
    buttonLastReading = reading;
    buttonLastChangeMs = millis();
  }

  if (millis() - buttonLastChangeMs < BUTTON_DEBOUNCE_MS) {
    return;
  }

  if (reading == buttonStableState) {
    return;
  }

  buttonStableState = reading;
  if (buttonStableState == LOW) {
    Serial.println("Button pressed: start speech-to-text recording");
    showStatus("button rec", ST77XX_CYAN);
    recordAndSendToStt(RECORD_SECONDS);
  }
}

static void handleSerialCommands() {
  while (Serial.available() > 0) {
    char command = static_cast<char>(Serial.read());
    if (command == '\r' || command == '\n' || command == ' ') {
      continue;
    }

    switch (command) {
    case 'h':
    case 'H':
      printHelp();
      break;
    case 'p':
    case 'P':
      printDeviceConfig();
      break;
    case 'c':
    case 'C':
      if (clearDeviceConfig()) {
        Serial.println("BLE/NVS config cleared. Reboot or press w to test fallback config.");
        showStatus("config clear", ST77XX_YELLOW);
        sendBleStatus("config_cleared", "using fallback");
        delay(1000);
        showReadyScreen();
      } else {
        Serial.println("Failed to clear BLE/NVS config.");
        showStatus("clear failed", ST77XX_RED);
      }
      break;
    case 'v':
    case 'V':
      volumeMeterEnabled = !volumeMeterEnabled;
      Serial.printf("Volume meter: %s\n", volumeMeterEnabled ? "on" : "off");
      break;
    case 'w':
    case 'W':
      runWifiScreenTest();
      break;
    case 'b':
    case 'B':
      Serial.println("Speaker beep test");
      playReadySound();
      break;
    case 'd':
    case 'D':
      fetchDemoSketch();
      break;
    case 'r':
    case 'R':
      volumeMeterEnabled = false;
      recordWavBase64(RECORD_SECONDS);
      Serial.println("Volume meter stays off. Press v to enable it again.");
      break;
    case 's':
    case 'S':
      recordAndSendToStt(RECORD_SECONDS);
      break;
    default:
      Serial.printf("Unknown command '%c'. Press h for help.\n", command);
      break;
    }
  }
}

void setup() {
  Serial.begin(921600);
  delay(600);

  Serial.println();
  Serial.println("ESP32-S3 INMP441 voice input starter");
  Serial.printf("I2S pins: SCK=%d, WS=%d, SD=%d, L/R=GND -> left channel\n",
                MIC_SCK_PIN,
                MIC_WS_PIN,
                MIC_SD_PIN);
  loadDeviceConfig();

  initDisplay();
  initButton();
  initBleConfigService();
  speakerReady = initSpeakerI2S();
  Serial.printf("Speaker: %s (BCLK=%d, LRC=%d, DIN=%d)\n",
                speakerReady ? "ready" : "disabled/error",
                SPEAKER_BCLK_PIN,
                SPEAKER_LRC_PIN,
                SPEAKER_DIN_PIN);

  if (!initMicI2S()) {
    Serial.println("Microphone init failed. Check wiring and reboot.");
    tft.fillRect(0, 76, 240, 54, ST77XX_BLACK);
    drawCenteredText("mic error", 84, ST77XX_RED, 2);
    while (true) {
      delay(1000);
    }
  }

  // Drop a few startup blocks so the DMA and mic settle before monitoring.
  for (uint8_t i = 0; i < 6; i++) {
    readPcm16Block(pcmBuffer, DMA_SAMPLES);
  }

  Serial.println("Microphone ready.");
  printDeviceConfig();
  playReadySound();
  connectWiFi();
  showReadyScreen();
  printHelp();
}

void loop() {
  handlePendingBleConfig();
  handleButton();
  handleSerialCommands();

  if (volumeMeterEnabled) {
    size_t samplesRead = readPcm16Block(pcmBuffer, DMA_SAMPLES);
    uint32_t level = averageAbs(pcmBuffer, samplesRead);
    printVolumeBar(level);
    updateDisplayVolume(level);
  } else {
    delay(20);
  }
}

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
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
static constexpr bool SPEAKER_ENABLED = true;
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
static constexpr uint16_t COLOR_DARK_GREY = 0x4208;

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

static Adafruit_ST7789 tft =
    Adafruit_ST7789(TFT_CS_PIN, TFT_DC_PIN, TFT_MOSI_PIN, TFT_SCLK_PIN, TFT_RST_PIN);

struct HttpEndpoint {
  String host;
  uint16_t port = 80;
  String path = "/";
};

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
  String ssid = WIFI_SSID;
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
  Serial.println("  v - toggle live volume meter");
  Serial.println("  w - test WiFi and update the screen");
  Serial.println("  b - test speaker beep");
  Serial.println("  r - record 5 seconds and print WAV Base64");
  Serial.println("  s - record 5 seconds and send to STT server");
  Serial.println("Button:");
  Serial.println("  GPIO13 -> GND triggers speech-to-text recording");
  Serial.println();
}

static bool isPlaceholderConfig() {
  return strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0 ||
         strcmp(STT_SERVER_URL, "http://192.168.1.100:8787/stt") == 0;
}

static bool parseHttpUrl(const char *url, HttpEndpoint &endpoint) {
  String value(url);
  const String prefix = "http://";
  if (!value.startsWith(prefix)) {
    Serial.println("Only http:// STT_SERVER_URL is supported by this firmware starter.");
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

  if (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0) {
    Serial.println("WiFi is not configured. Edit include/secrets.h first.");
    wifiEverTested = true;
    wifiIsConnected = false;
    wifiLastRssi = -127;
    wifiLastIp = "-";
    showStatus("wifi config?", ST77XX_RED);
    drawWifiPanel("NO CONFIG", ST77XX_RED, wifiLastRssi, "edit secrets.h");
    return false;
  }

  showStatus("wifi...", ST77XX_YELLOW);
  drawWifiPanel("WIFI...", ST77XX_YELLOW, -127, "connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t startMs = millis();
  uint32_t lastUiMs = 0;
  uint8_t dotCount = 0;
  Serial.printf("Connecting WiFi: %s", WIFI_SSID);
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

static String extractJsonTextField(const String &body) {
  int keyIndex = body.indexOf("\"text\"");
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
    Serial.println("Edit include/secrets.h: set WIFI_SSID, WIFI_PASSWORD, and STT_SERVER_URL.");
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
  if (!parseHttpUrl(STT_SERVER_URL, endpoint)) {
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
                STT_SERVER_URL);
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
  if (text.length() > 0) {
    Serial.print("Transcript: ");
    Serial.println(text);
    showStatus("stt ok", ST77XX_GREEN);
    playOkSound();
  } else {
    showStatus("stt error", ST77XX_RED);
    playErrorSound();
  }

  delay(1500);
  showReadyScreen();
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

  initDisplay();
  initButton();
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
  playReadySound();
  connectWiFi();
  showReadyScreen();
  printHelp();
}

void loop() {
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

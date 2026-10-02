#include "SmartWatchApp.h"

#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Preferences.h>
#include <Wire.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include "driver/i2s.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"

#include "es8311.h"

static const char *BOOK_DIR = "/books";
static const char *PROGRESS_DIR = "/progress";
static const char *FONT_DIR = "/fonts";
static const char *ALARM_FILE = "/alarms.csv";
static const char *STATE_FILE = "/watch_state.txt";
static const char *AP_SSID = "BBWF";
static const char *AP_PASS = "";
static const char *BLE_CONFIG_NAME = "BBWF Watch";
static const char *BLE_SERVICE_UUID = "6f4d0001-6f6f-4f20-8677-626277660001";
static const char *BLE_STATUS_UUID = "6f4d0002-6f6f-4f20-8677-626277660001";
static const char *BLE_COMMAND_UUID = "6f4d0003-6f6f-4f20-8677-626277660001";
static const uint32_t MAIN_MENU_IDLE_MS = 5000;
static const uint32_t MENU_IDLE_MS = 15000;
static const uint32_t WIFI_IDLE_MS = 120000;
static const uint32_t BLE_IDLE_MS = 300000;
static const uint32_t HEART_RATE_INTERVAL_MS = 10UL * 60UL * 1000UL;
static const uint32_t HEART_RATE_FIRST_SAMPLE_MS = 15000;
static const uint32_t TIME_SNAPSHOT_INTERVAL_MS = 5UL * 60UL * 1000UL;
static const uint32_t NTP_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;
static const uint8_t MAX30102_ADDR = 0x57;
static const uint32_t AUDIO_SAMPLE_RATE = 16000;

#define GBK(s) String(s)

static ES8311 audioCodec;

class WatchBleServerCallbacks : public BLEServerCallbacks {
public:
  explicit WatchBleServerCallbacks(SmartWatchApp *app) : app(app) {}

  void onConnect(BLEServer *server) override {
    if (app) app->onBleClientChanged(true);
  }

  void onDisconnect(BLEServer *server) override {
    if (app) app->onBleClientChanged(false);
  }

private:
  SmartWatchApp *app;
};

class WatchBleCommandCallbacks : public BLECharacteristicCallbacks {
public:
  explicit WatchBleCommandCallbacks(SmartWatchApp *app) : app(app) {}

  void onWrite(BLECharacteristic *characteristic) override {
    if (!app) return;
    String command = String(characteristic->getValue().c_str());
    app->handleBleCommand(command);
  }

private:
  SmartWatchApp *app;
};

SmartWatchApp::SmartWatchApp(const watchySettings &s)
    : Watchy(s), page(PAGE_WATCHFACE), previousPage(PAGE_WATCHFACE),
      selectedIndex(0), menuTopIndex(0), readerFontSize(14), selectedFontIndex(0),
      currentBookIndex(-1), sdReady(false), readingActive(false),
      wifiPortalActive(false), wifiApStarted(false), wifiLanActive(false), bleConfigActive(false), bleClientConnected(false),
      audioReady(false), max30102Ready(false), bmaReady(false), peripheralsSuspendedForWifi(false),
      lastInteractionMs(0), wifiStartedMs(0), bleStartedMs(0), lastHeartRateMs(0), lastLanReconnectMs(0),
      lastTimeSaveMs(0), lastNtpSyncMs(0), ntpSyncedThisBoot(false),
      heartRate(0), spo2(0), lastStepCount(0), lastStepResetDay(0),
      wifiClientSsid(""), wifiClientPass(""), bleStatusText("Idle"), bleLastCommand(""),
      bleUploadName(""), bleUploadExpectedBytes(0), bleUploadReceivedBytes(0),
      bookCount(0), chapterCount(0), alarmCount(0), fontCount(0), server(80),
      bleServer(nullptr), bleService(nullptr), bleStatusCharacteristic(nullptr), bleCommandCharacteristic(nullptr),
      lastBackState(true), lastMenuState(true),
      lastUpState(true), lastDownState(true), lastRtcReadMs(0), lastDrawnMinute(-1) {}

void SmartWatchApp::init(String datetime) {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("[Boot] SmartWatchFirmware starting");
  Serial.flush();
#ifdef ARDUINO_ESP32S3_DEV
  Wire.begin(WATCHY_V3_SDA, WATCHY_V3_SCL);
#else
  Wire.begin(SDA, SCL);
#endif
  RTC.init();
  if (datetime.length() > 0) {
    RTC.config(datetime);
  }
  RTC.read(currentTime);

  pinMode(BACK_BTN_PIN, INPUT_PULLUP);
  pinMode(MENU_BTN_PIN, INPUT_PULLUP);
  pinMode(UP_BTN_PIN, INPUT_PULLUP);
  pinMode(DOWN_BTN_PIN, INPUT_PULLUP);
  pinMode(BATT_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BATT_ADC_PIN, ADC_11db);

  bootStorage();
  loadAppState();
  restoreSavedTimeIfNeeded();
  display.epd2.initWatchy();
  loadAlarms();
  scanBooks();
  scanFonts();
  bootMotion();
  bootAudio();
  bootHeartRate();
  resetStepsIfNeeded();
  resetIdleTimer();
  redraw(false);
  Serial.println("[Boot] init complete");
  Serial.flush();
}

void SmartWatchApp::tick() {
  pollButtons();
  if (millis() - lastRtcReadMs > 1000) {
    RTC.read(currentTime);
    saveCurrentTimeSnapshot();
    lastRtcReadMs = millis();
    int16_t minuteKey = (int16_t)currentTime.Hour * 60 + currentTime.Minute;
    if (page == PAGE_WATCHFACE && minuteKey != lastDrawnMinute) redraw(true);
  }
  serviceTimers();
  delay(20);
}

void SmartWatchApp::handleButtonPress() {
  uint64_t wakeupBit = esp_sleep_get_ext1_wakeup_status();

  if (wakeupBit & BACK_BTN_MASK) {
    pressBack();
  } else if (wakeupBit & MENU_BTN_MASK) {
    pressSelect();
  } else if (wakeupBit & UP_BTN_MASK) {
    pressUp();
  } else if (wakeupBit & DOWN_BTN_MASK) {
    pressDown();
  }

  resetIdleTimer();
  serviceTimers();
}

void SmartWatchApp::pollButtons() {
  bool backState = digitalRead(BACK_BTN_PIN);
  bool menuState = digitalRead(MENU_BTN_PIN);
  bool upState = digitalRead(UP_BTN_PIN);
  bool downState = digitalRead(DOWN_BTN_PIN);

  if (!backState && lastBackState) {
    pressBack();
    resetIdleTimer();
  } else if (!menuState && lastMenuState) {
    pressSelect();
    resetIdleTimer();
  } else if (!upState && lastUpState) {
    pressUp();
    resetIdleTimer();
  } else if (!downState && lastDownState) {
    pressDown();
    resetIdleTimer();
  }

  lastBackState = backState;
  lastMenuState = menuState;
  lastUpState = upState;
  lastDownState = downState;
}

void SmartWatchApp::drawWatchFace() {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);

  const char *weekdays[] = {
      "\xD0\xC7\xC6\xDA\xC8\xD5",
      "\xD0\xC7\xC6\xDA\xD2\xBB",
      "\xD0\xC7\xC6\xDA\xB6\xFE",
      "\xD0\xC7\xC6\xDA\xC8\xFD",
      "\xD0\xC7\xC6\xDA\xCB\xC4",
      "\xD0\xC7\xC6\xDA\xCE\xE5",
      "\xD0\xC7\xC6\xDA\xC1\xF9"};
  drawUiText(GBK(weekdays[constrain(currentTime.Wday - 1, 0, 6)]), 8, 26);
  uint8_t level = batteryPercent();
  display.drawRect(160, 10, 32, 15, GxEPD_BLACK);
  display.fillRect(192, 14, 3, 7, GxEPD_BLACK);
  uint8_t fillWidth = map(level, 0, 100, 0, 28);
  if (fillWidth > 0) display.fillRect(162, 12, fillWidth, 11, GxEPD_BLACK);
  display.fillRect(0, 43, 200, 2, GxEPD_BLACK);

  display.setFont(&DSEG7_Classic_Bold_53);
  display.setCursor(7, 105);
  if (currentTime.Hour < 10) display.print("0");
  display.print(currentTime.Hour);
  display.print(":");
  if (currentTime.Minute < 10) display.print("0");
  display.print(currentTime.Minute);

  display.setFont(&FreeSansBold12pt7b);
  String dateText = String(tmYearToCalendar(currentTime.Year)) + "/" + String(currentTime.Month) + "/" + String(currentTime.Day);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(dateText, 0, 137, &x1, &y1, &w, &h);
  display.setCursor((200 - w) / 2, 137);
  display.print(dateText);

  display.drawBitmap(8, 151, steps, 19, 23, GxEPD_BLACK);
  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(32, 170);
  display.print(currentStepCount());

  display.drawBitmap(117, 151, heart_icon, 20, 20, GxEPD_BLACK);
  display.setCursor(140, 170);
  display.print(heartRate > 0 ? String(heartRate) : "--");
}

void SmartWatchApp::bootStorage() {
  pinMode(40, OUTPUT);
  digitalWrite(40, LOW);
  SD_MMC.setPins(4, 3, 5);
  sdReady = SD_MMC.begin("/sdcard", true);
  if (!sdReady) return;
  if (!SD_MMC.exists(BOOK_DIR)) SD_MMC.mkdir(BOOK_DIR);
  if (!SD_MMC.exists(PROGRESS_DIR)) SD_MMC.mkdir(PROGRESS_DIR);
  if (!SD_MMC.exists(FONT_DIR)) SD_MMC.mkdir(FONT_DIR);
}

void SmartWatchApp::bootMotion() {
#if BMA_ENABLED
  bmaReady = sensor.begin(
      [](uint8_t address, uint8_t reg, uint8_t *data, uint16_t len) -> uint16_t {
        Wire.beginTransmission(address);
        Wire.write(reg);
        if (Wire.endTransmission()) return 1;
        Wire.requestFrom(address, (uint8_t)len);
        uint16_t index = 0;
        while (Wire.available() && index < len) data[index++] = Wire.read();
        return index == len ? 0 : 1;
      },
      [](uint8_t address, uint8_t reg, uint8_t *data, uint16_t len) -> uint16_t {
        Wire.beginTransmission(address);
        Wire.write(reg);
        Wire.write(data, len);
        return Wire.endTransmission();
      },
      delay);
  if (!bmaReady) {
    Serial.println("[BMA423] init failed");
    return;
  }

  Acfg cfg;
  cfg.odr = BMA4_OUTPUT_DATA_RATE_100HZ;
  cfg.range = BMA4_ACCEL_RANGE_2G;
  cfg.bandwidth = BMA4_ACCEL_NORMAL_AVG4;
  cfg.perf_mode = BMA4_CONTINUOUS_MODE;
  sensor.setAccelConfig(cfg);
  sensor.enableAccel();
  sensor.enableFeature(BMA423_STEP_CNTR, true);
  sensor.enableStepCountInterrupt();
  lastStepCount = sensor.getCounter();
  Serial.printf("[BMA423] initialized steps=%lu\n", (unsigned long)lastStepCount);
#else
  Serial.println("[BMA423] disabled at compile time");
#endif
}

void SmartWatchApp::bootAudio() {
  audioReady = audioCodec.begin(WATCHY_V3_SDA, WATCHY_V3_SCL);
  if (!audioReady) return;
  audioCodec.setVolume(100);
  audioCodec.setSampleRate(AUDIO_SAMPLE_RATE);
  audioCodec.setBitsPerSample(16);
  audioCodec.setMode(true);

  i2s_config_t i2sConfig = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = AUDIO_SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 256,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0};
  i2s_pin_config_t pinConfig = {
      .mck_io_num = AUDIO_I2S_MCLK,
      .bck_io_num = AUDIO_I2S_BCLK,
      .ws_io_num = AUDIO_I2S_WS,
      .data_out_num = AUDIO_I2S_DOUT,
      .data_in_num = I2S_PIN_NO_CHANGE};
  audioReady = i2s_driver_install(I2S_NUM_0, &i2sConfig, 0, NULL) == ESP_OK &&
               i2s_set_pin(I2S_NUM_0, &pinConfig) == ESP_OK;
  pinMode(AUDIO_CODEC_PA_PIN, OUTPUT);
  digitalWrite(AUDIO_CODEC_PA_PIN, LOW);
}

void SmartWatchApp::bootHeartRate() {
  Wire.beginTransmission(MAX30102_ADDR);
  max30102Ready = Wire.endTransmission() == 0;
  if (!max30102Ready) {
    Serial.println("[MAX30102] not found");
    return;
  }

  writeMax30102Reg(0x09, 0x40);
  delay(100);
  writeMax30102Reg(0x04, 0x00);
  writeMax30102Reg(0x05, 0x00);
  writeMax30102Reg(0x06, 0x00);
  writeMax30102Reg(0x08, 0x4F);
  writeMax30102Reg(0x09, 0x03);
  writeMax30102Reg(0x0A, 0x27);
  writeMax30102Reg(0x0C, 0x24);
  writeMax30102Reg(0x0D, 0x24);
  lastHeartRateMs = millis() - HEART_RATE_INTERVAL_MS + HEART_RATE_FIRST_SAMPLE_MS;
  Serial.println("[MAX30102] initialized");
}

void SmartWatchApp::scanBooks() {
  bookCount = 0;
  if (!sdReady) return;

  File root = SD_MMC.open(BOOK_DIR);
  if (!root || !root.isDirectory()) return;

  File file = root.openNextFile();
  while (file && bookCount < MAX_BOOKS) {
    String name = String(file.name());
    if (!file.isDirectory() && isTxtFile(name)) {
      books[bookCount].name = name.substring(name.lastIndexOf('/') + 1);
      books[bookCount].path = String(BOOK_DIR) + "/" + books[bookCount].name;
      books[bookCount].pageCount = max<uint32_t>(1, (file.size() + charsPerPage() - 1) / charsPerPage());
      books[bookCount].pageIndex = 0;
      loadBookProgress(books[bookCount]);
      bookCount++;
    }
    file = root.openNextFile();
  }
}

void SmartWatchApp::scanFonts() {
  fontCount = 0;
  fonts[fontCount++] = {"Built-in", ""};
  if (!sdReady) return;

  File root = SD_MMC.open(FONT_DIR);
  if (!root || !root.isDirectory()) return;

  File file = root.openNextFile();
  while (file && fontCount < MAX_FONTS) {
    String name = String(file.name());
    if (!file.isDirectory() && isFontFile(name)) {
      fonts[fontCount].name = name.substring(name.lastIndexOf('/') + 1);
      fonts[fontCount].path = String(FONT_DIR) + "/" + fonts[fontCount].name;
      fontCount++;
    }
    file = root.openNextFile();
  }
  if (selectedFontIndex >= fontCount) selectedFontIndex = 0;
  if (selectedFontIndex == 0 && fontCount > 1) {
    selectedFontIndex = 1;
    saveAppState();
    Serial.printf("[Fonts] auto selected %s for Chinese text\n", fonts[selectedFontIndex].path.c_str());
  }
}

void SmartWatchApp::openBook(uint8_t index) {
  if (index >= bookCount) return;
  currentBookIndex = index;
  readingActive = true;
  chapterCount = 0;
  chapters[chapterCount++] = {"Start", 0};
  showPage(PAGE_READER);
}

void SmartWatchApp::loadBookProgress(BookEntry &book) {
  File file = SD_MMC.open(bookProgressPath(book), FILE_READ);
  if (!file) return;
  book.pageIndex = min<uint32_t>(file.readStringUntil('\n').toInt(), book.pageCount - 1);
  file.close();
}

void SmartWatchApp::saveBookProgress(const BookEntry &book) {
  if (!sdReady) return;
  File file = SD_MMC.open(bookProgressPath(book), FILE_WRITE);
  if (!file) return;
  file.println(book.pageIndex);
  file.close();
}

void SmartWatchApp::loadAppState() {
  Preferences prefs;
  prefs.begin("bbwf", false);
  readerFontSize = prefs.getUChar("fontSize", readerFontSize);
  selectedFontIndex = prefs.getUChar("fontIndex", selectedFontIndex);
  lastStepResetDay = prefs.getUChar("stepDay", 0);
  wifiClientSsid = prefs.getString("wifiSsid", "");
  wifiClientPass = prefs.getString("wifiPass", "");
  prefs.end();

  if (!sdReady || !SD_MMC.exists(STATE_FILE)) return;
  File file = SD_MMC.open(STATE_FILE, FILE_READ);
  if (!file) return;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    int split = line.indexOf('=');
    if (split < 0) continue;
    String key = line.substring(0, split);
    String value = line.substring(split + 1);
    if (key == "fontSize") readerFontSize = constrain(value.toInt(), 10, 24);
    if (key == "fontIndex") selectedFontIndex = value.toInt();
    if (key == "wifiSsid") wifiClientSsid = value;
    if (key == "wifiPass") wifiClientPass = value;
  }
  file.close();
}

void SmartWatchApp::saveAppState() {
  Preferences prefs;
  prefs.begin("bbwf", false);
  prefs.putUChar("fontSize", readerFontSize);
  prefs.putUChar("fontIndex", selectedFontIndex);
  prefs.putUChar("stepDay", lastStepResetDay);
  prefs.putString("wifiSsid", wifiClientSsid);
  prefs.putString("wifiPass", wifiClientPass);
  prefs.end();

  if (!sdReady) return;
  File file = SD_MMC.open(STATE_FILE, FILE_WRITE);
  if (!file) return;
  file.println("fontSize=" + String(readerFontSize));
  file.println("fontIndex=" + String(selectedFontIndex));
  file.println("wifiSsid=" + wifiClientSsid);
  file.println("wifiPass=" + wifiClientPass);
  file.close();
}

bool SmartWatchApp::isValidTime(const tmElements_t &tm) const {
  uint16_t year = tmYearToCalendar(tm.Year);
  return year >= 2024 && year <= 2099 &&
         tm.Month >= 1 && tm.Month <= 12 &&
         tm.Day >= 1 && tm.Day <= 31 &&
         tm.Hour <= 23 && tm.Minute <= 59 && tm.Second <= 59;
}

void SmartWatchApp::restoreSavedTimeIfNeeded() {
  if (isValidTime(currentTime)) return;

  Preferences prefs;
  prefs.begin("bbwf", false);
  uint32_t saved = prefs.getUInt("lastUnix", 0);
  prefs.end();

  if (saved < 1704067200UL) {
    Serial.println("[Time] RTC invalid and no saved time snapshot");
    return;
  }

  tmElements_t tm;
  breakTime((time_t)saved, tm);
  RTC.set(tm);
  RTC.read(currentTime);
  bleStatusText = "Time restored";
  Serial.printf("[Time] restored saved timestamp=%lu\n", (unsigned long)saved);
}

void SmartWatchApp::saveCurrentTimeSnapshot(bool force) {
  if (!isValidTime(currentTime)) return;
  if (!force && millis() - lastTimeSaveMs < TIME_SNAPSHOT_INTERVAL_MS) return;

  time_t timestamp = makeTime(currentTime);
  if (timestamp < 1704067200UL) return;

  Preferences prefs;
  prefs.begin("bbwf", false);
  prefs.putUInt("lastUnix", (uint32_t)timestamp);
  prefs.end();
  lastTimeSaveMs = millis();
}

bool SmartWatchApp::syncNetworkTime() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (ntpSyncedThisBoot && millis() - lastNtpSyncMs < NTP_RESYNC_INTERVAL_MS) return true;

  bool ok = syncNTP(settings.gmtOffset, settings.ntpServer);
  if (!ok) {
    bleStatusText = "NTP sync failed";
    Serial.println("[Time] NTP sync failed");
    return false;
  }

  RTC.read(currentTime);
  saveCurrentTimeSnapshot(true);
  ntpSyncedThisBoot = true;
  lastNtpSyncMs = millis();
  bleStatusText = "Time synced";
  Serial.printf("[Time] NTP synced %04d-%02d-%02d %02d:%02d:%02d\n",
                tmYearToCalendar(currentTime.Year), currentTime.Month, currentTime.Day,
                currentTime.Hour, currentTime.Minute, currentTime.Second);
  return true;
}

void SmartWatchApp::loadAlarms() {
  alarmCount = 0;
  if (sdReady && SD_MMC.exists(ALARM_FILE)) {
    File file = SD_MMC.open(ALARM_FILE, FILE_READ);
    while (file && file.available() && alarmCount < MAX_ALARMS) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (!line.length()) continue;
      int first = line.indexOf(',');
      int second = line.indexOf(',', first + 1);
      if (first < 0 || second < 0) continue;
      alarms[alarmCount++] = {
          (uint8_t)constrain(line.substring(0, first).toInt(), 0, 23),
          (uint8_t)constrain(line.substring(first + 1, second).toInt(), 0, 59),
          line.substring(second + 1).toInt() != 0,
          false};
    }
    if (file) file.close();
  }

  if (alarmCount == 0) {
    alarms[0] = {7, 30, true, false};
    alarms[1] = {22, 0, false, false};
    alarmCount = 2;
    saveAlarms();
  }
}

void SmartWatchApp::saveAlarms() {
  if (!sdReady) return;
  File file = SD_MMC.open(ALARM_FILE, FILE_WRITE);
  if (!file) return;
  for (uint8_t i = 0; i < alarmCount; i++) {
    file.print(alarms[i].hour);
    file.print(",");
    file.print(alarms[i].minute);
    file.print(",");
    file.println(alarms[i].enabled ? 1 : 0);
  }
  file.close();
}

String SmartWatchApp::bookProgressPath(const BookEntry &book) const {
  String clean = book.name;
  clean.replace("/", "_");
  clean.replace("\\", "_");
  return String(PROGRESS_DIR) + "/" + clean + ".pos";
}

void SmartWatchApp::scanChapters() {
  chapterCount = 0;
  if (currentBookIndex < 0 || !sdReady) return;
  File file = SD_MMC.open(books[currentBookIndex].path, FILE_READ);
  if (!file) return;

  uint32_t page = 0;
  uint32_t chars = 0;
  const uint32_t maxScanBytes = 512UL * 1024UL;
  while (file.available() && chapterCount < MAX_CHAPTERS && file.position() < maxScanBytes) {
    String line = file.readStringUntil('\n');
    chars += line.length() + 1;
    page = chars / charsPerPage();
    if (looksLikeChapter(line)) {
      line.trim();
      chapters[chapterCount++] = {line.substring(0, 18), page};
    }
  }
  file.close();
  if (chapterCount == 0) chapters[chapterCount++] = {"Start", 0};
}

String SmartWatchApp::readPageText(BookEntry &book, uint32_t pageIndex) {
  if (!sdReady) return "SD card not ready";
  File file = SD_MMC.open(book.path, FILE_READ);
  if (!file) return "Cannot open book";
  uint32_t offset = safePageOffset(book, pageIndex);
  file.seek(offset);
  uint16_t maxBytes = charsPerPage() * 4 + 96;
  String text;
  text.reserve(maxBytes + 1);
  while (file.available() && text.length() < maxBytes) {
    text += (char)file.read();
  }
  file.close();
  if (pageIndex == 0 && text.length() >= 3 &&
      (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
    text = text.substring(3);
  }
  text.replace("\r", "");
  text = text.substring(0, safeTextLimit(text));
  return text;
}

uint16_t SmartWatchApp::charsPerPage() const {
  const uint8_t cjkCellWidth = 17;
  const uint8_t lineHeight = max<uint8_t>(20, readerFontSize + 6);
  uint8_t charsPerLine = max<uint8_t>(10, 176 / cjkCellWidth);
  uint8_t lines = max<uint8_t>(5, 138 / lineHeight);
  return charsPerLine * lines;
}

uint32_t SmartWatchApp::safePageOffset(const BookEntry &book, uint32_t pageIndex) {
  if (pageIndex == 0) return 0;
  uint32_t offset = pageIndex * charsPerPage();
  File file = SD_MMC.open(book.path, FILE_READ);
  if (!file) return offset;
  uint32_t size = file.size();
  if (offset >= size) {
    file.close();
    return size;
  }
  // 小说文件按 GBK 保存并用 HZK16 字库渲染，分页偏移不能落在中文双字节中间。
  // 从附近换行位置重新扫描，可以保持字节边界正确，避免下一页开头乱码。
  uint32_t anchor = 0;
  uint32_t scanStart = offset > 2048 ? offset - 2048 : 0;
  file.seek(scanStart);
  for (uint32_t pos = scanStart; pos < offset && file.available(); pos++) {
    uint8_t value = file.read();
    if (value == '\n' || value == '\r') anchor = pos + 1;
  }

  file.seek(anchor);
  uint32_t pos = anchor;
  while (pos < offset && file.available()) {
    uint32_t charStart = pos;
    uint8_t value = file.read();
    pos++;
    if (value < 0x80) continue;
    if (isGbkLead(value) && pos < size) {
      uint8_t trail = file.read();
      if (isGbkTrail(trail)) {
        if (pos >= offset) {
          offset = charStart;
          break;
        }
        pos++;
      }
    }
  }
  file.close();
  return offset;
}

uint16_t SmartWatchApp::safeTextLimit(const String &text) const {
  uint16_t limit = min<uint16_t>(text.length(), charsPerPage() * 3);
  uint16_t safe = 0;
  // 截断当前页内容时只停在有效 GBK 字符边界，避免页尾中文被切成半个字。
  for (uint16_t i = 0; i < limit;) {
    uint8_t value = text[i];
    if (value < 0x80) {
      safe = ++i;
    } else if (isGbkPair(text, i)) {
      if (i + 2 > limit) break;
      i += 2;
      safe = i;
    } else {
      safe = ++i;
    }
  }
  return safe;
}

uint8_t SmartWatchApp::utf8GlyphWidth(const String &text, uint16_t index) const {
  uint8_t value = text[index];
  if (value < 0x80) return 1;
  if ((value & 0xE0) == 0xC0) return 2;
  if ((value & 0xF0) == 0xE0) return 3;
  if ((value & 0xF8) == 0xF0) return 4;
  return 1;
}

uint16_t SmartWatchApp::nextUtf8Index(const String &text, uint16_t index) const {
  return min<uint16_t>(text.length(), index + utf8GlyphWidth(text, index));
}

bool SmartWatchApp::isGbkLead(uint8_t value) const {
  return value >= 0x81 && value <= 0xFE;
}

bool SmartWatchApp::isGbkTrail(uint8_t value) const {
  return value >= 0x40 && value <= 0xFE && value != 0x7F;
}

bool SmartWatchApp::isGbkPair(const String &text, uint16_t index) const {
  return index + 1 < text.length() &&
         isGbkLead((uint8_t)text[index]) &&
         isGbkTrail((uint8_t)text[index + 1]);
}

bool SmartWatchApp::textLooksUtf8(const String &text) const {
  uint16_t valid = 0;
  uint16_t invalid = 0;
  for (uint16_t i = 0; i < text.length();) {
    uint8_t c = text[i];
    if (c < 0x80) {
      i++;
      continue;
    }
    uint8_t width = 0;
    if ((c & 0xE0) == 0xC0) width = 2;
    else if ((c & 0xF0) == 0xE0) width = 3;
    else if ((c & 0xF8) == 0xF0) width = 4;
    else {
      invalid++;
      i++;
      continue;
    }
    if (i + width > text.length()) {
      invalid++;
      break;
    }
    bool ok = true;
    for (uint8_t j = 1; j < width; j++) {
      if (((uint8_t)text[i + j] & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    if (ok) {
      valid++;
      i += width;
    } else {
      invalid++;
      i++;
    }
  }
  return valid > 0 && invalid == 0;
}

bool SmartWatchApp::textHasNonAscii(const String &text) const {
  for (uint16_t i = 0; i < text.length(); i++) {
    if ((uint8_t)text[i] >= 0x80) return true;
  }
  return false;
}

uint8_t SmartWatchApp::batteryPercent() const {
  float voltage = const_cast<SmartWatchApp *>(this)->getBatteryVoltage();
  if (voltage < 1.0f || voltage > 5.2f) return 0;
  if (voltage >= 4.18f) return 100;
  if (voltage <= 3.30f) return 0;
  uint8_t percent = (uint8_t)constrain((int)((voltage - 3.30f) * 100.0f / (4.18f - 3.30f)), 0, 100);
  return percent;
}

bool SmartWatchApp::isTxtFile(const String &name) const {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".txt");
}

bool SmartWatchApp::isFontFile(const String &name) const {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".bin") || lower.endsWith(".hzk") || lower.endsWith(".fon");
}

bool SmartWatchApp::looksLikeChapter(const String &line) const {
  String lower = line;
  lower.toLowerCase();
  if (lower.indexOf("chapter") >= 0) return true;
  const char utf8Di[] = "\xE7\xAC\xAC";
  const char utf8Zhang[] = "\xE7\xAB\xA0";
  int di = line.indexOf(utf8Di);
  int zhang = line.indexOf(utf8Zhang);
  return di >= 0 && zhang > di;
}

void SmartWatchApp::refreshCurrentBookPageCount() {
  if (currentBookIndex < 0 || !sdReady) return;
  File file = SD_MMC.open(books[currentBookIndex].path, FILE_READ);
  if (!file) return;
  books[currentBookIndex].pageCount = max<uint32_t>(1, (file.size() + charsPerPage() - 1) / charsPerPage());
  books[currentBookIndex].pageIndex = min<uint32_t>(books[currentBookIndex].pageIndex, books[currentBookIndex].pageCount - 1);
  file.close();
}

void SmartWatchApp::showPage(AppPage nextPage, bool partial) {
  previousPage = page;
  page = nextPage;
  selectedIndex = 0;
  menuTopIndex = 0;
  redraw(partial);
}

void SmartWatchApp::redraw(bool partial) {
  if (page == PAGE_WATCHFACE) {
    lastDrawnMinute = (int16_t)currentTime.Hour * 60 + currentTime.Minute;
    showWatchFace(partial);
    return;
  }

  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeMono9pt7b);

  switch (page) {
    case PAGE_MAIN_MENU: {
      String items[] = {
          GBK("\xB5\xE7\xD7\xD3\xCA\xE9"),
          GBK("WiFi\xC8\xC8\xB5\xE3"),
          GBK("\xC0\xB6\xD1\xC0\xC9\xE8\xD6\xC3"),
          GBK("\xC4\xD6\xD6\xD3"),
          GBK("\xCF\xB5\xCD\xB3\xC9\xE8\xD6\xC3")};
      String footer = sdReady ? GBK("SD\xD2\xD1\xBE\xCD\xD0\xF7  \xCA\xE9\xBC\xAE:") + String(bookCount)
                              : GBK("SD\xBF\xA8\xCE\xB4\xBE\xCD\xD0\xF7");
      drawMenu(GBK("\xCF\xB5\xCD\xB3\xB2\xCB\xB5\xA5"), items, 5, footer);
      break;
    }
    case PAGE_BOOK_LIST: drawBookList(); break;
    case PAGE_READER: drawReader(); break;
    case PAGE_READER_MENU: drawReaderMenu(); break;
    case PAGE_TOC: drawToc(); break;
    case PAGE_WIFI: drawWifi(); break;
    case PAGE_ALARMS: drawAlarms(); break;
    case PAGE_SETTINGS: drawSettings(); break;
    case PAGE_FONT_SIZE: drawFontSize(); break;
    case PAGE_FONT_SELECT: drawFontSelect(); break;
    case PAGE_STORAGE: drawStorage(); break;
    case PAGE_BLE: drawBle(); break;
    default: break;
  }
  display.display(partial);
}

void SmartWatchApp::drawHeader(const String &title) {
  display.fillRect(0, 0, 200, 24, GxEPD_BLACK);
  drawUiText(title, 6, 18, GxEPD_WHITE);
}

void SmartWatchApp::drawMenu(const String &title, const String items[], uint8_t count, const String &footer) {
  drawHeader(title);
  const uint8_t visible = footer.length() ? 5 : 6;
  if (selectedIndex < menuTopIndex) menuTopIndex = selectedIndex;
  if (selectedIndex >= menuTopIndex + visible) menuTopIndex = selectedIndex - visible + 1;

  for (uint8_t row = 0; row < visible && row + menuTopIndex < count; row++) {
    uint8_t idx = row + menuTopIndex;
    int16_t y = 52 + row * 24;
    if (idx == selectedIndex) {
      display.fillRect(0, y - 16, 200, 22, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
    }
    drawUiText(items[idx], 10, y, idx == selectedIndex ? GxEPD_WHITE : GxEPD_BLACK);
  }
  if (footer.length()) {
    display.drawLine(0, 176, 200, 176, GxEPD_BLACK);
    drawUiText(footer, 6, 194);
  }
}

void SmartWatchApp::drawBookList() {
  drawHeader(GBK("\xCA\xE9\xBC\xAE"));
  if (!sdReady) {
    drawUiText(GBK("SD\xBF\xA8\xCE\xB4\xBE\xCD\xD0\xF7"), 12, 70);
    return;
  }
  if (bookCount == 0) {
    drawUiText(GBK("\xC7\xEB\xB7\xC5TXT\xB5\xBD/books"), 12, 70);
    return;
  }
  String items[MAX_BOOKS];
  for (uint8_t i = 0; i < bookCount; i++) items[i] = books[i].name;
  drawMenu(GBK("\xCA\xE9\xBC\xAE"), items, bookCount);
}

void SmartWatchApp::drawReader() {
  if (currentBookIndex < 0) {
    drawHeader(GBK("\xD4\xC4\xB6\xC1\xC6\xF7"));
    drawUiText(GBK("\xCE\xB4\xB4\xF2\xBF\xAA\xCA\xE9\xBC\xAE"), 12, 70);
    return;
  }

  BookEntry &book = books[currentBookIndex];
  String text = readPageText(book, book.pageIndex);
  display.setFont(&FreeMono9pt7b);
  display.setCursor(6, 14);
  display.print(book.name.substring(0, 20));
  display.drawLine(0, 20, 200, 20, GxEPD_BLACK);

  drawReaderText(text, 6, 38, 176);

  if (textHasNonAscii(text) && selectedFontIndex == 0) {
    display.fillRect(0, 160, 200, 19, GxEPD_WHITE);
    drawUiText(GBK("\xD0\xE8\xD2\xAAHZK16\xD7\xD6\xBF\xE2"), 6, 175);
    Serial.println("[Reader] Chinese text needs HZK16/GBK font in SD /fonts");
  }

  display.drawLine(0, 180, 200, 180, GxEPD_BLACK);
  display.setCursor(6, 197);
  display.print(twoDigits(currentTime.Hour));
  display.print(":");
  display.print(twoDigits(currentTime.Minute));
  // 页数很多时进度文字会变长，先测量宽度再靠右绘制，防止超出 200px 屏幕。
  String pageText = String(book.pageIndex + 1) + "/" + String(book.pageCount);
  if (pageText.length() > 11) pageText = String(book.pageIndex + 1);
  uint16_t percent = ((book.pageIndex + 1) * 100) / max<uint32_t>(1, book.pageCount);
  String progressText = String(percent) + "% " + pageText;
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(progressText, 0, 197, &x1, &y1, &w, &h);
  display.setCursor(max<int16_t>(74, 198 - w), 197);
  display.print(progressText);
}

void SmartWatchApp::drawReaderText(const String &text, int16_t x, int16_t y, int16_t maxY) {
  display.setFont(&FreeMono9pt7b);
  int16_t cursorX = x;
  int16_t cursorY = y;
  const uint8_t cjkGlyphSize = 16;
  const uint8_t cjkCellWidth = 17;
  uint8_t lineHeight = max<uint8_t>(20, readerFontSize + 6);
  uint8_t asciiAdvance = max<uint8_t>(7, readerFontSize / 2 + 2);
  uint8_t cjkAdvance = max<uint8_t>(cjkCellWidth, readerFontSize + 1);

  for (uint16_t i = 0; i < text.length() && cursorY < maxY;) {
    uint8_t c = text[i];
    if (c == '\n') {
      cursorX = x;
      cursorY += lineHeight;
      i++;
      continue;
    }

    // 这里不要自动判断 UTF-8：某些 GBK 字节组合在单页里也可能像合法 UTF-8。
    // App 上传前已经把小说转成 GBK，所以阅读器固定按 GBK 解析更稳定。
    bool gbkPair = isGbkPair(text, i);
    uint8_t width = gbkPair ? 2 : utf8GlyphWidth(text, i);
    uint8_t advance = (width == 1 && c < 0x80) ? asciiAdvance : cjkAdvance;
    if (cursorX + advance > 194) {
      cursorX = x;
      cursorY += lineHeight;
      if (cursorY >= maxY) break;
    }

    if (width == 1 && c < 0x80) {
      display.setCursor(cursorX, cursorY);
      display.print((char)c);
    } else if (selectedFontIndex > 0 && gbkPair) {
      int16_t glyphY = cursorY - cjkGlyphSize + 2;
      if (!drawGbkGlyph((uint8_t)text[i], (uint8_t)text[i + 1], cursorX, glyphY)) {
        display.drawRect(cursorX, glyphY, cjkGlyphSize, cjkGlyphSize, GxEPD_BLACK);
      }
    } else {
      display.drawRect(cursorX, cursorY - cjkGlyphSize + 2, cjkGlyphSize, cjkGlyphSize, GxEPD_BLACK);
    }
    cursorX += advance;
    i = gbkPair ? i + 2 : nextUtf8Index(text, i);
  }
}

void SmartWatchApp::drawUiText(const String &text, int16_t x, int16_t baseline, uint16_t color) {
  display.setTextColor(color);
  display.setFont(&FreeMono9pt7b);
  int16_t cursorX = x;
  for (uint16_t i = 0; i < text.length();) {
    uint8_t c = text[i];
    if (c < 0x80) {
      display.setCursor(cursorX, baseline);
      display.print((char)c);
      cursorX += 9;
      i++;
    } else if (isGbkPair(text, i)) {
      drawGbkGlyph((uint8_t)text[i], (uint8_t)text[i + 1], cursorX, baseline - 15, color);
      cursorX += 17;
      i += 2;
    } else {
      cursorX += 9;
      i++;
    }
  }
  display.setTextColor(GxEPD_BLACK);
}

bool SmartWatchApp::drawGbkGlyph(uint8_t high, uint8_t low, int16_t x, int16_t y, uint16_t color) {
  if (selectedFontIndex == 0 || selectedFontIndex >= fontCount || !sdReady) return false;
  if (!isGbkLead(high) || !isGbkTrail(low)) return false;

  File font = SD_MMC.open(fonts[selectedFontIndex].path, FILE_READ);
  if (!font) return false;

  uint32_t offset = 0;
  if (high >= 0xA1 && high <= 0xFE && low >= 0xA1 && low <= 0xFE) {
    offset = ((uint32_t)(high - 0xA1) * 94 + (low - 0xA1)) * 32UL;
  } else {
    uint32_t index = (uint32_t)(high - 0x81) * 190 + (low >= 0x80 ? low - 0x41 : low - 0x40);
    offset = index * 32UL;
  }

  if (offset + 32 > font.size()) {
    font.close();
    return false;
  }
  font.seek(offset);
  uint8_t bitmap[32];
  font.read(bitmap, 32);
  font.close();

  for (uint8_t row = 0; row < 16; row++) {
    uint16_t bits = ((uint16_t)bitmap[row * 2] << 8) | bitmap[row * 2 + 1];
    for (uint8_t col = 0; col < 16; col++) {
      if (bits & (0x8000 >> col)) display.drawPixel(x + col, y + row, color);
    }
  }
  return true;
}

void SmartWatchApp::drawReaderMenu() {
  String items[] = {
      GBK("\xC4\xBF\xC2\xBC"),
      GBK("\xD7\xD6\xBA\xC5"),
      GBK("\xB8\xFC\xBB\xBB\xD7\xD6\xCC\xE5"),
      GBK("\xD6\xD8\xD6\xC3\xBD\xF8\xB6\xC8"),
      GBK("\xCD\xCB\xB3\xF6\xD4\xC4\xB6\xC1")};
  drawMenu(GBK("\xD4\xC4\xB6\xC1\xB2\xCB\xB5\xA5"), items, 5);
}

void SmartWatchApp::drawToc() {
  if (chapterCount == 0) scanChapters();
  String items[MAX_CHAPTERS];
  for (uint8_t i = 0; i < chapterCount; i++) items[i] = chapters[i].title;
  drawMenu(GBK("\xC4\xBF\xC2\xBC"), items, chapterCount);
}

void SmartWatchApp::drawWifi() {
  Serial.printf("[WiFi UI] drawWifi active=%s started=%s\n",
                wifiPortalActive ? "yes" : "no",
                wifiApStarted ? "yes" : "no");
  Serial.flush();
  if (!wifiPortalActive || !wifiApStarted) startWifiPortal();
  drawHeader(GBK("WiFi\xC8\xC8\xB5\xE3"));
  display.drawBitmap(86, 34, wifiApStarted ? wifi : wifioff, 26, 18, GxEPD_BLACK);
  drawUiText(wifiApStarted ? GBK("\xC8\xC8\xB5\xE3\xD2\xD1\xBF\xAA\xC6\xF4") : GBK("\xC8\xC8\xB5\xE3\xCA\xA7\xB0\xDC"), 16, 70);
  display.setCursor(16, 94);
  display.print("SSID: ");
  display.print(AP_SSID);
  display.setCursor(16, 118);
  display.print("PASS: none");
  display.setCursor(16, 142);
  display.print("IP: ");
  display.print(WiFi.softAPIP().toString());
  display.setCursor(16, 166);
  display.print("CH");
  display.print(WiFi.channel());
  display.print(" ");
  display.print(String(WiFi.softAPgetStationNum()) + " client(s)");
}

void SmartWatchApp::drawBle() {
  if (!bleConfigActive) startBleConfig();
  drawHeader(GBK("\xC0\xB6\xD1\xC0\xC5\xE4\xD6\xC3"));
  drawUiText(bleConfigActive ? GBK("\xC0\xB6\xD1\xC0\xB9\xE3\xB2\xA5\xD6\xD0") : GBK("\xC0\xB6\xD1\xC0\xD2\xD1\xCD\xA3\xD6\xB9"), 16, 58);
  display.setCursor(16, 82);
  display.print("Name: BBWF Watch");
  drawUiText(bleClientConnected ? GBK("\xCA\xD6\xBB\xFA\xD2\xD1\xC1\xAC\xBD\xD3") : GBK("\xB5\xC8\xB4\xFD\xCA\xD6\xBB\xFA"), 16, 106);
  drawUiText(GBK("WiFi: "), 16, 130);
  display.setCursor(70, 130);
  display.print(wifiClientSsid.length() ? wifiClientSsid.substring(0, 10) : "not set");
  drawUiText(GBK("\xCA\xB9\xD3\xC3\xCA\xD6\xBB\xFA\xD0\xA3\xCA\xB1"), 16, 154);
  display.setCursor(16, 190);
  display.print(bleStatusText.substring(0, 20));
}

void SmartWatchApp::drawAlarms() {
  if (alarmCount == 0) {
    drawHeader(GBK("\xC4\xD6\xD6\xD3"));
    drawUiText(GBK("\xCE\xDE\xC4\xD6\xD6\xD3"), 16, 80);
    return;
  }
  String items[MAX_ALARMS];
  for (uint8_t i = 0; i < alarmCount; i++) items[i] = alarmLabel(alarms[i]);
  drawMenu(GBK("\xC4\xD6\xD6\xD3"), items, alarmCount, GBK("\xC7\xEB\xD3\xC3WiFi\xBB\xF2App\xC9\xE8\xD6\xC3"));
}

void SmartWatchApp::drawSettings() {
  String items[] = {
      GBK("\xD7\xD6\xBA\xC5"),
      GBK("\xD7\xD6\xCC\xE5\xD1\xA1\xD4\xF1"),
      GBK("\xB4\xE6\xB4\xA2\xD0\xC5\xCF\xA2"),
      GBK("\xC1\xAC\xBD\xD3WiFi")};
  String footer = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : wifiClientSsid;
  drawMenu(GBK("\xC9\xE8\xD6\xC3"), items, 4, footer);
}

void SmartWatchApp::drawFontSize() {
  drawHeader(GBK("\xD7\xD6\xBA\xC5"));
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(56, 105);
  display.print(readerFontSize);
  display.print(" px");
  drawUiText(GBK("\xC9\xCF\xBC\xFC\xB7\xC5\xB4\xF3 \xCF\xC2\xBC\xFC\xCB\xF5\xD0\xA1"), 18, 160);
}

void SmartWatchApp::drawFontSelect() {
  scanFonts();
  if (fontCount == 0) {
    drawHeader(GBK("\xD7\xD6\xCC\xE5\xD1\xA1\xD4\xF1"));
    drawUiText(GBK("\xC3\xBB\xD3\xD0\xD7\xD6\xCC\xE5"), 16, 80);
    return;
  }
  String items[MAX_FONTS];
  for (uint8_t i = 0; i < fontCount; i++) {
    String fontName = i == 0 ? GBK("\xC4\xDA\xD6\xC3\xD7\xD6\xCC\xE5") : fonts[i].name;
    items[i] = (i == selectedFontIndex ? "* " : "  ") + fontName;
  }
  drawMenu(GBK("\xD7\xD6\xCC\xE5\xD1\xA1\xD4\xF1"), items, fontCount, GBK("\xB0\xD1HZK16\xB7\xC5\xC8\xEB/fonts"));
}

void SmartWatchApp::drawStorage() {
  drawHeader(GBK("\xB4\xE6\xB4\xA2\xBF\xD5\xBC\xE4"));
  if (!sdReady) {
    drawUiText(GBK("SD\xBF\xA8\xCE\xB4\xBE\xCD\xD0\xF7"), 16, 90);
    return;
  }
  uint64_t used = SD_MMC.usedBytes() / (1024 * 1024);
  uint64_t total = SD_MMC.totalBytes() / (1024 * 1024);
  display.drawCircle(100, 95, 50, GxEPD_BLACK);
  display.drawCircle(100, 95, 42, GxEPD_BLACK);
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(64, 101);
  display.print(used);
  display.print("MB");
  drawUiText(GBK("\xD7\xDC\xBC\xC6 "), 30, 170);
  display.setFont(&FreeMono9pt7b);
  display.setCursor(82, 170);
  display.print(total);
  display.print(" MB");
}

void SmartWatchApp::drawToast(const String &message) {
  display.fillRect(18, 78, 164, 44, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  display.setCursor(28, 104);
  display.print(message);
  display.setTextColor(GxEPD_BLACK);
  display.display(true);
}

void SmartWatchApp::pressBack() {
  if (page == PAGE_WATCHFACE && readingActive) {
    showPage(PAGE_READER);
  } else if (page == PAGE_READER) {
    showPage(PAGE_WATCHFACE);
  } else if (page == PAGE_READER_MENU) {
    showPage(PAGE_BOOK_LIST);
    readingActive = false;
  } else if (page == PAGE_BOOK_LIST || page == PAGE_MAIN_MENU) {
    showPage(PAGE_WATCHFACE);
  } else if (page == PAGE_TOC) {
    showPage(PAGE_READER_MENU);
  } else if (page == PAGE_WIFI || page == PAGE_BLE || page == PAGE_ALARMS || page == PAGE_SETTINGS) {
    if (page == PAGE_WIFI) stopWifiPortal();
    if (page == PAGE_BLE) stopBleConfig();
    showPage(PAGE_MAIN_MENU);
  } else if (page == PAGE_FONT_SIZE || page == PAGE_FONT_SELECT) {
    showPage(previousPage == PAGE_SETTINGS ? PAGE_SETTINGS : PAGE_READER_MENU);
  } else if (page == PAGE_STORAGE) {
    showPage(PAGE_SETTINGS);
  }
}

void SmartWatchApp::pressSelect() {
  if (page == PAGE_WATCHFACE) {
    showPage(PAGE_MAIN_MENU);
  } else if (page == PAGE_READER) {
    showPage(PAGE_READER_MENU);
  } else if (page == PAGE_MAIN_MENU) {
    if (selectedIndex == 0) {
      scanBooks();
      showPage(PAGE_BOOK_LIST);
    } else if (selectedIndex == 1) {
      startWifiPortal();
      showPage(PAGE_WIFI);
    } else if (selectedIndex == 2) {
      startBleConfig();
      showPage(PAGE_BLE);
    } else if (selectedIndex == 3) {
      showPage(PAGE_ALARMS);
    } else if (selectedIndex == 4) {
      showPage(PAGE_SETTINGS);
    }
  } else if (page == PAGE_BOOK_LIST) {
    openBook(selectedIndex);
  } else if (page == PAGE_READER_MENU) {
    if (selectedIndex == 0) showPage(PAGE_TOC);
    if (selectedIndex == 1) showPage(PAGE_FONT_SIZE);
    if (selectedIndex == 2) showPage(PAGE_FONT_SELECT);
    if (selectedIndex == 3 && currentBookIndex >= 0) {
      books[currentBookIndex].pageIndex = 0;
      saveBookProgress(books[currentBookIndex]);
      showPage(PAGE_READER);
    }
    if (selectedIndex == 4) {
      readingActive = false;
      showPage(PAGE_BOOK_LIST);
    }
  } else if (page == PAGE_TOC) {
    if (currentBookIndex >= 0 && selectedIndex < chapterCount) {
      books[currentBookIndex].pageIndex = min<uint32_t>(chapters[selectedIndex].pageIndex, books[currentBookIndex].pageCount - 1);
      saveBookProgress(books[currentBookIndex]);
      showPage(PAGE_READER);
    }
  } else if (page == PAGE_ALARMS && selectedIndex < alarmCount) {
    alarms[selectedIndex].enabled = !alarms[selectedIndex].enabled;
    saveAlarms();
    redraw(true);
  } else if (page == PAGE_SETTINGS) {
    if (selectedIndex == 0) showPage(PAGE_FONT_SIZE);
    if (selectedIndex == 1) showPage(PAGE_FONT_SELECT);
    if (selectedIndex == 2) showPage(PAGE_STORAGE);
    if (selectedIndex == 3) {
      drawToast(GBK("\xD5\xFD\xD4\xDA\xC1\xAC\xBD\xD3WiFi"));
      bool ok = connectWifiLan();
      drawToast(ok ? ("IP " + WiFi.localIP().toString()) : bleStatusText);
      showPage(PAGE_SETTINGS);
    }
  } else if (page == PAGE_FONT_SELECT && selectedIndex < fontCount) {
    selectedFontIndex = selectedIndex;
    saveAppState();
    showPage(PAGE_READER_MENU);
  }
}

void SmartWatchApp::pressUp() {
  if (page == PAGE_READER && currentBookIndex >= 0) {
    BookEntry &book = books[currentBookIndex];
    if (book.pageIndex > 0) book.pageIndex--;
    saveBookProgress(book);
    redraw(true);
  } else if (page == PAGE_FONT_SIZE) {
    readerFontSize = min<uint8_t>(24, readerFontSize + 2);
    refreshCurrentBookPageCount();
    saveAppState();
    redraw(true);
  } else {
    uint8_t count = 0;
    if (page == PAGE_MAIN_MENU) count = 5;
    if (page == PAGE_BOOK_LIST) count = bookCount;
    if (page == PAGE_READER_MENU) count = 5;
    if (page == PAGE_TOC) count = chapterCount;
    if (page == PAGE_ALARMS) count = alarmCount;
    if (page == PAGE_SETTINGS) count = 4;
    if (page == PAGE_FONT_SELECT) count = fontCount;
    moveSelection(-1, count);
  }
}

void SmartWatchApp::pressDown() {
  if (page == PAGE_READER && currentBookIndex >= 0) {
    BookEntry &book = books[currentBookIndex];
    if (book.pageIndex + 1 < book.pageCount) book.pageIndex++;
    saveBookProgress(book);
    redraw(true);
  } else if (page == PAGE_FONT_SIZE) {
    readerFontSize = max<uint8_t>(10, readerFontSize - 2);
    refreshCurrentBookPageCount();
    saveAppState();
    redraw(true);
  } else {
    uint8_t count = 0;
    if (page == PAGE_MAIN_MENU) count = 5;
    if (page == PAGE_BOOK_LIST) count = bookCount;
    if (page == PAGE_READER_MENU) count = 5;
    if (page == PAGE_TOC) count = chapterCount;
    if (page == PAGE_ALARMS) count = alarmCount;
    if (page == PAGE_SETTINGS) count = 4;
    if (page == PAGE_FONT_SELECT) count = fontCount;
    moveSelection(1, count);
  }
}

void SmartWatchApp::moveSelection(int8_t delta, uint8_t count) {
  if (count == 0) return;
  selectedIndex = (selectedIndex + count + delta) % count;
  redraw(true);
}

void SmartWatchApp::resetIdleTimer() {
  lastInteractionMs = millis();
}

void SmartWatchApp::serviceTimers() {
  if (wifiPortalActive || wifiLanActive) server.handleClient();
  // 局域网上传 10MB 小说可能持续几分钟，路由器短暂断开时后台自动重连。
  // App 会按 offset 续传，所以这里保持 WiFi 恢复即可。
  if (wifiLanActive && WiFi.status() != WL_CONNECTED && wifiClientSsid.length() && millis() - lastLanReconnectMs > 5000) {
    lastLanReconnectMs = millis();
    WiFi.reconnect();
  }
  if (wifiLanActive && WiFi.status() == WL_CONNECTED) {
    syncNetworkTime();
  }
  if (!wifiPortalActive) {
    sampleHeartRate();
    checkAlarms();
    resetStepsIfNeeded();
  }

  if (wifiPortalActive && page != PAGE_WIFI && WiFi.softAPgetStationNum() == 0 && millis() - wifiStartedMs > WIFI_IDLE_MS) {
    stopWifiPortal();
    if (page == PAGE_WIFI) showPage(PAGE_MAIN_MENU);
  }

  if (bleConfigActive && !bleClientConnected && millis() - bleStartedMs > BLE_IDLE_MS) {
    stopBleConfig();
    if (page == PAGE_BLE) showPage(PAGE_MAIN_MENU);
  }

  if (page == PAGE_MAIN_MENU && millis() - lastInteractionMs > MAIN_MENU_IDLE_MS) {
    showPage(PAGE_WATCHFACE);
  }

  bool idlePage = page == PAGE_ALARMS || page == PAGE_SETTINGS || page == PAGE_FONT_SIZE || page == PAGE_FONT_SELECT ||
                  page == PAGE_STORAGE;
  if (idlePage && millis() - lastInteractionMs > MENU_IDLE_MS) {
    showPage(PAGE_WATCHFACE);
  }
  if (page == PAGE_READER_MENU && millis() - lastInteractionMs > MENU_IDLE_MS) {
    showPage(PAGE_READER);
  }
}

void SmartWatchApp::checkAlarms() {
  for (uint8_t i = 0; i < alarmCount; i++) {
    AlarmEntry &alarm = alarms[i];
    if (currentTime.Hour == 0 && currentTime.Minute == 0) alarm.firedToday = false;
    if (alarm.enabled && !alarm.firedToday && currentTime.Hour == alarm.hour && currentTime.Minute == alarm.minute) {
      alarm.firedToday = true;
      playAlarmAlert();
      if (page != PAGE_READER) drawToast("Alarm " + alarmLabel(alarm));
    }
  }
}

void SmartWatchApp::sampleHeartRate() {
  if (millis() - lastHeartRateMs < HEART_RATE_INTERVAL_MS) return;
  lastHeartRateMs = millis();
  if (!max30102Ready) {
    heartRate = 0;
    spo2 = 0;
    return;
  }

  const uint8_t sampleCount = 80;
  uint32_t red[sampleCount];
  uint32_t ir[sampleCount];
  uint32_t irMin = UINT32_MAX;
  uint32_t irMax = 0;
  uint64_t redSum = 0;
  uint64_t irSum = 0;
  for (uint8_t i = 0; i < sampleCount; i++) {
    if (!readMax30102Sample(red[i], ir[i])) {
      heartRate = 0;
      spo2 = 0;
      Serial.println("[MAX30102] sample read failed");
      return;
    }
    redSum += red[i];
    irSum += ir[i];
    irMin = min<uint32_t>(irMin, ir[i]);
    irMax = max<uint32_t>(irMax, ir[i]);
    delay(25);
  }

  uint32_t irAvg = irSum / sampleCount;
  if (irAvg < 12000 || irMax - irMin < 800) {
    heartRate = 0;
    spo2 = 0;
    Serial.printf("[MAX30102] no stable finger irAvg=%lu amplitude=%lu\n",
                  (unsigned long)irAvg, (unsigned long)(irMax - irMin));
    return;
  }

  uint16_t peaks[12];
  uint8_t peakCount = 0;
  uint32_t threshold = irMin + (irMax - irMin) * 55 / 100;
  for (uint8_t i = 1; i + 1 < sampleCount && peakCount < 12; i++) {
    if (ir[i] > threshold && ir[i] > ir[i - 1] && ir[i] >= ir[i + 1]) {
      if (peakCount == 0 || i - peaks[peakCount - 1] > 8) {
        peaks[peakCount++] = i;
      }
    }
  }

  if (peakCount >= 2) {
    uint16_t intervals = 0;
    for (uint8_t i = 1; i < peakCount; i++) intervals += peaks[i] - peaks[i - 1];
    float avgIntervalSamples = (float)intervals / (peakCount - 1);
    heartRate = constrain((int)(2400.0f / avgIntervalSamples), 45, 180);
  } else {
    heartRate = 0;
  }

  uint32_t redAvg = redSum / sampleCount;
  uint32_t redMin = UINT32_MAX;
  uint32_t redMax = 0;
  for (uint8_t i = 0; i < sampleCount; i++) {
    redMin = min<uint32_t>(redMin, red[i]);
    redMax = max<uint32_t>(redMax, red[i]);
  }
  if (heartRate > 0 && redAvg > 0 && irAvg > 0 && redMax > redMin) {
    float r = ((float)(redMax - redMin) / redAvg) / ((float)(irMax - irMin) / irAvg);
    spo2 = constrain((int)(110.0f - 25.0f * r), 85, 100);
  } else {
    spo2 = 0;
  }

  Serial.printf("[MAX30102] hr=%u spo2=%u irAvg=%lu amp=%lu peaks=%u\n",
                heartRate, spo2, (unsigned long)irAvg, (unsigned long)(irMax - irMin), peakCount);
}

void SmartWatchApp::resetStepsIfNeeded() {
  if (currentTime.Hour != 0 || currentTime.Minute != 0 || currentTime.Day == lastStepResetDay) return;
#if BMA_ENABLED
  if (bmaReady) sensor.resetStepCounter();
#endif
  lastStepResetDay = currentTime.Day;
  saveAppState();
}

void SmartWatchApp::playAlarmAlert() {
  uint32_t vibStart = millis();
  pinMode(VIB_MOTOR_PIN, OUTPUT);
  digitalWrite(VIB_MOTOR_PIN, HIGH);
  for (uint8_t i = 0; i < 6; i++) {
    playTone(2200, 240);
    delay(80);
  }
  while (millis() - vibStart < 1000) delay(10);
  digitalWrite(VIB_MOTOR_PIN, LOW);
}

void SmartWatchApp::playTone(uint16_t frequency, uint32_t durationMs) {
  if (!audioReady) {
    pinMode(AUDIO_CODEC_PA_PIN, OUTPUT);
    digitalWrite(AUDIO_CODEC_PA_PIN, HIGH);
    delay(durationMs);
    digitalWrite(AUDIO_CODEC_PA_PIN, LOW);
    return;
  }

  digitalWrite(AUDIO_CODEC_PA_PIN, HIGH);
  const uint16_t samplesPerChunk = 128;
  int16_t buffer[samplesPerChunk * 2];
  uint32_t totalSamples = (durationMs * AUDIO_SAMPLE_RATE) / 1000;
  uint32_t writtenSamples = 0;
  while (writtenSamples < totalSamples) {
    uint16_t chunk = min<uint32_t>(samplesPerChunk, totalSamples - writtenSamples);
    for (uint16_t i = 0; i < chunk; i++) {
      float t = (float)(writtenSamples + i) / AUDIO_SAMPLE_RATE;
      int16_t sample = (int16_t)(sinf(2.0f * PI * frequency * t) * 18000);
      buffer[i * 2] = sample;
      buffer[i * 2 + 1] = sample;
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_NUM_0, buffer, chunk * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    writtenSamples += chunk;
  }
  digitalWrite(AUDIO_CODEC_PA_PIN, LOW);
}

bool SmartWatchApp::readMax30102Sample(uint32_t &red, uint32_t &ir) {
  if (!max30102Ready) return false;
  Wire.beginTransmission(MAX30102_ADDR);
  Wire.write(0x07);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MAX30102_ADDR, (uint8_t)6) != 6) return false;
  red = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();
  ir = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();
  red &= 0x3FFFF;
  ir &= 0x3FFFF;
  return true;
}

uint8_t SmartWatchApp::calculateSpO2(uint32_t red, uint32_t ir) const {
  if (ir == 0) return 0;
  int ratio = (int)((red * 100UL) / ir);
  return constrain(110 - ratio / 4, 88, 100);
}

bool SmartWatchApp::writeMax30102Reg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(MAX30102_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

uint8_t SmartWatchApp::readMax30102Reg(uint8_t reg) {
  Wire.beginTransmission(MAX30102_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(MAX30102_ADDR, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

uint32_t SmartWatchApp::currentStepCount() {
#if BMA_ENABLED
  if (!bmaReady) return lastStepCount;
  lastStepCount = sensor.getCounter();
  return lastStepCount;
#else
  return 0;
#endif
}

void SmartWatchApp::startWifiPortal() {
  if (wifiPortalActive && wifiApStarted) {
    wifiStartedMs = millis();
    Serial.printf("[WiFi AP] already running: SSID=%s IP=%s clients=%u\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum());
    Serial.flush();
    return;
  }
  stopLanServer();
  prepareForWifiHotspot();
  Serial.println();
  Serial.println("[WiFi AP] resetting radio");
  Serial.flush();
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  delay(200);
  WiFi.enableSTA(false);
  delay(300);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  wifi_country_t country = {"CN", 1, 13, WIFI_COUNTRY_POLICY_MANUAL};
  esp_wifi_set_country(&country);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  bool configured = true;

  wifiApStarted = false;
  for (uint8_t attempt = 1; attempt <= 3 && !wifiApStarted; attempt++) {
    wifiApStarted = WiFi.softAP(AP_SSID, AP_PASS, 6, false, 4);
    delay(700);
    Serial.printf("[WiFi AP] attempt=%u configured=%s started=%s mode=%d channel=%d ip=%s mac=%s clients=%u\n",
                  attempt,
                  configured ? "yes" : "no",
                  wifiApStarted ? "yes" : "no",
                  WiFi.getMode(),
                  WiFi.channel(),
                  WiFi.softAPIP().toString().c_str(),
                  WiFi.softAPmacAddress().c_str(),
                  WiFi.softAPgetStationNum());
    Serial.flush();
  }
  WIFI_CONFIGURED = configured && wifiApStarted;
  wifiPortalActive = true;
  wifiStartedMs = millis();
  Serial.printf("[WiFi AP] config=%s softAP=%s SSID=%s IP=%s GW=%s MAC=%s channel=%d clients=%u max_clients=4\n",
                configured ? "ok" : "failed",
                wifiApStarted ? "ok" : "failed",
                AP_SSID,
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPmacAddress().c_str(),
                WiFi.channel(),
                WiFi.softAPgetStationNum());
  Serial.flush();

  if (wifiApStarted) {
    startLanServer();
    Serial.printf("[WiFi AP] web server started: http://%s/\n", WiFi.softAPIP().toString().c_str());
    Serial.flush();
  }
}

void SmartWatchApp::stopWifiPortal() {
  if (!wifiPortalActive) return;
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  WIFI_CONFIGURED = false;
  wifiApStarted = false;
  wifiPortalActive = false;
  wifiLanActive = false;
  Serial.println("[WiFi AP] stopped");
  Serial.flush();
  resumeAfterWifiHotspot();
}

void SmartWatchApp::startLanServer() {
  if (wifiLanActive) return;
  server.stop();
  server.on("/", [this]() { handleWebRoot(); });
  server.on("/alarm/add", [this]() { handleAddAlarm(); });
  server.on("/alarm/toggle", [this]() { handleToggleAlarm(); });
  server.on("/alarm/delete", [this]() { handleDeleteAlarm(); });
  server.on("/sync", [this]() { handleSyncSettings(); });
  server.on("/wifi", [this]() { handleWifiSettings(); });
  server.on("/book/delete", [this]() { handleDeleteBook(); });
  // 分块接口替代一次性 multipart 上传。WiFi 中断后，手机可以查询已保存 offset 续传。
  server.on("/book/chunk/begin", HTTP_GET, [this]() { handleBookChunkBegin(); });
  server.on("/book/chunk/status", HTTP_GET, [this]() { handleBookChunkStatus(); });
  server.on("/book/chunk/write", HTTP_POST, [this]() { handleBookChunkWrite(); });
  server.on("/book/chunk/finish", HTTP_GET, [this]() { handleBookChunkFinish(); });
  server.on(
      "/book/upload", HTTP_POST,
      [this]() {
        scanBooks();
        updateBleStatus("Book uploaded");
        server.sendHeader("Location", "/");
        server.send(302, "text/plain", "");
      },
      [this]() { handleBookUpload(); });
  server.begin();
  wifiLanActive = true;
}

void SmartWatchApp::stopLanServer() {
  if (!wifiLanActive) return;
  server.stop();
  wifiLanActive = false;
}

bool SmartWatchApp::connectWifiLan(uint32_t timeoutMs) {
  if (!wifiClientSsid.length()) {
    bleStatusText = "WiFi SSID empty";
    return false;
  }
  if (wifiPortalActive) stopWifiPortal();
  stopLanServer();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // LAN 模式下上传稳定性比省电更重要。WiFi 休眠可能导致延迟或断连，所以保持唤醒。
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.disconnect(false, false);
  delay(200);
  wl_status_t beginStatus = WiFi.begin(wifiClientSsid.c_str(), wifiClientPass.c_str());

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < timeoutMs) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    wl_status_t finalStatus = WiFi.status();
    WiFi.mode(WIFI_OFF);
    bleStatusText = "LAN WiFi failed begin=" + String((int)beginStatus) + " status=" + String((int)finalStatus);
    return false;
  }

  WIFI_CONFIGURED = true;
  startLanServer();
  String ipText = WiFi.localIP().toString();
  syncNetworkTime();
  bleStatusText = "LAN IP " + ipText;
  saveAppState();
  return true;
}

void SmartWatchApp::prepareForWifiHotspot() {
  if (peripheralsSuspendedForWifi) return;
  Serial.println("[WiFi AP] preparing hotspot");
  Serial.flush();

  if (bleConfigActive) {
    stopBleConfig();
    delay(200);
    Serial.println("[WiFi AP] BLE stopped");
    Serial.flush();
  }
  peripheralsSuspendedForWifi = false;
  Serial.println("[WiFi AP] sensors left running for AP test");
  Serial.flush();
}

void SmartWatchApp::resumeAfterWifiHotspot() {
  if (!peripheralsSuspendedForWifi) return;
  Serial.println("[WiFi AP] resuming sensors/peripherals");
  Serial.flush();

#ifdef ARDUINO_ESP32S3_DEV
  Wire.begin(WATCHY_V3_SDA, WATCHY_V3_SCL);
#else
  Wire.begin(SDA, SCL);
#endif
  delay(100);

  bootMotion();
  bootAudio();
  bootHeartRate();
  peripheralsSuspendedForWifi = false;
  Serial.println("[WiFi AP] sensors/peripherals resumed");
  Serial.flush();
}

void SmartWatchApp::startBleConfig() {
  bleStartedMs = millis();
  if (bleConfigActive) {
    updateBleStatus("Advertising");
    return;
  }

  Serial.println("[BLE Config] starting");
  BLEDevice::init(BLE_CONFIG_NAME);
  BLEDevice::setMTU(247);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new WatchBleServerCallbacks(this));
  bleService = bleServer->createService(BLE_SERVICE_UUID);

  bleStatusCharacteristic = bleService->createCharacteristic(
      BLE_STATUS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleStatusCharacteristic->addDescriptor(new BLE2902());

  bleCommandCharacteristic = bleService->createCharacteristic(
      BLE_COMMAND_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  bleCommandCharacteristic->setCallbacks(new WatchBleCommandCallbacks(this));

  bleService->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setName(BLE_CONFIG_NAME);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  advertising->setMinInterval(0x20);
  advertising->setMaxInterval(0x40);
  BLEDevice::startAdvertising();

  bleConfigActive = true;
  bleStatusText = "Advertising";
  updateBleStatus();
  Serial.printf("[BLE Config] advertising name=%s service=%s\n", BLE_CONFIG_NAME, BLE_SERVICE_UUID);
}

void SmartWatchApp::stopBleConfig() {
  if (!bleConfigActive) return;
  BLEDevice::stopAdvertising();
  BLEDevice::deinit(false);
  bleServer = nullptr;
  bleService = nullptr;
  bleStatusCharacteristic = nullptr;
  bleCommandCharacteristic = nullptr;
  bleConfigActive = false;
  bleClientConnected = false;
  bleStatusText = "BLE stopped";
  Serial.println("[BLE Config] stopped");
}

void SmartWatchApp::onBleClientChanged(bool connected) {
  bleClientConnected = connected;
  bleStartedMs = millis();
  bleStatusText = connected ? "Phone connected" : "Phone disconnected";
  Serial.printf("[BLE Config] client %s\n", connected ? "connected" : "disconnected");
  updateBleStatus();
  if (!connected && bleConfigActive) {
    delay(100);
    BLEDevice::startAdvertising();
  }
  if (page == PAGE_BLE) redraw(true);
}

void SmartWatchApp::handleBleCommand(const String &command) {
  String result = applyConfigCommand(command);
  bleLastCommand = command;
  bleStatusText = result;
  Serial.printf("[BLE Config] command=%s result=%s\n", command.c_str(), result.c_str());
  if (!command.startsWith("BOOK_UPLOAD_CHUNK:")) updateBleStatus(result);
  if (page == PAGE_BLE) redraw(true);
}

void SmartWatchApp::updateBleStatus(const String &message) {
  if (message.length()) bleStatusText = message;
  if (!bleStatusCharacteristic) return;
  String status = buildBleStatus();
  bleStatusCharacteristic->setValue(status.c_str());
  if (bleClientConnected) bleStatusCharacteristic->notify();
}

String SmartWatchApp::buildBleStatus() const {
  String status = "{";
  status += "\"time\":\"" + twoDigits(currentTime.Hour) + ":" + twoDigits(currentTime.Minute) + "\",";
  status += "\"heartRate\":" + String(heartRate) + ",";
  status += "\"spo2\":" + String(spo2) + ",";
  status += "\"wifiSsid\":\"" + jsonEscape(wifiClientSsid) + "\",";
  status += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  status += "\"lanIp\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\",";
  status += "\"lanActive\":" + String(wifiLanActive ? "true" : "false") + ",";
  status += "\"bookCount\":" + String(bookCount) + ",";
  status += "\"alarmCount\":" + String(alarmCount) + ",";
  status += "\"message\":\"" + jsonEscape(bleStatusText) + "\"}";
  return status;
}

String SmartWatchApp::applyConfigCommand(const String &rawCommand) {
  String command = rawCommand;
  command.trim();
  if (!command.length()) return "Empty command";

  int split = command.indexOf(':');
  String action = split >= 0 ? command.substring(0, split) : command;
  String payload = split >= 0 ? command.substring(split + 1) : "";
  action.toUpperCase();

  if (action == "WIFI") {
    int sep = payload.indexOf('|');
    if (sep < 0) return "Use WIFI:ssid|pass";
    wifiClientSsid = payload.substring(0, sep);
    wifiClientPass = payload.substring(sep + 1);
    saveAppState();
    return connectWifiLan() ? "LAN IP " + WiFi.localIP().toString() : "LAN WiFi failed";
  }

  if (action == "LAN_CONNECT") {
    return connectWifiLan() ? "LAN IP " + WiFi.localIP().toString() : "LAN WiFi failed";
  }

  if (action == "ALARM_ADD") {
    if (alarmCount >= MAX_ALARMS || payload.length() < 5) return "Alarm full";
    alarms[alarmCount++] = {
        (uint8_t)constrain(payload.substring(0, 2).toInt(), 0, 23),
        (uint8_t)constrain(payload.substring(3, 5).toInt(), 0, 59),
        true,
        false};
    saveAlarms();
    return "Alarm added";
  }

  if (action == "ALARM_TOGGLE") {
    uint8_t id = payload.toInt();
    if (id >= alarmCount) return "Bad alarm id";
    alarms[id].enabled = !alarms[id].enabled;
    saveAlarms();
    return "Alarm toggled";
  }

  if (action == "ALARM_DELETE") {
    uint8_t id = payload.toInt();
    if (id >= alarmCount) return "Bad alarm id";
    for (uint8_t i = id; i + 1 < alarmCount; i++) {
      alarms[i] = alarms[i + 1];
    }
    alarmCount--;
    saveAlarms();
    return "Alarm deleted";
  }

  if (action == "SET_TIME") {
    return setClockFromText(payload) ? "Time set" : "Bad time";
  }

  if (action == "BOOK_DELETE") {
    return deleteBook(payload.toInt()) ? "Book deleted" : "Bad book id";
  }

  if (action == "BOOK_REFRESH") {
    scanBooks();
    return "Books refreshed";
  }

  if (action == "BOOK_UPLOAD_BEGIN") {
    return beginBleBookUpload(payload) ? "Upload ready" : "Upload begin failed";
  }

  if (action == "BOOK_UPLOAD_CHUNK") {
    return writeBleBookChunk(payload) ? "Chunk ok " + String(bleUploadReceivedBytes) : "Chunk failed";
  }

  if (action == "BOOK_UPLOAD_END") {
    return finishBleBookUpload() ? "Upload complete" : "Upload incomplete";
  }

  if (action == "BOOK_UPLOAD_ABORT") {
    abortBleBookUpload();
    return "Upload aborted";
  }

  if (action == "STATUS") return "Status updated";
  return "Unknown command";
}

bool SmartWatchApp::setClockFromText(const String &dateTime) {
  if (dateTime.length() < 19) return false;
  tmElements_t tm;
  tm.Year = CalendarYrToTm(dateTime.substring(0, 4).toInt());
  tm.Month = constrain(dateTime.substring(5, 7).toInt(), 1, 12);
  tm.Day = constrain(dateTime.substring(8, 10).toInt(), 1, 31);
  tm.Hour = constrain(dateTime.substring(11, 13).toInt(), 0, 23);
  tm.Minute = constrain(dateTime.substring(14, 16).toInt(), 0, 59);
  tm.Second = constrain(dateTime.substring(17, 19).toInt(), 0, 59);
  time_t t = makeTime(tm);
  breakTime(t, tm);
  RTC.set(tm);
  RTC.read(currentTime);
  saveCurrentTimeSnapshot(true);
  return true;
}

bool SmartWatchApp::deleteBook(uint8_t id) {
  if (id >= bookCount || !sdReady) return false;
  String progressPath = bookProgressPath(books[id]);
  if (currentBookIndex == id) {
    currentBookIndex = -1;
    readingActive = false;
  }
  if (!SD_MMC.remove(books[id].path)) return false;
  if (SD_MMC.exists(progressPath)) SD_MMC.remove(progressPath);
  scanBooks();
  if (page == PAGE_BOOK_LIST || page == PAGE_READER || page == PAGE_READER_MENU) showPage(PAGE_BOOK_LIST, true);
  return true;
}

bool SmartWatchApp::beginBleBookUpload(const String &payload) {
  if (!sdReady) return false;
  abortBleBookUpload();
  int split = payload.indexOf('|');
  if (split < 0) return false;
  bleUploadName = nextBookFileName();
  bleUploadExpectedBytes = payload.substring(split + 1).toInt();
  bleUploadReceivedBytes = 0;
  if (!isTxtFile(bleUploadName) || bleUploadExpectedBytes == 0) return false;
  if (!SD_MMC.exists(BOOK_DIR)) SD_MMC.mkdir(BOOK_DIR);
  String path = String(BOOK_DIR) + "/" + bleUploadName;
  bleUploadFile = SD_MMC.open(path, FILE_WRITE);
  if (!bleUploadFile) return false;
  bleStatusText = "Uploading " + bleUploadName;
  Serial.printf("[BLE Upload] begin %s size=%lu\n", path.c_str(), (unsigned long)bleUploadExpectedBytes);
  return true;
}

bool SmartWatchApp::writeBleBookChunk(const String &payload) {
  if (!bleUploadFile) return false;
  size_t expectedLen = 0;
  int decodeLen = mbedtls_base64_decode(nullptr, 0, &expectedLen,
                                        (const unsigned char *)payload.c_str(), payload.length());
  if (decodeLen != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || expectedLen == 0) return false;
  uint8_t buffer[180];
  if (expectedLen > sizeof(buffer)) return false;
  size_t outLen = 0;
  if (mbedtls_base64_decode(buffer, sizeof(buffer), &outLen,
                            (const unsigned char *)payload.c_str(), payload.length()) != 0) {
    return false;
  }
  if (outLen == 0) return false;
  if (bleUploadFile.write(buffer, outLen) != outLen) return false;
  bleUploadReceivedBytes += outLen;
  bleStartedMs = millis();
  bleStatusText = "Uploading " + String(bleUploadReceivedBytes) + "/" + String(bleUploadExpectedBytes);
  return true;
}

bool SmartWatchApp::finishBleBookUpload() {
  if (!bleUploadFile) return false;
  bleUploadFile.close();
  bool ok = bleUploadReceivedBytes == bleUploadExpectedBytes;
  Serial.printf("[BLE Upload] end %s received=%lu expected=%lu ok=%s\n",
                bleUploadName.c_str(),
                (unsigned long)bleUploadReceivedBytes,
                (unsigned long)bleUploadExpectedBytes,
                ok ? "yes" : "no");
  if (!ok) {
    SD_MMC.remove(String(BOOK_DIR) + "/" + bleUploadName);
    bleStatusText = "Upload incomplete";
  } else {
    scanBooks();
    bleStatusText = "Upload complete";
  }
  bleUploadName = "";
  bleUploadExpectedBytes = 0;
  bleUploadReceivedBytes = 0;
  return ok;
}

void SmartWatchApp::abortBleBookUpload() {
  if (bleUploadFile) bleUploadFile.close();
  if (bleUploadName.length() && sdReady) SD_MMC.remove(String(BOOK_DIR) + "/" + bleUploadName);
  bleUploadName = "";
  bleUploadExpectedBytes = 0;
  bleUploadReceivedBytes = 0;
}

String SmartWatchApp::safeUploadName(const String &name) const {
  String clean = name;
  clean.replace("\\", "/");
  clean = clean.substring(clean.lastIndexOf('/') + 1);
  clean.trim();
  clean.replace("..", "_");
  clean.replace(":", "_");
  clean.replace("*", "_");
  clean.replace("?", "_");
  clean.replace("\"", "_");
  clean.replace("<", "_");
  clean.replace(">", "_");
  clean.replace("|", "_");
  if (!clean.length()) clean = "book.txt";
  return clean;
}

bool SmartWatchApp::isPlaceholderUploadName(const String &name) const {
  String clean = name;
  clean.trim();
  clean.toLowerCase();
  return clean.length() == 0 || clean == ".txt" || clean == "txt" ||
         clean == "book.txt" || clean == "novel.txt" || clean == "document.txt";
}

String SmartWatchApp::nextBookFileName() const {
  if (!sdReady || !SD_MMC.exists(BOOK_DIR)) return "0.txt";
  for (uint16_t i = 0; i < 1000; i++) {
    String name = String(i) + ".txt";
    String path = String(BOOK_DIR) + "/" + name;
    if (!SD_MMC.exists(path)) return name;
  }
  return String(millis()) + ".txt";
}

String SmartWatchApp::jsonEscape(const String &value) const {
  String out;
  out.reserve(value.length() + 4);
  for (uint16_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n' || c == '\r') {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

void SmartWatchApp::handleWebRoot() {
  String html = "<!doctype html><meta charset='utf-8'><title>BBWF</title>";
  html += "<script>function fillPhoneTime(){const d=new Date(),p=n=>String(n).padStart(2,'0');";
  html += "document.getElementById('dt').value=d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())+':'+p(d.getSeconds());}</script>";
  html += "<h2>BBWF Remote Manager</h2><p>Time ";
  html += twoDigits(currentTime.Hour) + ":" + twoDigits(currentTime.Minute) + "</p>";
  html += "<p>HR ";
  html += heartRate > 0 ? String(heartRate) : "--";
  html += " bpm / SpO2 ";
  html += spo2 > 0 ? String(spo2) : "--";
  html += "%</p>";
  html += "<h3>Alarm Settings</h3><form action='/alarm/add'><input name='time' type='time'><button>Add</button></form><ul>";
  for (uint8_t i = 0; i < alarmCount; i++) {
    html += "<li>" + alarmLabel(alarms[i]) + " <a href='/alarm/toggle?id=" + String(i) + "'>Toggle</a> ";
    html += "<a href='/alarm/delete?id=" + String(i) + "'>Delete</a></li>";
  }
  html += "</ul><h3>Time</h3><form action='/sync'><input id='dt' name='dt' placeholder='YYYY-MM-DD HH:MM:SS'>";
  html += "<button type='button' onclick='fillPhoneTime()'>Use phone time</button><button>Set</button></form>";
  html += "<h3>WiFi Client</h3><form action='/wifi'>SSID <input name='ssid' value='";
  html += wifiClientSsid;
  html += "'><br>Password <input name='pass' type='password' value='";
  html += wifiClientPass;
  html += "'><button>Save</button></form>";
  html += "<h3>Upload Novel TXT</h3><form method='POST' action='/book/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='book' accept='.txt,text/plain'><button>Upload TXT</button></form>";
  html += "<h3>Books on SD</h3><ul>";
  for (uint8_t i = 0; i < bookCount; i++) {
    html += "<li>";
    html += String(i);
    html += ". ";
    html += books[i].name;
    html += " ";
    html += books[i].pageIndex + 1;
    html += "/";
    html += books[i].pageCount;
    html += " <a href='/book/delete?id=" + String(i) + "'>Delete</a></li>";
  }
  if (bookCount == 0) html += "<li>No TXT books</li>";
  html += "</ul><p>Put HZK16/GBK font files in SD /fonts.</p>";
  server.send(200, "text/html; charset=utf-8", html);
}

void SmartWatchApp::handleAddAlarm() {
  if (alarmCount < MAX_ALARMS && server.hasArg("time")) {
    String t = server.arg("time");
    alarms[alarmCount++] = {(uint8_t)t.substring(0, 2).toInt(), (uint8_t)t.substring(3, 5).toInt(), true, false};
    saveAlarms();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void SmartWatchApp::handleToggleAlarm() {
  uint8_t id = server.arg("id").toInt();
  if (id < alarmCount) {
    alarms[id].enabled = !alarms[id].enabled;
    saveAlarms();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void SmartWatchApp::handleSyncSettings() {
  if (server.hasArg("dt")) setClockFromText(server.arg("dt"));
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void SmartWatchApp::handleDeleteAlarm() {
  uint8_t id = server.arg("id").toInt();
  if (id < alarmCount) {
    for (uint8_t i = id; i + 1 < alarmCount; i++) {
      alarms[i] = alarms[i + 1];
    }
    alarmCount--;
    saveAlarms();
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void SmartWatchApp::handleWifiSettings() {
  if (server.hasArg("ssid")) wifiClientSsid = server.arg("ssid");
  if (server.hasArg("pass")) wifiClientPass = server.arg("pass");
  saveAppState();
  connectWifiLan();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void SmartWatchApp::handleBookUpload() {
  static File uploadFile;
  HTTPUpload &upload = server.upload();
  if (!sdReady) return;

  if (upload.status == UPLOAD_FILE_START) {
    if (uploadFile) uploadFile.close();
    String filename = nextBookFileName();
    if (!isTxtFile(filename)) {
      Serial.printf("[Book Upload] rejected non-txt file: %s\n", filename.c_str());
      return;
    }
    if (!SD_MMC.exists(BOOK_DIR)) SD_MMC.mkdir(BOOK_DIR);
    String path = String(BOOK_DIR) + "/" + filename;
    uploadFile = SD_MMC.open(path, FILE_WRITE);
    Serial.printf("[Book Upload] start %s\n", path.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("[Book Upload] done %s size=%u\n", upload.filename.c_str(), upload.totalSize);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    Serial.println("[Book Upload] aborted");
  }
}

void SmartWatchApp::handleBookChunkBegin() {
  if (!sdReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"sd\"}");
    return;
  }
  if (!SD_MMC.exists(BOOK_DIR)) SD_MMC.mkdir(BOOK_DIR);
  // 文件名由固件分配，避免 Android 文件选择器返回 ".txt" 或重复文件名造成混乱。
  String filename = nextBookFileName();
  String path = String(BOOK_DIR) + "/" + filename;
  if (SD_MMC.exists(path)) SD_MMC.remove(path);
  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"open\"}");
    return;
  }
  file.close();
  Serial.printf("[Book Chunk] begin %s\n", path.c_str());
  server.send(200, "application/json", "{\"ok\":true,\"name\":\"" + jsonEscape(filename) + "\",\"offset\":0}");
}

void SmartWatchApp::handleBookChunkStatus() {
  if (!sdReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"sd\"}");
    return;
  }
  String name = server.arg("name");
  String filename = safeUploadName(name);
  if (isPlaceholderUploadName(filename) || !isTxtFile(filename)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"name\"}");
    return;
  }
  String path = String(BOOK_DIR) + "/" + filename;
  uint32_t offset = 0;
  // 分块失败后手机会查询这里，按 SD 卡实际写入字节数继续，而不是按上次尝试发送的位置。
  if (SD_MMC.exists(path)) {
    File file = SD_MMC.open(path, FILE_READ);
    if (file) {
      offset = file.size();
      file.close();
    }
  }
  server.send(200, "application/json", "{\"ok\":true,\"offset\":" + String(offset) + "}");
}

void SmartWatchApp::handleBookChunkWrite() {
  if (!sdReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"sd\"}");
    return;
  }
  String name = safeUploadName(server.arg("name"));
  uint32_t offset = (uint32_t)server.arg("offset").toInt();
  if (isPlaceholderUploadName(name) || !isTxtFile(name) || !server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"arg\"}");
    return;
  }
  if (!SD_MMC.exists(BOOK_DIR)) SD_MMC.mkdir(BOOK_DIR);
  String path = String(BOOK_DIR) + "/" + name;
  uint32_t currentSize = 0;
  if (SD_MMC.exists(path)) {
    File existing = SD_MMC.open(path, FILE_READ);
    if (existing) {
      currentSize = existing.size();
      existing.close();
    }
  }
  if (currentSize != offset) {
    // offset 不一致说明上一个请求可能部分成功或被重试，返回 SD 实际大小让 App 重新同步。
    server.send(409, "application/json", "{\"ok\":false,\"offset\":" + String(currentSize) + "}");
    return;
  }
  File file = SD_MMC.open(path, FILE_APPEND);
  if (!file) {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"open\"}");
    return;
  }
  String chunk = server.arg("plain");
  size_t written = file.write((const uint8_t *)chunk.c_str(), chunk.length());
  file.close();
  uint32_t nextOffset = currentSize + written;
  server.send(200, "application/json", "{\"ok\":true,\"offset\":" + String(nextOffset) + "}");
}

void SmartWatchApp::handleBookChunkFinish() {
  if (!sdReady) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"sd\"}");
    return;
  }
  scanBooks();
  updateBleStatus("Book uploaded");
  server.send(200, "application/json", "{\"ok\":true,\"books\":" + String(bookCount) + "}");
}

void SmartWatchApp::handleDeleteBook() {
  deleteBook(server.arg("id").toInt());
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

String SmartWatchApp::alarmLabel(const AlarmEntry &alarm) const {
  return twoDigits(alarm.hour) + ":" + twoDigits(alarm.minute) + (alarm.enabled ? " [ON]" : " [OFF]");
}
// 数字补零工具函数
String SmartWatchApp::twoDigits(uint8_t value) const {
  return value < 10 ? "0" + String(value) : String(value);
}

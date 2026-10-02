#ifndef SMART_WATCH_APP_H
#define SMART_WATCH_APP_H

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include "src/Watchy.h"
#include <WebServer.h>
#include <WiFi.h>

class BLECharacteristic;
class BLEServer;
class BLEService;

#include "icons.h"

enum AppPage : uint8_t {
  PAGE_WATCHFACE,
  PAGE_MAIN_MENU,
  PAGE_BOOK_LIST,
  PAGE_READER,
  PAGE_READER_MENU,
  PAGE_TOC,
  PAGE_WIFI,
  PAGE_ALARMS,
  PAGE_SETTINGS,
  PAGE_FONT_SIZE,
  PAGE_FONT_SELECT,
  PAGE_STORAGE,
  PAGE_BLE
};

struct BookEntry {
  String name;
  String path;
  uint32_t pageCount;
  uint32_t pageIndex;
};

struct ChapterEntry {
  String title;
  uint32_t pageIndex;
};

struct AlarmEntry {
  uint8_t hour;
  uint8_t minute;
  bool enabled;
  bool firedToday;
};

struct FontEntry {
  String name;
  String path;
};

class SmartWatchApp : public Watchy {
public:
  explicit SmartWatchApp(const watchySettings &s);

  void init(String datetime = "");
  void tick();
  void handleButtonPress() override;
  void drawWatchFace() override;

private:
  static const uint8_t MAX_BOOKS = 16;
  static const uint8_t MAX_CHAPTERS = 32;
  static const uint8_t MAX_ALARMS = 8;
  static const uint8_t MAX_FONTS = 8;

  AppPage page;
  AppPage previousPage;
  uint8_t selectedIndex;
  uint8_t menuTopIndex;
  uint8_t readerFontSize;
  uint8_t selectedFontIndex;
  int8_t currentBookIndex;
  bool sdReady;
  bool readingActive;
  bool wifiPortalActive;
  bool wifiApStarted;
  bool wifiLanActive;
  bool bleConfigActive;
  bool bleClientConnected;
  bool audioReady;
  bool max30102Ready;
  bool bmaReady;
  bool peripheralsSuspendedForWifi;
  uint32_t lastInteractionMs;
  uint32_t wifiStartedMs;
  uint32_t bleStartedMs;
  uint32_t lastHeartRateMs;
  uint32_t lastLanReconnectMs;
  uint32_t lastTimeSaveMs;
  uint32_t lastNtpSyncMs;
  bool ntpSyncedThisBoot;
  uint16_t heartRate;
  uint8_t spo2;
  uint32_t lastStepCount;
  uint8_t lastStepResetDay;
  String wifiClientSsid;
  String wifiClientPass;
  String bleStatusText;
  String bleLastCommand;
  String bleUploadName;
  uint32_t bleUploadExpectedBytes;
  uint32_t bleUploadReceivedBytes;
  File bleUploadFile;
  bool lastBackState;
  bool lastMenuState;
  bool lastUpState;
  bool lastDownState;
  uint32_t lastRtcReadMs;
  int16_t lastDrawnMinute;

  BookEntry books[MAX_BOOKS];
  uint8_t bookCount;
  ChapterEntry chapters[MAX_CHAPTERS];
  uint8_t chapterCount;
  AlarmEntry alarms[MAX_ALARMS];
  uint8_t alarmCount;
  FontEntry fonts[MAX_FONTS];
  uint8_t fontCount;
  WebServer server;
  BLEServer *bleServer;
  BLEService *bleService;
  BLECharacteristic *bleStatusCharacteristic;
  BLECharacteristic *bleCommandCharacteristic;

public:
  void onBleClientChanged(bool connected);
  void handleBleCommand(const String &command);

  void bootStorage();
  void bootMotion();
  void bootAudio();
  void bootHeartRate();
  void scanBooks();
  void scanFonts();
  void openBook(uint8_t index);
  void loadBookProgress(BookEntry &book);
  void saveBookProgress(const BookEntry &book);
  void loadAppState();
  void saveAppState();
  bool isValidTime(const tmElements_t &tm) const;
  void restoreSavedTimeIfNeeded();
  void saveCurrentTimeSnapshot(bool force = false);
  bool syncNetworkTime();
  void loadAlarms();
  void saveAlarms();
  String bookProgressPath(const BookEntry &book) const;
  void scanChapters();
  String readPageText(BookEntry &book, uint32_t pageIndex);
  uint16_t charsPerPage() const;
  uint32_t safePageOffset(const BookEntry &book, uint32_t pageIndex);
  uint16_t safeTextLimit(const String &text) const;
  void drawReaderText(const String &text, int16_t x, int16_t y, int16_t maxY);
  void drawUiText(const String &text, int16_t x, int16_t baseline, uint16_t color = GxEPD_BLACK);
  bool drawGbkGlyph(uint8_t high, uint8_t low, int16_t x, int16_t y, uint16_t color = GxEPD_BLACK);
  uint8_t utf8GlyphWidth(const String &text, uint16_t index) const;
  uint16_t nextUtf8Index(const String &text, uint16_t index) const;
  bool isGbkLead(uint8_t value) const;
  bool isGbkTrail(uint8_t value) const;
  bool isGbkPair(const String &text, uint16_t index) const;
  bool textLooksUtf8(const String &text) const;
  bool textHasNonAscii(const String &text) const;
  uint8_t batteryPercent() const;
  bool isTxtFile(const String &name) const;
  bool isFontFile(const String &name) const;
  bool looksLikeChapter(const String &line) const;
  void refreshCurrentBookPageCount();

  void showPage(AppPage nextPage, bool partial = false);
  void redraw(bool partial = false);
  void drawHeader(const String &title);
  void drawMenu(const String &title, const String items[], uint8_t count, const String &footer = "");
  void drawBookList();
  void drawReader();
  void drawReaderMenu();
  void drawToc();
  void drawWifi();
  void drawAlarms();
  void drawSettings();
  void drawFontSize();
  void drawFontSelect();
  void drawStorage();
  void drawBle();
  void drawToast(const String &message);

  void pressBack();
  void pressSelect();
  void pressUp();
  void pressDown();
  void moveSelection(int8_t delta, uint8_t count);
  void resetIdleTimer();
  void serviceTimers();
  void checkAlarms();
  void sampleHeartRate();
  void resetStepsIfNeeded();
  void playAlarmAlert();
  void playTone(uint16_t frequency, uint32_t durationMs);
  bool readMax30102Sample(uint32_t &red, uint32_t &ir);
  uint8_t calculateSpO2(uint32_t red, uint32_t ir) const;
  bool writeMax30102Reg(uint8_t reg, uint8_t value);
  uint8_t readMax30102Reg(uint8_t reg);
  uint32_t currentStepCount();
  void pollButtons();

  void startWifiPortal();
  void stopWifiPortal();
  void startLanServer();
  void stopLanServer();
  bool connectWifiLan(uint32_t timeoutMs = 15000);
  void prepareForWifiHotspot();
  void resumeAfterWifiHotspot();
  void startBleConfig();
  void stopBleConfig();
  void updateBleStatus(const String &message = "");
  String buildBleStatus() const;
  String applyConfigCommand(const String &command);
  bool setClockFromText(const String &dateTime);
  bool deleteBook(uint8_t id);
  bool beginBleBookUpload(const String &payload);
  bool writeBleBookChunk(const String &payload);
  bool finishBleBookUpload();
  void abortBleBookUpload();
  String safeUploadName(const String &name) const;
  bool isPlaceholderUploadName(const String &name) const;
  String nextBookFileName() const;
  String jsonEscape(const String &value) const;
  void handleWebRoot();
  void handleAddAlarm();
  void handleToggleAlarm();
  void handleDeleteAlarm();
  void handleSyncSettings();
  void handleWifiSettings();
  void handleBookUpload();
  void handleBookChunkBegin();
  void handleBookChunkStatus();
  void handleBookChunkWrite();
  void handleBookChunkFinish();
  void handleDeleteBook();
  String alarmLabel(const AlarmEntry &alarm) const;
  String twoDigits(uint8_t value) const;
};

#endif


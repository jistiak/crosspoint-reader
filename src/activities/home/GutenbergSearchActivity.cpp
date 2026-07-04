#include "GutenbergSearchActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cctype>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 18;

std::string urlEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}
}  // namespace

void GutenbergSearchActivity::onEnter() {
  Activity::onEnter();
  results.clear();
  selectorIndex = 0;
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();
  checkAndConnectWifi();
}

void GutenbergSearchActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void GutenbergSearchActivity::loop() {
  if (state == State::WIFI_SELECTION || state == State::SEARCH_INPUT || state == State::SEARCHING ||
      state == State::DOWNLOADING) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (lastQuery.empty()) {
        launchSearch();
      } else {
        performSearch(lastQuery);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    launchSearch();
    return;
  }

  if (state == State::RESULTS) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !results.empty()) {
      downloadBook(results[selectorIndex]);
      return;
    }
    if (!results.empty()) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, results.size());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, results.size());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, results.size(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, results.size(), PAGE_ITEMS);
        requestUpdate();
      });
    }
  }
}

void GutenbergSearchActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_BOOK_SEARCH), true, EpdFontFamily::BOLD);

  if (state == State::CHECK_WIFI || state == State::SEARCHING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10,
                              renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40).c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{50, pageHeight / 2 + 20, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_SEARCH), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (results.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);
    for (size_t i = pageStartIndex; i < results.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); ++i) {
      const auto& book = results[i];
      std::string displayText = book.title;
      if (!book.author.empty()) displayText += " - " + book.author;
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

void GutenbergSearchActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    launchSearch();
    return;
  }
  launchWifiSelection();
}

void GutenbergSearchActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void GutenbergSearchActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    launchSearch();
  } else {
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

void GutenbergSearchActivity::launchSearch() {
  consumeConfirm = true;
  state = State::SEARCH_INPUT;
  requestUpdate();
  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_BOOK_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else if (results.empty()) {
      onGoHome();
    } else {
      state = State::RESULTS;
      requestUpdate();
    }
  });
}

void GutenbergSearchActivity::performSearch(const std::string& query) {
  if (query.empty()) {
    state = results.empty() ? State::ERROR : State::RESULTS;
    errorMessage = tr(STR_NO_ENTRIES);
    requestUpdate();
    return;
  }
  lastQuery = query;
  state = State::SEARCHING;
  statusMessage = tr(STR_LOADING);
  results.clear();
  selectorIndex = 0;
  requestUpdate(true);

  std::string json;
  const std::string url = "https://gutendex.com/books/?mime_type=application%2Fepub%2Bzip&search=" + urlEncode(query);
  if (!HttpDownloader::fetchUrl(url, json)) {
    state = State::ERROR;
    errorMessage = tr(STR_FETCH_FEED_FAILED);
    requestUpdate();
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    state = State::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  for (JsonObject item : doc["results"].as<JsonArray>()) {
    const char* title = item["title"] | "";
    std::string author;
    JsonArray authors = item["authors"].as<JsonArray>();
    if (!authors.isNull() && authors.size() > 0) author = authors[0]["name"] | "";
    const char* epub = item["formats"]["application/epub+zip"] | "";
    if (title[0] != '\0' && epub[0] != '\0') results.push_back(BookResult{title, author, epub});
    if (results.size() >= 50) break;
  }

  state = results.empty() ? State::ERROR : State::RESULTS;
  if (results.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

std::string GutenbergSearchActivity::getDownloadFolder() const {
  std::string folder = SETTINGS.gutenbergFolder;
  if (folder.empty()) folder = "gutenberg";
  while (!folder.empty() && folder.front() == '/') folder.erase(folder.begin());
  while (!folder.empty() && folder.back() == '/') folder.pop_back();
  if (folder.empty()) folder = "gutenberg";
  return "/" + StringUtils::sanitizeFilename(folder);
}

bool GutenbergSearchActivity::ensureDownloadFolder(const std::string& folder) {
  if (Storage.exists(folder.c_str())) return true;
  return Storage.mkdir(folder.c_str());
}

void GutenbergSearchActivity::downloadBook(const BookResult& book) {
  state = State::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  requestUpdate(true);

  const std::string folder = getDownloadFolder();
  if (!ensureDownloadFolder(folder)) {
    state = State::ERROR;
    errorMessage = "Folder create failed";
    requestUpdate();
    return;
  }

  std::string filename = folder + "/" +
                         StringUtils::sanitizeFilename((book.author.empty() ? "" : book.author + " - ") + book.title) +
                         ".epub";
  LOG_DBG("GUT", "Downloading: %s -> %s", book.downloadUrl.c_str(), filename.c_str());
  const auto result = HttpDownloader::downloadToFile(
      book.downloadUrl, filename,
      [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        requestUpdate(true);
      });

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    state = State::RESULTS;
  } else {
    state = State::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

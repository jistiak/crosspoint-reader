#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class GutenbergSearchActivity final : public Activity {
 public:
  enum class State { CHECK_WIFI, WIFI_SELECTION, SEARCH_INPUT, SEARCHING, RESULTS, DOWNLOADING, ERROR };

  explicit GutenbergSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("GutenbergSearch", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct BookResult {
    std::string title;
    std::string author;
    std::string downloadUrl;
  };

  ButtonNavigator buttonNavigator;
  State state = State::CHECK_WIFI;
  std::vector<BookResult> results;
  int selectorIndex = 0;
  std::string statusMessage;
  std::string errorMessage;
  std::string lastQuery;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool consumeConfirm = false;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void launchSearch();
  void performSearch(const std::string& query);
  void downloadBook(const BookResult& book);
  std::string getDownloadFolder() const;
  bool ensureDownloadFolder(const std::string& folder);
  bool preventAutoSleep() override { return true; }
};

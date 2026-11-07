#pragma once

#include <string>
#include <vector>

#include "AppLabFiles.h"
#include "activities/UiListActivity.h"

// App Lab: describe an app, Gemini writes it, the Sticky runs it.
// Rows: New app, a prompt sent from the phone page (when there is one), the
// saved apps, then Settings.
class AppLabActivity final : public UiListActivity {
 public:
  AppLabActivity(GfxRenderer& renderer, MappedInputManager& input) : UiListActivity("AppLab", renderer, input) {}
  void onEnter() override;

 private:
  enum class RowKind : uint8_t { New, FromPhone, App, Settings };
  struct Row {
    RowKind kind;
    size_t app;  // index into apps_ for RowKind::App
  };

  std::vector<applab::AppInfo> apps_;
  std::vector<Row> rows_;
  std::vector<freeink::ui::ListItem> items_;
  std::string pendingPrompt_;
  std::string pendingLabel_;

  void reload();
  void requestNewApp();
  void generate(int kind, std::string request, std::string appId = "");
  void openApp(const std::string& id);
  void showAppMenu(size_t app);

  int listCount() const override { return static_cast<int>(rows_.size()); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
};

// Open / Change it / Delete for one saved app. Finishes with MenuResult{0..2}.
class AppLabAppMenuActivity final : public UiListActivity {
 public:
  AppLabAppMenuActivity(GfxRenderer& renderer, MappedInputManager& input, std::string title)
      : UiListActivity("AppLabAppMenu", renderer, input), title_(std::move(title)) {}

 private:
  std::string title_;
  freeink::ui::ListItem items_[3]{};

  int listCount() const override { return 3; }
  const char* headerTitle() const override { return title_.c_str(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
};

// Gemini API key and model.
class AppLabSettingsActivity final : public UiListActivity {
 public:
  AppLabSettingsActivity(GfxRenderer& renderer, MappedInputManager& input)
      : UiListActivity("AppLabSettings", renderer, input) {}

 private:
  std::string values_[2];
  freeink::ui::ListItem items_[2]{};

  int listCount() const override { return 2; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
};

#pragma once

#include <I18n.h>

#include <string>

#include "activities/UiListActivity.h"
#include "activities/util/KeyboardEntryActivity.h"

// Setup for the Hermes chat app: chat API server, speech-to-text endpoint,
// voice behaviour, and starting a fresh server-side conversation.
class HermesSettingsActivity final : public UiListActivity {
 public:
  explicit HermesSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 13;

  // True once after the user started a new conversation (the chat screen then
  // clears its local transcript).
  static bool takeNewConversationRequest();

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void editText(StrId title, std::string& field, size_t maxLength, InputType type);
  void save();

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};

#include "HermesSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "HermesStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

enum Row : int {
  kServerUrl,
  kApiKey,
  kModel,
  kConversation,
  kApiStyle,
  kTimeout,
  kSttUrl,
  kSttKey,
  kSttModel,
  kSttLanguage,
  kAutoSend,
  kMaxRecord,
  kNewChat,
  kRowCount,
};
static_assert(kRowCount == HermesSettingsActivity::MENU_ITEMS, "row table and MENU_ITEMS must agree");

const StrId kRowNames[kRowCount] = {
    StrId::STR_HERMES_SERVER_URL, StrId::STR_HERMES_API_KEY,      StrId::STR_HERMES_MODEL,
    StrId::STR_HERMES_CONVERSATION, StrId::STR_HERMES_API_STYLE,  StrId::STR_HERMES_TIMEOUT,
    StrId::STR_HERMES_STT_URL,    StrId::STR_HERMES_STT_KEY,      StrId::STR_HERMES_STT_MODEL,
    StrId::STR_HERMES_STT_LANGUAGE, StrId::STR_HERMES_AUTO_SEND,  StrId::STR_HERMES_MAX_RECORD,
    StrId::STR_HERMES_NEW_CHAT,
};

bool newConversationRequested = false;

// "sticky" -> "sticky-2", "sticky-2" -> "sticky-3".
std::string nextConversationName(const std::string& current) {
  std::string base = current.empty() ? "sticky" : current;
  unsigned number = 1;
  const size_t dash = base.find_last_of('-');
  if (dash != std::string::npos && dash + 1 < base.size() && isdigit(static_cast<unsigned char>(base[dash + 1]))) {
    bool allDigits = true;
    for (size_t i = dash + 1; i < base.size(); ++i) allDigits = allDigits && isdigit(static_cast<unsigned char>(base[i]));
    if (allDigits) {
      number = static_cast<unsigned>(strtoul(base.c_str() + dash + 1, nullptr, 10));
      base.resize(dash);
    }
  }
  return base + "-" + std::to_string(number + 1);
}

std::string secondsLabel(const unsigned seconds) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), tr(STR_HERMES_SECONDS), seconds);
  return buffer;
}

}  // namespace

bool HermesSettingsActivity::takeNewConversationRequest() {
  const bool requested = newConversationRequested;
  newConversationRequested = false;
  return requested;
}

HermesSettingsActivity::HermesSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("HermesSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(kRowNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

int HermesSettingsActivity::listCount() const { return MENU_ITEMS; }

const char* HermesSettingsActivity::headerTitle() const { return tr(STR_HERMES_SETTINGS_TITLE); }

void HermesSettingsActivity::save() {
  if (!HERMES_STORE.saveToFile()) LOG_ERR("HERMES", "Failed to save settings");
}

void HermesSettingsActivity::editText(const StrId title, std::string& field, const size_t maxLength,
                                      const InputType type) {
  std::string* target = &field;  // HERMES_STORE is a singleton; the field outlives the keyboard
  startActivityForResultWith<KeyboardEntryActivity>(
      [this, target](const ActivityResult& result) {
        if (result.isCancelled) return;
        *target = std::get<KeyboardResult>(result.data).text;
        save();
      },
      I18N.get(title), field, maxLength, type);
}

void HermesSettingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  auto& store = HERMES_STORE;
  switch (index) {
    case kServerUrl:
      if (store.chatUrl.empty()) {
        store.chatUrl = "http://";  // prefill to save typing; kept only if confirmed
        editText(StrId::STR_HERMES_SERVER_URL, store.chatUrl, 160, InputType::Url);
        store.chatUrl.clear();
        return;
      }
      editText(StrId::STR_HERMES_SERVER_URL, store.chatUrl, 160, InputType::Url);
      return;
    case kApiKey:
      editText(StrId::STR_HERMES_API_KEY, store.chatKey, 200, InputType::Password);
      return;
    case kModel:
      editText(StrId::STR_HERMES_MODEL, store.chatModel, 64, InputType::Text);
      return;
    case kConversation:
      editText(StrId::STR_HERMES_CONVERSATION, store.conversation, 64, InputType::Text);
      return;
    case kApiStyle:
      store.cycleChatApi();
      break;
    case kTimeout:
      store.cycleTimeout();
      break;
    case kSttUrl: {
      // Prefill the usual OpenAI-compatible path on the Hermes host.
      if (store.sttUrl.empty()) {
        std::string host = store.chatUrl.empty() ? std::string("http://") : store.chatUrl;
        const size_t scheme = host.find("://");
        const size_t port = host.find(':', scheme == std::string::npos ? 0 : scheme + 3);
        if (port != std::string::npos) host.resize(port);
        store.sttUrl = host + ":8000/v1/audio/transcriptions";
        editText(StrId::STR_HERMES_STT_URL, store.sttUrl, 160, InputType::Url);
        store.sttUrl.clear();  // only kept if the user confirms
        return;
      }
      editText(StrId::STR_HERMES_STT_URL, store.sttUrl, 160, InputType::Url);
      return;
    }
    case kSttKey:
      editText(StrId::STR_HERMES_STT_KEY, store.sttKey, 200, InputType::Password);
      return;
    case kSttModel:
      editText(StrId::STR_HERMES_STT_MODEL, store.sttModel, 64, InputType::Text);
      return;
    case kSttLanguage:
      editText(StrId::STR_HERMES_STT_LANGUAGE, store.sttLanguage, 8, InputType::Text);
      return;
    case kAutoSend:
      store.voiceAutoSend = !store.voiceAutoSend;
      break;
    case kMaxRecord:
      store.cycleMaxRecord();
      break;
    case kNewChat:
      store.conversation = nextConversationName(store.conversation);
      newConversationRequested = true;
      break;
    default:
      return;
  }
  save();
  requestUpdate();
}

void HermesSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const auto& store = HERMES_STORE;
  const auto orNotSet = [](const std::string& value) -> std::string { return value.empty() ? tr(STR_NOT_SET) : value; };
  const auto masked = [](const std::string& value) -> std::string {
    return value.empty() ? tr(STR_NOT_SET) : std::string("******");
  };
  for (int i = 0; i < MENU_ITEMS; i++) {
    switch (i) {
      case kServerUrl:
        rowValues_[i] = orNotSet(store.chatUrl);
        break;
      case kApiKey:
        rowValues_[i] = masked(store.chatKey);
        break;
      case kModel:
        rowValues_[i] = orNotSet(store.chatModel);
        break;
      case kConversation:
        rowValues_[i] = orNotSet(store.conversation);
        break;
      case kApiStyle:
        rowValues_[i] = store.chatApi == HermesStore::ChatApi::Responses ? tr(STR_HERMES_API_RESPONSES)
                                                                          : tr(STR_HERMES_API_CHAT);
        break;
      case kTimeout:
        rowValues_[i] = secondsLabel(store.timeoutSeconds);
        break;
      case kSttUrl:
        rowValues_[i] = orNotSet(store.sttUrl);
        break;
      case kSttKey:
        rowValues_[i] = masked(store.sttKey);
        break;
      case kSttModel:
        rowValues_[i] = orNotSet(store.sttModel);
        break;
      case kSttLanguage:
        rowValues_[i] = store.sttLanguage.empty() ? tr(STR_HERMES_AUTO) : store.sttLanguage;
        break;
      case kAutoSend:
        rowValues_[i] = store.voiceAutoSend ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        break;
      case kMaxRecord:
        rowValues_[i] = secondsLabel(store.maxRecordSeconds);
        break;
      default:
        rowValues_[i].clear();
        break;
    }
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

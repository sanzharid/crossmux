#include "AppLabActivity.h"

#include <I18n.h>
#include <Logging.h>

#include "AppLabGenerateActivity.h"
#include "AppLabProtocol.h"
#include "AppLabStore.h"
#include "LabAppActivity.h"
#include "activities/apps/hermes/HermesWebUpdate.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {

constexpr size_t kMaxRequestChars = 1000;
constexpr size_t kPreviewChars = 60;

void setListMargins(UiAppHost::UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin({static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                           static_cast<int16_t>(metrics.buttonHintsHeight), 0});
}

}  // namespace

// ---------------------------------------------------------------------------
// AppLabActivity

void AppLabActivity::onEnter() {
  UiListActivity::onEnter();
  APPLAB_STORE.loadFromFile();
  reload();
}

void AppLabActivity::reload() {
  {
    RenderLock lock(*this);
    apps_ = applab::listApps();
    applab::readPendingPrompt(pendingPrompt_);
    pendingLabel_ =
        pendingPrompt_.size() > kPreviewChars ? pendingPrompt_.substr(0, kPreviewChars) + "..." : pendingPrompt_;
    rows_.clear();
    rows_.push_back({RowKind::New, 0});
    if (!pendingPrompt_.empty()) rows_.push_back({RowKind::FromPhone, 0});
    for (size_t i = 0; i < apps_.size(); ++i) rows_.push_back({RowKind::App, i});
    rows_.push_back({RowKind::Settings, 0});

    items_.assign(rows_.size(), fui::ListItem{});
    for (size_t i = 0; i < rows_.size(); ++i) {
      auto& item = items_[i];
      item.actionValue = static_cast<int16_t>(i);
      switch (rows_[i].kind) {
        case RowKind::New:
          item.label = tr(STR_APPLAB_NEW);
          item.subtitle = APPLAB_STORE.hasKey() ? tr(STR_APPLAB_NEW_HINT) : tr(STR_APPLAB_NO_KEY);
          break;
        case RowKind::FromPhone:
          item.label = tr(STR_APPLAB_FROM_PHONE);
          item.subtitle = pendingLabel_.c_str();
          break;
        case RowKind::App:
          item.label = apps_[rows_[i].app].name.c_str();
          break;
        case RowKind::Settings:
          item.label = tr(STR_APPLAB_SETTINGS);
          break;
      }
    }
  }
  requestUpdate();
}

const char* AppLabActivity::headerTitle() const { return tr(STR_APPLAB_TITLE); }

void AppLabActivity::buildScreen(UiScreen& screen) {
  setListMargins(screen);
  fui::ListProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void AppLabActivity::activateIndex(const int index) {
  if (index < 0 || static_cast<size_t>(index) >= rows_.size()) return;
  app.clearTapFlash();
  const Row row = rows_[static_cast<size_t>(index)];
  switch (row.kind) {
    case RowKind::New:
      requestNewApp();
      return;
    case RowKind::FromPhone:
      generate(static_cast<int>(applab::RequestKind::Create), pendingPrompt_);
      return;
    case RowKind::App:
      showAppMenu(row.app);
      return;
    case RowKind::Settings:
      startActivityForResultWith<AppLabSettingsActivity>([this](const ActivityResult&) { reload(); });
      return;
  }
}

void AppLabActivity::requestNewApp() {
  if (!APPLAB_STORE.hasKey()) {
    startActivityForResultWith<AppLabSettingsActivity>([this](const ActivityResult&) { reload(); });
    return;
  }
  startActivityForResultWith<KeyboardEntryActivity>(
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const std::string text = std::get<KeyboardResult>(result.data).text;
        if (!text.empty()) generate(static_cast<int>(applab::RequestKind::Create), text);
      },
      tr(STR_APPLAB_DESCRIBE), "", kMaxRequestChars);
}

void AppLabActivity::generate(const int kind, std::string request, std::string appId) {
  const bool fromPhone = !pendingPrompt_.empty() && request == pendingPrompt_;
  startActivityForResultWith<AppLabGenerateActivity>(
      [this, fromPhone](const ActivityResult& result) {
        if (result.isCancelled) {
          reload();
          return;
        }
        if (fromPhone) applab::clearPendingPrompt();
        reload();
        openApp(std::get<FilePathResult>(result.data).path);
      },
      static_cast<applab::RequestKind>(kind), std::move(request), std::move(appId));
}

void AppLabActivity::openApp(const std::string& id) {
  startActivityForResultWith<LabAppActivity>([this](const ActivityResult&) { reload(); }, id);
}

void AppLabActivity::showAppMenu(const size_t app) {
  if (app >= apps_.size()) return;
  const std::string id = apps_[app].id;
  const std::string name = apps_[app].name;
  startActivityForResultWith<AppLabAppMenuActivity>(
      [this, id, name](const ActivityResult& result) {
        if (result.isCancelled) return;
        switch (std::get<MenuResult>(result.data).action) {
          case 0:
            openApp(id);
            break;
          case 1:
            startActivityForResultWith<KeyboardEntryActivity>(
                [this, id](const ActivityResult& change) {
                  if (change.isCancelled) return;
                  const std::string text = std::get<KeyboardResult>(change.data).text;
                  if (!text.empty()) generate(static_cast<int>(applab::RequestKind::Change), text, id);
                },
                tr(STR_APPLAB_CHANGE_PROMPT), "", kMaxRequestChars);
            break;
          case 2:
            startActivityForResultWith<ConfirmationActivity>(
                [this, id](const ActivityResult& confirm) {
                  if (confirm.isCancelled) return;
                  if (!applab::deleteApp(id)) LOG_ERR("APPLAB", "Could not delete %s", id.c_str());
                  reload();
                },
                tr(STR_APPLAB_DELETE_CONFIRM), name);
            break;
          default:
            break;
        }
      },
      name);
}

// ---------------------------------------------------------------------------
// AppLabAppMenuActivity

void AppLabAppMenuActivity::buildScreen(UiScreen& screen) {
  setListMargins(screen);
  items_[0] = {};
  items_[0].label = tr(STR_OPEN);
  items_[0].actionValue = 0;
  items_[1] = {};
  items_[1].label = tr(STR_APPLAB_CHANGE);
  items_[1].subtitle = tr(STR_APPLAB_CHANGE_HINT);
  items_[1].actionValue = 1;
  items_[2] = {};
  items_[2].label = tr(STR_DELETE);
  items_[2].actionValue = 2;
  fui::ListProps props;
  props.items = items_;
  props.count = 3;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void AppLabAppMenuActivity::activateIndex(const int index) {
  if (index < 0 || index > 2) return;
  app.clearTapFlash();
  MenuResult result;
  result.action = index;
  setResult(std::move(result));
  finish();
}

// ---------------------------------------------------------------------------
// AppLabSettingsActivity

const char* AppLabSettingsActivity::headerTitle() const { return tr(STR_APPLAB_SETTINGS); }

void AppLabSettingsActivity::buildScreen(UiScreen& screen) {
  setListMargins(screen);
  const auto& store = APPLAB_STORE;
  values_[0] = store.hasKey() ? std::string("******") : std::string(tr(STR_NOT_SET));
  values_[1] = store.model;
  items_[0] = {};
  items_[0].label = tr(STR_APPLAB_KEY);
  items_[0].value = values_[0].c_str();
  items_[0].actionValue = 0;
  items_[1] = {};
  items_[1].label = tr(STR_APPLAB_MODEL);
  items_[1].value = values_[1].c_str();
  items_[1].actionValue = 1;
  fui::ListProps props;
  props.items = items_;
  props.count = 2;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

void AppLabSettingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index == 0) {
    startActivityForResultWith<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          if (result.isCancelled) return;
          const std::string key = hermesweb::trimmed(std::get<KeyboardResult>(result.data).text);
          if (!hermesweb::isValidKey(key)) return;  // spaces or control characters: keep the old key
          APPLAB_STORE.geminiKey = key;
          if (!APPLAB_STORE.saveToFile()) LOG_ERR("APPLAB", "Failed to save settings");
          requestUpdate();
        },
        tr(STR_APPLAB_KEY), "", 200, InputType::Password);
  } else if (index == 1) {
    startActivityForResultWith<KeyboardEntryActivity>(
        [this](const ActivityResult& result) {
          if (result.isCancelled) return;
          const std::string model = std::get<KeyboardResult>(result.data).text;
          APPLAB_STORE.model = applab::isValidModel(model) ? model : AppLabStore::kDefaultModel;
          if (!APPLAB_STORE.saveToFile()) LOG_ERR("APPLAB", "Failed to save settings");
          requestUpdate();
        },
        tr(STR_APPLAB_MODEL), APPLAB_STORE.model, 64, InputType::Text);
  }
}

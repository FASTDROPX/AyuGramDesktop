// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
// Boxes, menu items and alerts of plugins.
#pragma once

#include "ayu/plugins/plugins_engine.h"

class HistoryItem;
class PeerData;

namespace style {
struct icon;
} // namespace style

namespace Ui {
class PopupMenu;
class VerticalLayout;
} // namespace Ui

namespace Ui::Menu {
struct MenuCallback;
} // namespace Ui::Menu

namespace Window {
class SessionController;
} // namespace Window

namespace AyuPlugins {

// The window that is in front, if any.
[[nodiscard]] Window::SessionController *ActiveController();

// Android drawable names (msg_settings, msg_copy, ...) to menu icons.
[[nodiscard]] const style::icon *IconFor(const QString &androidName);

void ShowAlert(const AlertEvent &event);
void ShowPluginBox(const QString &pluginId);
void ShowPluginSettings(
	const QString &pluginId,
	const QString &subPageToken = QString(),
	const QString &title = QString());
void ShowPluginLog(const QString &pluginId);

// Asks first, installs, then opens the plugins section.
void InstallWithConfirmation(const QString &path, const QString &name);
void InstallNow(const QString &path);
void ChooseAndInstall();
void OpenPluginsSection();

// Plugin menu items (MenuItemType on Android).
void AddMessageMenuItems(not_null<Ui::PopupMenu*> menu, HistoryItem *item);
void AddPeerMenuItems(PeerData *peer, const Ui::Menu::MenuCallback &addAction);
void AddMainMenuItems(not_null<Ui::VerticalLayout*> menu);

} // namespace AyuPlugins

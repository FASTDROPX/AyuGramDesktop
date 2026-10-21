// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
#include "ayu/plugins/plugins_ui.h"

#include "ayu/plugins/plugins_hooks.h"
#include "ayu/plugins/plugins_lang.h"
#include "ayu/plugins/plugins_section.h"
#include "ayu/ui/boxes/plugin_info_box.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_peer.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mainwindow.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "styles/style_window.h"

#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QCursor>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>

namespace AyuPlugins {
namespace {

constexpr auto kAlertLoading = 1;
constexpr auto kAlertSpinner = 2;
constexpr auto kButtonPositive = -1;
constexpr auto kButtonNegative = -2;
constexpr auto kButtonNeutral = -3;

struct AlertState {
	std::map<QString, base::weak_qptr<Ui::GenericBox>> boxes;
	base::flat_set<QString> silentClose;
};

[[nodiscard]] AlertState &Alerts() {
	static auto result = AlertState();
	return result;
}

template <typename BoxType>
base::weak_qptr<BoxType> ShowBox(object_ptr<BoxType> box) {
	if (!Core::IsAppLaunched()) {
		return {};
	}
	const auto window = Core::App().activePrimaryWindow();
	if (!window) {
		return {};
	}
	return window->uiShow()->show(std::move(box));
}

[[nodiscard]] style::margins LabelPadding() {
	const auto &padding = st::settingsButtonNoIcon.padding;
	return style::margins(padding.left(), 0, padding.right(), padding.bottom());
}

void AddSubtext(not_null<Ui::VerticalLayout*> content, const QString &text) {
	if (text.isEmpty()) {
		return;
	}
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			rpl::single(text),
			st::boxDividerLabel),
		LabelPadding());
}

[[nodiscard]] not_null<Ui::SettingsButton*> AddRow(
		not_null<Ui::VerticalLayout*> content,
		const QString &text,
		const QString &icon) {
	if (icon.isEmpty()) {
		return Settings::AddButtonWithIcon(
			content,
			rpl::single(text),
			st::settingsButtonNoIcon);
	}
	return Settings::AddButtonWithIcon(
		content,
		rpl::single(text),
		st::settingsButton,
		{ IconFor(icon) });
}

[[nodiscard]] QJsonObject PeerContext(not_null<PeerData*> peer) {
	const auto dialogId = DialogIdFromPeer(peer->id);
	auto result = QJsonObject{
		{ u"dialog_id"_q, dialogId },
	};
	if (peer->isUser()) {
		result.insert(u"userId"_q, dialogId);
	} else {
		result.insert(u"chatId"_q, -dialogId);
	}
	return result;
}

[[nodiscard]] QJsonObject MessageContext(not_null<HistoryItem*> item) {
	const auto peer = item->history()->peer;
	const auto from = item->from();
	auto result = PeerContext(peer);
	result.insert(u"message"_q, QJsonObject{
		{ u"id"_q, qint64(item->id.bare) },
		{ u"dialog_id"_q, DialogIdFromPeer(peer->id) },
		{ u"peer_type"_q, PeerTypeName(peer->id) },
		{ u"text"_q, item->originalText().text },
		{ u"date"_q, qint64(item->date()) },
		{ u"out"_q, item->out() },
		{ u"from_id"_q, DialogIdFromPeer(from->id) },
		{ u"from_type"_q, PeerTypeName(from->id) },
		{ u"reply_to_msg_id"_q, qint64(item->replyToId().bare) },
	});
	return result;
}

void FillAlertBox(not_null<Ui::GenericBox*> box, AlertEvent event) {
	const auto pluginId = event.pluginId;
	const auto token = event.token;
	const auto &state = event.state;
	const auto send = [=](QString kind, int which) {
		Engine::Instance().invokeCallback(
			pluginId,
			token,
			QJsonArray{ kind, which });
	};

	box->setWidth(st::boxWideWidth);
	const auto title = state.value(u"title"_q).toString();
	if (!title.isEmpty()) {
		box->setTitle(rpl::single(title));
	}
	auto text = state.value(u"message"_q).toString();
	const auto style = state.value(u"style"_q).toInt();
	if (style == kAlertLoading) {
		text += (text.isEmpty() ? QString() : u"\n\n"_q)
			+ QString::number(state.value(u"progress"_q).toInt())
			+ '%';
	} else if (style == kAlertSpinner && text.isEmpty()) {
		text = u"…"_q;
	}
	if (!text.isEmpty()) {
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(text),
			st::boxLabel));
		label->setSelectable(true);
	}
	const auto items = state.value(u"items"_q).toArray();
	if (!items.isEmpty()) {
		Ui::AddSkip(box->verticalLayout());
		for (auto i = 0; i != int(items.size()); ++i) {
			const auto button = Settings::AddButtonWithIcon(
				box->verticalLayout(),
				rpl::single(items[i].toString()),
				st::settingsButtonNoIcon);
			button->setClickedCallback([=] {
				send(u"item"_q, i);
				box->closeBox();
			});
		}
	}
	const auto cancelable = state.value(u"cancelable"_q).toBool(true);
	box->setCloseByEscape(cancelable);
	box->setCloseByOutsideClick(cancelable);

	const auto buttons = state.value(u"buttons"_q).toObject();
	auto red = base::flat_set<int>();
	for (const auto value : state.value(u"red"_q).toArray()) {
		red.emplace(value.toInt());
	}
	const auto add = [&](int which, bool left) {
		const auto buttonText = buttons.value(
			QString::number(which)).toString();
		if (buttonText.isEmpty()) {
			return;
		}
		const auto callback = [=] {
			send(u"button"_q, which);
			box->closeBox();
		};
		const auto &buttonSt = red.contains(which)
			? st::attentionBoxButton
			: st::defaultBoxButton;
		if (left) {
			box->addLeftButton(rpl::single(buttonText), callback, buttonSt);
		} else {
			box->addButton(rpl::single(buttonText), callback, buttonSt);
		}
	};
	add(kButtonPositive, false);
	add(kButtonNegative, false);
	add(kButtonNeutral, true);
	if (buttons.isEmpty() && items.isEmpty() && cancelable) {
		box->addButton(rpl::single(Tr("Close")), [=] { box->closeBox(); });
	}

	box->boxClosing() | rpl::on_next([=] {
		auto &alerts = Alerts();
		alerts.boxes.erase(token);
		if (!alerts.silentClose.remove(token)) {
			send(u"dismiss"_q, 0);
		}
	}, box->lifetime());
}

void FillSettingsBox(
		not_null<Ui::GenericBox*> box,
		QString pluginId,
		QString subPageToken,
		QString title,
		QJsonArray items,
		QJsonObject values) {
	box->setTitle(rpl::single(title));
	box->setWidth(st::boxWideWidth);
	const auto content = box->verticalLayout();
	if (items.isEmpty()) {
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(Tr("Error")),
			st::boxDividerLabel));
	}
	for (const auto &entry : items) {
		const auto item = entry.toObject();
		const auto kind = item.value(u"kind"_q).toString();
		const auto text = item.value(u"text"_q).toString();
		const auto subtext = item.value(u"subtext"_q).toString();
		const auto icon = item.value(u"icon"_q).toString();
		const auto key = item.value(u"key"_q).toString();
		const auto onChange = item.value(u"on_change"_q).toString();
		const auto current = values.contains(key)
			? values.value(key)
			: item.value(u"default"_q);
		if (kind == u"header"_q) {
			Ui::AddSubsectionTitle(content, rpl::single(text));
		} else if (kind == u"divider"_q) {
			if (text.isEmpty()) {
				Ui::AddSkip(content);
				Ui::AddDivider(content);
				Ui::AddSkip(content);
			} else {
				Ui::AddDividerText(content, rpl::single(text));
			}
		} else if (kind == u"switch"_q) {
			const auto button = AddRow(content, text, icon);
			button->toggleOn(rpl::single(current.toBool()));
			button->toggledChanges(
			) | rpl::on_next([=](bool on) {
				Engine::Instance().setSettingFromUi(pluginId, key, on, onChange);
			}, button->lifetime());
			AddSubtext(content, subtext);
		} else if (kind == u"selector"_q) {
			auto options = QStringList();
			for (const auto &option : item.value(u"items"_q).toArray()) {
				options.push_back(option.toString());
			}
			const auto index = content->lifetime().make_state<
				rpl::variable<int>>(current.toInt());
			const auto menu = content->lifetime().make_state<
				base::unique_qptr<Ui::PopupMenu>>();
			const auto button = Settings::AddButtonWithLabel(
				content,
				rpl::single(text),
				index->value() | rpl::map([=](int i) {
					return (i >= 0 && i < options.size())
						? options[i]
						: QString();
				}),
				icon.isEmpty() ? st::settingsButtonNoIcon : st::settingsButton,
				{ icon.isEmpty() ? nullptr : IconFor(icon) });
			button->setClickedCallback([=] {
				*menu = base::make_unique_q<Ui::PopupMenu>(
					button,
					st::defaultPopupMenu);
				for (auto i = 0; i != options.size(); ++i) {
					(*menu)->addAction(options[i], [=] {
						*index = i;
						Engine::Instance().setSettingFromUi(
							pluginId,
							key,
							i,
							onChange);
					});
				}
				(*menu)->popup(QCursor::pos());
			});
		} else if (kind == u"input"_q) {
			const auto value = content->lifetime().make_state<
				rpl::variable<QString>>(current.toString());
			const auto button = Settings::AddButtonWithLabel(
				content,
				rpl::single(text),
				value->value(),
				icon.isEmpty() ? st::settingsButtonNoIcon : st::settingsButton,
				{ icon.isEmpty() ? nullptr : IconFor(icon) });
			button->setClickedCallback([=] {
				ShowBox(Box([=](not_null<Ui::GenericBox*> input) {
					input->setTitle(rpl::single(text));
					const auto field = input->addRow(
						object_ptr<Ui::InputField>(
							input,
							st::defaultInputField,
							rpl::single(subtext),
							value->current()));
					const auto save = [=] {
						*value = field->getLastText();
						Engine::Instance().setSettingFromUi(
							pluginId,
							key,
							field->getLastText(),
							onChange);
						input->closeBox();
					};
					field->submits() | rpl::on_next(save, field->lifetime());
					input->setFocusCallback([=] { field->setFocusFast(); });
					input->addButton(rpl::single(Tr("Save")), save);
					input->addButton(
						rpl::single(Tr("Close")),
						[=] { input->closeBox(); });
				}));
			});
			AddSubtext(content, subtext);
		} else if (kind == u"edit_text"_q) {
			const auto multiline = item.value(u"multiline"_q).toBool();
			const auto field = content->add(
				object_ptr<Ui::InputField>(
					content,
					st::defaultInputField,
					(multiline
						? Ui::InputField::Mode::MultiLine
						: Ui::InputField::Mode::SingleLine),
					rpl::single(item.value(u"hint"_q).toString()),
					current.toString()),
				st::boxRowPadding);
			const auto maxLength = item.value(u"max_length"_q).toInt();
			if (maxLength > 0) {
				field->setMaxLength(maxLength);
			}
			field->changes() | rpl::on_next([=] {
				Engine::Instance().setSettingFromUi(
					pluginId,
					key,
					field->getLastText(),
					onChange);
			}, field->lifetime());
			Ui::AddSkip(content);
		} else if (kind == u"text"_q) {
			const auto button = AddRow(content, text, icon);
			if (item.value(u"red"_q).toBool()) {
				button->setColorOverride(st::attentionButtonFg->c);
			} else if (item.value(u"accent"_q).toBool()) {
				button->setColorOverride(st::windowActiveTextFg->c);
			}
			const auto subPage = item.value(u"sub_page"_q).toString();
			const auto onClick = item.value(u"on_click"_q).toString();
			button->setClickedCallback([=] {
				if (!subPage.isEmpty()) {
					ShowPluginSettings(pluginId, subPage, text);
				} else if (!onClick.isEmpty()) {
					Engine::Instance().invokeCallback(
						pluginId,
						onClick,
						QJsonArray{ QJsonValue() });
				}
			});
			AddSubtext(content, subtext);
		} else {
			Ui::AddDividerText(
				content,
				rpl::single(Tr("AndroidOnly")
					+ (text.isEmpty() ? QString() : (u": "_q + text))));
		}
	}
	Ui::AddSkip(content);
	box->addButton(rpl::single(Tr("Close")), [=] { box->closeBox(); });

	Engine::Instance().settingsReloads(
	) | rpl::filter([=](const QString &id) {
		return (id == pluginId);
	}) | rpl::take(1) | rpl::on_next([=] {
		box->closeBox();
		ShowPluginSettings(pluginId, subPageToken, title);
	}, box->lifetime());
}

void FillPluginBox(not_null<Ui::GenericBox*> box, QString pluginId) {
	auto &engine = Engine::Instance();
	const auto info = engine.plugin(pluginId);
	box->setWidth(st::boxWideWidth);
	box->addButton(rpl::single(Tr("Close")), [=] { box->closeBox(); });
	if (!info) {
		return;
	}
	box->setTitle(rpl::single(info->name));
	const auto content = box->verticalLayout();

	auto about = Tr("Version", info->version);
	if (!info->author.isEmpty()) {
		about += u" • "_q + Tr("Author", info->author);
	}
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		rpl::single(about),
		st::boxDividerLabel));
	if (!info->description.isEmpty()) {
		Ui::AddSkip(content);
		const auto description = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(info->description),
			st::boxLabel));
		description->setSelectable(true);
	}
	if (!info->error.isEmpty()) {
		Ui::AddSkip(content);
		const auto error = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(Tr("Error") + u": "_q + info->error),
			st::boxLabel));
		error->setTextColorOverride(st::attentionButtonFg->c);
		error->setSelectable(true);
	}
	Ui::AddSkip(content);
	Ui::AddDivider(content);
	Ui::AddSkip(content);

	const auto toggle = Settings::AddButtonWithIcon(
		content,
		rpl::single(Tr("Enable")),
		st::settingsButtonNoIcon);
	toggle->toggleOn(rpl::single(info->enabled));
	toggle->toggledChanges(
	) | rpl::on_next([=](bool enabled) {
		Engine::Instance().setEnabled(pluginId, enabled);
	}, toggle->lifetime());

	if (info->loaded && info->hasSettings) {
		Settings::AddButtonWithIcon(
			content,
			rpl::single(Tr("Settings")),
			st::settingsButton,
			{ &st::menuIconSettings }
		)->setClickedCallback([=] {
			const auto now = Engine::Instance().plugin(pluginId);
			ShowPluginSettings(
				pluginId,
				QString(),
				now ? now->name : pluginId);
		});
	}
	Settings::AddButtonWithIcon(
		content,
		rpl::single(Tr("Log")),
		st::settingsButton,
		{ &st::menuIconFile }
	)->setClickedCallback([=] {
		ShowPluginLog(pluginId);
	});
	Settings::AddButtonWithIcon(
		content,
		rpl::single(Tr("Reload")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->setClickedCallback([=] {
		Engine::Instance().reload(pluginId);
		box->closeBox();
	});
	const auto path = info->path;
	Settings::AddButtonWithIcon(
		content,
		rpl::single(Tr("ShowFile")),
		st::settingsButton,
		{ &st::menuIconShowInFolder }
	)->setClickedCallback([=] {
		File::ShowInFolder(path);
	});
	const auto name = info->name;
	const auto remove = Settings::AddButtonWithIcon(
		content,
		rpl::single(Tr("Delete")),
		st::settingsAttentionButtonWithIcon,
		{ &st::menuIconDeleteAttention });
	remove->setClickedCallback([=] {
		ShowBox(Ui::MakeConfirmBox({
			.text = Tr("DeleteConfirm", name),
			.confirmed = [=](Fn<void()> close) {
				Engine::Instance().remove(pluginId);
				close();
				box->closeBox();
			},
			.confirmText = Tr("Delete"),
			.confirmStyle = &st::attentionBoxButton,
		}));
	});
	Ui::AddSkip(content);
}

} // namespace

Window::SessionController *ActiveController() {
	if (!Core::IsAppLaunched()) {
		return nullptr;
	}
	if (const auto window = Core::App().activePrimaryWindow()) {
		return window->sessionController();
	}
	return nullptr;
}

const style::icon *IconFor(const QString &androidName) {
	struct Entry {
		const char *prefix;
		const style::icon *icon;
	};
	static const auto table = std::vector<Entry>{
		{ "settings", &st::menuIconSettings },
		{ "copy", &st::menuIconCopy },
		{ "delete", &st::menuIconDelete },
		{ "clear", &st::menuIconClear },
		{ "edit", &st::menuIconEdit },
		{ "info", &st::menuIconInfo },
		{ "link", &st::menuIconLink },
		{ "forward", &st::menuIconForward },
		{ "share", &st::menuIconShare },
		{ "reply", &st::menuIconReply },
		{ "unpin", &st::menuIconUnpin },
		{ "pin", &st::menuIconPin },
		{ "translate", &st::menuIconTranslate },
		{ "download", &st::menuIconDownload },
		{ "search", &st::menuIconSearch },
		{ "unmute", &st::menuIconUnmute },
		{ "mute", &st::menuIconMute },
		{ "photo", &st::menuIconPhoto },
		{ "media", &st::menuIconPhoto },
		{ "gallery", &st::menuIconPhoto },
		{ "folder", &st::menuIconShowInFolder },
		{ "file", &st::menuIconFile },
		{ "view_file", &st::menuIconFile },
		{ "user", &st::menuIconProfile },
		{ "contact", &st::menuIconProfile },
		{ "profile", &st::menuIconProfile },
		{ "group", &st::menuIconGroups },
		{ "channel", &st::menuIconChannel },
		{ "bot", &st::menuIconBot },
		{ "stats", &st::menuIconStats },
		{ "archive", &st::menuIconArchive },
		{ "lock", &st::menuIconLock },
		{ "block", &st::menuIconBlock },
		{ "report", &st::menuIconReport },
		{ "timer", &st::menuIconTimer },
		{ "schedule", &st::menuIconSchedule },
		{ "calendar", &st::menuIconSchedule },
		{ "send", &st::menuIconSend },
		{ "emoji", &st::menuIconEmoji },
		{ "sticker", &st::menuIconStickers },
		{ "gif", &st::menuIconGif },
		{ "retry", &st::menuIconRestore },
		{ "reset", &st::menuIconRestore },
		{ "restore", &st::menuIconRestore },
		{ "refresh", &st::menuIconRestore },
		{ "fave", &st::menuIconFave },
		{ "star", &st::menuIconFave },
		{ "premium", &st::menuIconPremium },
		{ "palette", &st::menuIconPalette },
		{ "theme", &st::menuIconPalette },
		{ "notification", &st::menuIconNotifications },
		{ "qr", &st::menuIconQrCode },
		{ "saved", &st::menuIconSavedMessages },
		{ "save", &st::menuIconDownload },
		{ "markread", &st::menuIconMarkRead },
		{ "read", &st::menuIconMarkRead },
		{ "unread", &st::menuIconMarkUnread },
		{ "select", &st::menuIconSelect },
		{ "spoiler", &st::menuIconSpoiler },
		{ "font", &st::menuIconFont },
		{ "text", &st::menuIconFont },
		{ "phone", &st::menuIconPhone },
		{ "call", &st::menuIconPhone },
		{ "ghost", &st::menuIconStealth },
		{ "stealth", &st::menuIconStealth },
		{ "hide", &st::menuIconStealth },
		{ "language", &st::menuIconTranslate },
		{ "chat", &st::menuIconChatBubble },
		{ "message", &st::menuIconChatBubble },
		{ "add", &st::menuIconAdd },
		{ "plus", &st::menuIconAdd },
		{ "location", &st::menuIconAddress },
		{ "poll", &st::menuIconCreatePoll },
		{ "devices", &st::menuIconDevices },
		{ "network", &st::menuIconNetwork },
		{ "proxy", &st::menuIconNetwork },
		{ "storage", &st::menuIconStorage },
		{ "code", &st::menuIconExperimental },
		{ "plugin", &st::menuIconExperimental },
	};
	auto name = androidName.toLower();
	if (name.startsWith(u"msg_"_q)) {
		name = name.mid(4);
	}
	if (!name.isEmpty()) {
		for (const auto &entry : table) {
			if (name.startsWith(QLatin1String(entry.prefix))) {
				return entry.icon;
			}
		}
	}
	return &st::menuIconExperimental;
}

void ShowAlert(const AlertEvent &event) {
	auto &alerts = Alerts();
	const auto i = alerts.boxes.find(event.token);
	const auto existing = (i != end(alerts.boxes)) ? i->second.get() : nullptr;
	if (event.op == u"dismiss"_q || event.op == u"update"_q) {
		if (existing) {
			if (event.op == u"update"_q) {
				alerts.silentClose.emplace(event.token);
			}
			existing->closeBox();
		}
		if (event.op == u"dismiss"_q) {
			return;
		}
	} else if (existing) {
		alerts.silentClose.emplace(event.token);
		existing->closeBox();
	}
	auto box = Box(FillAlertBox, event);
	const auto raw = box.data();
	if (ShowBox(std::move(box))) {
		alerts.boxes[event.token] = base::weak_qptr<Ui::GenericBox>(raw);
	}
}

void ShowPluginBox(const QString &pluginId) {
	ShowBox(Box(FillPluginBox, pluginId));
}

void ShowPluginSettings(
		const QString &pluginId,
		const QString &subPageToken,
		const QString &title) {
	const auto info = Engine::Instance().plugin(pluginId);
	const auto shownTitle = !title.isEmpty()
		? title
		: info
		? info->name
		: pluginId;
	Engine::Instance().requestSettings(pluginId, subPageToken, [=](
			QJsonArray items,
			QJsonObject values) {
		ShowBox(Box(
			FillSettingsBox,
			pluginId,
			subPageToken,
			shownTitle,
			items,
			values));
	});
}

void ShowPluginLog(const QString &pluginId) {
	const auto info = Engine::Instance().plugin(pluginId);
	const auto text = (info && !info->log.isEmpty())
		? info->log.join('\n')
		: Tr("EmptyLog");
	ShowBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(Tr("Log")));
		box->setWidth(st::boxWideWidth);
		const auto label = box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			rpl::single(text),
			st::boxLabel));
		label->setSelectable(true);
		box->addButton(rpl::single(Tr("Close")), [=] { box->closeBox(); });
		box->addLeftButton(rpl::single(Tr("Copy")), [=] {
			QGuiApplication::clipboard()->setText(text);
			Ui::Toast::Show(Tr("Copied"));
		});
	}));
}

void InstallWithConfirmation(const QString &path, const QString &name) {
	ShowBox(Ui::MakeConfirmBox({
		.text = Tr("InstallConfirm", name),
		.confirmed = [=](Fn<void()> close) {
			close();
			InstallNow(path);
		},
		.confirmText = Tr("Install"),
		.title = Tr("Install"),
	}));
}

void InstallNow(const QString &path) {
	Engine::Instance().install(path, [=](QString installed, QString error) {
		if (!error.isEmpty()) {
			Ui::Toast::Show(Tr("Error") + u": "_q + error);
			return;
		}
		Ui::Toast::Show(Tr("Installed", installed));
		OpenPluginsSection();
	});
}

void ChooseAndInstall() {
	auto parent = QPointer<QWidget>();
	if (Core::IsAppLaunched()) {
		if (const auto window = Core::App().activePrimaryWindow()) {
			parent = window->widget().get();
		}
	}
	FileDialog::GetOpenPath(
		parent,
		Tr("InstallFile"),
		Tr("PluginFiles") + u";;All files (*.*)"_q,
		[=](FileDialog::OpenResult &&result) {
			if (result.paths.isEmpty()) {
				return;
			}
			const auto path = result.paths.front();
			auto name = QFileInfo(path).completeBaseName();
			auto file = QFile(path);
			if (file.open(QIODevice::ReadOnly)) {
				const auto meta = Ui::ParsePluginMetadata(file.readAll());
				if (!meta.name.isEmpty()) {
					name = meta.name;
				}
			}
			InstallWithConfirmation(path, name);
		});
}

void OpenPluginsSection() {
	if (const auto controller = ActiveController()) {
		controller->showSettings(Settings::AyuPluginsSectionId());
	}
}

void AddMessageMenuItems(not_null<Ui::PopupMenu*> menu, HistoryItem *item) {
	if (!item) {
		return;
	}
	const auto items = Engine::Instance().menuItems(
		u"MESSAGE_CONTEXT_MENU"_q);
	if (items.empty()) {
		return;
	}
	const auto context = MessageContext(item);
	for (const auto &entry : items) {
		menu->addAction(entry.text, [=] {
			Engine::Instance().menuClick(entry, context);
		}, IconFor(entry.icon));
	}
}

void AddPeerMenuItems(
		PeerData *peer,
		const Ui::Menu::MenuCallback &addAction) {
	if (!peer) {
		return;
	}
	auto items = Engine::Instance().menuItems(u"CHAT_ACTION_MENU"_q);
	for (auto &item : Engine::Instance().menuItems(u"PROFILE_ACTION_MENU"_q)) {
		items.push_back(std::move(item));
	}
	if (items.empty()) {
		return;
	}
	const auto context = PeerContext(peer);
	for (const auto &entry : items) {
		addAction(entry.text, [=] {
			Engine::Instance().menuClick(entry, context);
		}, IconFor(entry.icon));
	}
}

void AddMainMenuItems(not_null<Ui::VerticalLayout*> menu) {
	auto items = Engine::Instance().menuItems(u"DRAWER_MENU"_q);
	for (auto &item : Engine::Instance().menuItems(u"MAIN_MENU"_q)) {
		items.push_back(std::move(item));
	}
	for (const auto &entry : items) {
		Settings::AddButtonWithIcon(
			menu,
			rpl::single(entry.text),
			st::mainMenuButton,
			{ IconFor(entry.icon) }
		)->setClickedCallback([=] {
			Engine::Instance().menuClick(entry, QJsonObject());
		});
	}
}

} // namespace AyuPlugins

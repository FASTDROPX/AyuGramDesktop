// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
#include "ayu/plugins/plugins_section.h"

#include "ayu/plugins/plugins_engine.h"
#include "ayu/plugins/plugins_lang.h"
#include "ayu/plugins/plugins_ui.h"
#include "settings/settings_common.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

namespace Settings {
namespace {

using ::AyuPlugins::Engine;
using ::AyuPlugins::Tr;

[[nodiscard]] style::margins LabelPadding() {
	const auto &padding = st::settingsButtonNoIcon.padding;
	return style::margins(padding.left(), 0, padding.right(), padding.bottom());
}

void AddLabel(not_null<Ui::VerticalLayout*> container, rpl::producer<QString> text) {
	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			std::move(text),
			st::boxDividerLabel),
		LabelPadding());
}

void AddEngineBlock(not_null<Ui::VerticalLayout*> container) {
	auto &engine = Engine::Instance();

	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(container, rpl::single(Tr("PluginEngine")));
	AddLabel(
		container,
		rpl::single(rpl::empty) | rpl::then(
			engine.changes()
		) | rpl::map([] {
			return Engine::Instance().status();
		}));

	const auto enabled = AddButtonWithIcon(
		container,
		rpl::single(Tr("EnablePlugins")),
		st::settingsButtonNoIcon);
	enabled->toggleOn(rpl::single(engine.pluginsEnabled()));
	enabled->toggledChanges(
	) | rpl::filter([](bool value) {
		return (value != Engine::Instance().pluginsEnabled());
	}) | rpl::on_next([](bool value) {
		Engine::Instance().setPluginsEnabled(value);
	}, enabled->lifetime());

	const auto safe = AddButtonWithIcon(
		container,
		rpl::single(Tr("SafeMode")),
		st::settingsButtonNoIcon);
	safe->toggleOn(rpl::single(engine.safeMode()));
	safe->toggledChanges(
	) | rpl::filter([](bool value) {
		return (value != Engine::Instance().safeMode());
	}) | rpl::on_next([](bool value) {
		Engine::Instance().setSafeMode(value);
	}, safe->lifetime());
	AddLabel(container, rpl::single(Tr("SafeModeAbout")));

	AddButtonWithIcon(
		container,
		rpl::single(Tr("InstallFile")),
		st::settingsButton,
		{ &st::menuIconAdd }
	)->setClickedCallback([] {
		::AyuPlugins::ChooseAndInstall();
	});
	AddButtonWithIcon(
		container,
		rpl::single(Tr("OpenFolder")),
		st::settingsButton,
		{ &st::menuIconShowInFolder }
	)->setClickedCallback([] {
		QDesktopServices::openUrl(QUrl::fromLocalFile(
			Engine::Instance().pluginsFolder()));
	});
	AddButtonWithIcon(
		container,
		rpl::single(Tr("Restart")),
		st::settingsButton,
		{ &st::menuIconRestore }
	)->setClickedCallback([] {
		Engine::Instance().restart();
	});

	Ui::AddSkip(container);
	Ui::AddDividerText(container, rpl::single(Tr("About")));
}

void FillPluginsList(not_null<Ui::VerticalLayout*> list) {
	list->clear();
	const auto &plugins = Engine::Instance().plugins();
	if (plugins.empty()) {
		AddLabel(list, rpl::single(Tr("NoPlugins")));
	}
	for (const auto &plugin : plugins) {
		const auto id = plugin.id;
		const auto row = list->add(object_ptr<Ui::VerticalLayout>(list));
		auto title = plugin.name;
		if (!plugin.version.isEmpty()) {
			title += u"  •  "_q + plugin.version;
		}
		const auto button = AddButtonWithIcon(
			row,
			rpl::single(title),
			st::settingsButtonNoIcon);
		button->setClickedCallback([=] {
			::AyuPlugins::ShowPluginBox(id);
		});
		const auto [toggle, checkView] = AddSeparatedToggle(
			button,
			st::settingsButtonNoIcon,
			plugin.enabled);
		toggle->clicks() | rpl::on_next([=] {
			const auto enable = !checkView->checked();
			checkView->setChecked(enable, anim::type::normal);
			Engine::Instance().setEnabled(id, enable);
		}, toggle->lifetime());

		auto about = plugin.error.isEmpty()
			? plugin.description.section('\n', 0, 0)
			: (Tr("Error") + u": "_q + plugin.error.section('\n', 0, 0));
		if (about.size() > 160) {
			about = about.left(157) + u"..."_q;
		}
		if (!about.isEmpty()) {
			AddLabel(row, rpl::single(about));
		}
	}
	list->resizeToWidth(list->width());
}

} // namespace

AyuPluginsSection::AyuPluginsSection(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	setupContent();
}

rpl::producer<QString> AyuPluginsSection::title() {
	return rpl::single(Tr("Plugins"));
}

void AyuPluginsSection::setupContent() {
	const auto content = Ui::CreateChild<Ui::VerticalLayout>(this);

	AddEngineBlock(content);

	Ui::AddSkip(content);
	Ui::AddSubsectionTitle(content, rpl::single(Tr("InstalledList")));
	const auto list = content->add(object_ptr<Ui::VerticalLayout>(content));
	FillPluginsList(list);
	Ui::AddSkip(content);

	// Rebuild later: a toggle inside the list may be the one firing.
	const auto scheduled = list->lifetime().make_state<bool>(false);
	Engine::Instance().changes(
	) | rpl::on_next([=] {
		if (*scheduled) {
			return;
		}
		*scheduled = true;
		InvokeQueued(list, [=] {
			*scheduled = false;
			FillPluginsList(list);
		});
	}, list->lifetime());

	Ui::ResizeFitChild(this, content);
}

Type AyuPluginsSectionId() {
	return AyuPluginsSection::Id();
}

} // namespace Settings

// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
// Settings > AyuGram > Plugins.
#pragma once

#include "settings/settings_common.h"
#include "settings/settings_common_session.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Settings {

class AyuPluginsSection : public Section<AyuPluginsSection> {
public:
	AyuPluginsSection(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;

private:
	void setupContent();

};

[[nodiscard]] Type AyuPluginsSectionId();

} // namespace Settings

// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
#include "ayu/plugins/plugins_lang.h"

#include "lang/lang_instance.h"

namespace AyuPlugins {
namespace {

struct Phrase {
	const char *key;
	const char *en;
	const char *ru;
};

const Phrase kPhrases[] = {
	{ "Plugins", "Plugins", "Плагины" },
	{ "PluginEngine", "Plugin engine", "Движок плагинов" },
	{ "Starting", "Starting the plugin engine…", "Запуск движка плагинов…" },
	{ "Ready", "Running · Python %1", "Работает · Python %1" },
	{ "NoPython", "Python 3.10+ was not found. Put it next to AyuGram (python\\python.exe) or install it from python.org.", "Python 3.10+ не найден. Положите его рядом с AyuGram (python\\python.exe) или установите с python.org." },
	{ "HostFailed", "The plugin engine could not start: %1", "Не удалось запустить движок плагинов: %1" },
	{ "HostStopped", "The plugin engine stopped. Restarting…", "Движок плагинов остановился. Перезапуск…" },
	{ "Disabled", "Plugins are turned off", "Плагины выключены" },
	{ "SafeMode", "Safe mode", "Безопасный режим" },
	{ "SafeModeAbout", "Start without loading plugins", "Запуск без загрузки плагинов" },
	{ "EnablePlugins", "Enable plugins", "Включить плагины" },
	{ "Install", "Install plugin", "Установить плагин" },
	{ "InstallFile", "Install from file…", "Установить из файла…" },
	{ "InstallConfirm", "Install the plugin «%1»? Plugins run Python code on this computer — install only plugins you trust.", "Установить плагин «%1»? Плагины выполняют Python-код на этом компьютере — ставьте только те, которым доверяете." },
	{ "Installed", "Plugin «%1» installed", "Плагин «%1» установлен" },
	{ "OpenFolder", "Open plugins folder", "Открыть папку плагинов" },
	{ "Restart", "Restart plugin engine", "Перезапустить движок плагинов" },
	{ "InstalledList", "Installed plugins", "Установленные плагины" },
	{ "NoPlugins", "No plugins yet. Open a .plugin file in a chat and press «Install plugin».", "Плагинов пока нет. Откройте .plugin-файл в чате и нажмите «Установить плагин»." },
	{ "About", "Plugins from exteraGram (the same Python API as on Android) run in a separate process. Hooks see Telegram requests and updates as TLRPC objects.", "Плагины exteraGram (тот же Python API, что на Android) работают в отдельном процессе. Хуки видят запросы и апдейты Telegram как объекты TLRPC." },
	{ "On", "On", "Вкл." },
	{ "Off", "Off", "Выкл." },
	{ "Error", "Error", "Ошибка" },
	{ "Enable", "Enable", "Включить" },
	{ "Disable", "Disable", "Выключить" },
	{ "Settings", "Settings", "Настройки" },
	{ "Log", "Log", "Журнал" },
	{ "EmptyLog", "The log is empty", "Журнал пуст" },
	{ "Reload", "Reload", "Перезагрузить" },
	{ "Delete", "Delete", "Удалить" },
	{ "DeleteConfirm", "Delete the plugin «%1» and its settings?", "Удалить плагин «%1» вместе с настройками?" },
	{ "ShowFile", "Show file", "Показать файл" },
	{ "Version", "Version %1", "Версия %1" },
	{ "Author", "Author: %1", "Автор: %1" },
	{ "Copied", "Copied to clipboard", "Скопировано в буфер обмена" },
	{ "Copy", "Copy", "Копировать" },
	{ "AndroidOnly", "This settings row exists only on Android", "Эта строка настроек есть только на Android" },
	{ "Cancelled", "Cancelled by a plugin", "Отменено плагином" },
	{ "Close", "Close", "Закрыть" },
	{ "Save", "Save", "Сохранить" },
	{ "Undo", "Undo", "Отменить" },
	{ "PluginFiles", "exteraGram plugins (*.plugin *.py)", "Плагины exteraGram (*.plugin *.py)" },
};

[[nodiscard]] bool UseRussian() {
	const auto id = Lang::GetInstance().id().toLower();
	return id.startsWith(u"ru"_q) || id.startsWith(u"uk"_q) || id.startsWith(u"be"_q);
}

} // namespace

QString Tr(const char *key) {
	const auto ru = UseRussian();
	for (const auto &phrase : kPhrases) {
		if (!strcmp(phrase.key, key)) {
			return QString::fromUtf8(ru ? phrase.ru : phrase.en);
		}
	}
	return QString::fromUtf8(key);
}

QString Tr(const char *key, const QString &arg) {
	return Tr(key).arg(arg);
}

} // namespace AyuPlugins

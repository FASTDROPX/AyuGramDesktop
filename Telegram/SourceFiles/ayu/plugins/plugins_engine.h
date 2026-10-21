// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
//
// Plugins are exteraGram .plugin files (Python, the same SDK as on Android).
// They run in a separate Python process (plugins-runtime/host/plugin_host.py)
// that talks to the client with one JSON object per line over stdin/stdout.
// Hooks get raw TL bytes, which the Python side turns into TLRPC objects built
// from the client's own api.tl, so field names match Telegram for Android.
#pragma once

#include "mtproto/core_types.h"

#include <QtCore/QObject>
#include <QtCore/QProcess>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>

namespace Main {
class Session;
} // namespace Main

namespace AyuPlugins {

struct PluginInfo {
	QString id;
	QString name;
	QString description;
	QString author;
	QString version;
	QString path;
	QString error;
	QStringList log;
	bool enabled = false;
	bool loaded = false;
	bool hasSettings = false;
};

struct MenuItem {
	QString pluginId;
	QString id;
	QString menuType;
	QString text;
	QString subtext;
	QString icon;
	QString token;
	int priority = 0;
};

struct HookOutcome {
	bool cancel = false;
	bool modified = false;
	QByteArray tl;
	QJsonObject obj;
};

struct AlertEvent {
	QString pluginId;
	QString op;
	QString token;
	QJsonObject state;
};

class Engine final : public QObject {
public:
	[[nodiscard]] static Engine &Instance();

	void start();
	void restart();
	void shutdown();

	[[nodiscard]] bool available() const {
		return _available;
	}
	[[nodiscard]] QString status() const {
		return _status;
	}
	[[nodiscard]] const std::vector<PluginInfo> &plugins() const {
		return _plugins;
	}
	[[nodiscard]] const PluginInfo *plugin(const QString &id) const;
	[[nodiscard]] std::vector<MenuItem> menuItems(const QString &type) const;
	[[nodiscard]] rpl::producer<> changes() const {
		return _changes.events();
	}
	[[nodiscard]] rpl::producer<QString> settingsReloads() const {
		return _settingsReloads.events();
	}

	[[nodiscard]] QString pluginsFolder() const;
	[[nodiscard]] bool pluginsEnabled() const;
	void setPluginsEnabled(bool enabled);
	[[nodiscard]] bool safeMode() const;
	void setSafeMode(bool enabled);

	void setEnabled(const QString &id, bool enabled);
	void install(const QString &path, Fn<void(QString name, QString error)> done = nullptr);
	void remove(const QString &id);
	void reload(const QString &id);

	// Hooks. Called from the integration points on the main thread.
	[[nodiscard]] bool hasHook(mtpTypeId type) const;
	[[nodiscard]] bool hasSendMessageHook() const;
	[[nodiscard]] bool bypassed() const;
	HookOutcome runHook(
		const QString &kind,
		mtpTypeId type,
		const QByteArray &tl,
		const QJsonObject &extra = QJsonObject());
	HookOutcome runSendMessageHook(const QJsonObject &params);

	// While alive, the client's own actions skip plugin hooks
	// (messages sent by plugins, updates already filtered once).
	class Bypass final {
	public:
		Bypass();
		~Bypass();
	};

	// Settings pages and callbacks of plugins.
	void requestSettings(
		const QString &pluginId,
		const QString &subPageToken,
		Fn<void(QJsonArray items, QJsonObject values)> done);
	[[nodiscard]] QJsonValue settingValue(
		const QString &pluginId,
		const QString &key) const;
	void setSettingFromUi(
		const QString &pluginId,
		const QString &key,
		const QJsonValue &value,
		const QString &onChangeToken);
	void invokeCallback(
		const QString &pluginId,
		const QString &token,
		QJsonArray args,
		bool wrap = false);
	void menuClick(const MenuItem &item, QJsonObject context);

private:
	Engine();

	struct HookReg {
		QString pluginId;
		QString name;
		int priority = 0;
	};
	struct Python {
		QString exe;
		QString version;
	};

	[[nodiscard]] static std::optional<Python> FindPython(QString configured);
	void startHost(std::optional<Python> python);
	void onHostFinished();
	void readOutput();
	void readErrors();
	void handleLine(const QByteArray &line);
	void scheduleInbox();
	void processInbox();
	void handleMessage(const QJsonObject &msg);
	void handleRequest(const QJsonObject &msg);
	void reply(int id, const QJsonValue &value);
	void replyError(int id, int code, const QString &text);

	void write(const QJsonObject &msg);
	void call(
		const QString &op,
		QJsonObject fields,
		Fn<void(QJsonObject reply)> done = nullptr);
	std::optional<QJsonObject> callSync(
		const QString &op,
		QJsonObject fields,
		int timeoutMs);

	void rescan();
	void scanNext(QStringList files, int index, std::shared_ptr<QSet<QString>> seen);
	void load(const QString &id);
	void unload(const QString &id);
	void dropPluginState(const QString &id);
	void addLog(const QString &pluginId, const QString &line);
	[[nodiscard]] PluginInfo *pluginMutable(const QString &id);
	[[nodiscard]] QJsonObject settingsOf(const QString &id) const;
	void applyMeta(PluginInfo &info, const QJsonObject &meta);

	void loadState();
	void saveState();
	[[nodiscard]] QString stateFile() const;
	[[nodiscard]] QString dataFolder() const;
	[[nodiscard]] static QString RuntimeFolder();

	std::unique_ptr<QProcess> _process;
	QByteArray _outBuffer;
	QByteArray _errBuffer;
	int _nextId = 0;
	QSet<int> _syncWaiting;
	QHash<int, QJsonObject> _syncReplies;
	QHash<int, Fn<void(QJsonObject)>> _callbacks;
	std::vector<std::pair<Fn<void(QJsonObject)>, QJsonObject>> _readyCallbacks;
	std::vector<QJsonObject> _inbox;
	bool _inboxScheduled = false;
	bool _stopping = false;
	int _restarts = 0;
	bool _started = false;

	bool _available = false;
	QString _status;
	QString _pythonVersion;
	std::vector<PluginInfo> _plugins;
	std::vector<MenuItem> _menuItems;
	std::map<mtpTypeId, std::vector<HookReg>> _hooks;
	std::vector<HookReg> _sendMessageHooks;
	QJsonObject _state;

	rpl::event_stream<> _changes;
	rpl::event_stream<QString> _settingsReloads;

};

[[nodiscard]] Main::Session *ActiveSession();

} // namespace AyuPlugins

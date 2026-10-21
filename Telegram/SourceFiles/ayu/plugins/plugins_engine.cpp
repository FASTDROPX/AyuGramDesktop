// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
#include "ayu/plugins/plugins_engine.h"

#include "ayu/plugins/plugins_hooks.h"
#include "ayu/plugins/plugins_lang.h"
#include "ayu/plugins/plugins_ui.h"
#include "ayu/ui/toasts.h"
#include "api/api_common.h"
#include "api/api_text_entities.h"
#include "apiwrap.h"
#include "core/application.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/details/mtproto_serialized_request.h"
#include "mtproto/mtp_instance.h"
#include "settings.h"
#include "ui/toast/toast.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace AyuPlugins {
namespace {

constexpr auto kHookTimeout = 2500;
constexpr auto kMaxLogLines = 300;

int BypassDepth = 0;
int PluginSending = 0;

[[nodiscard]] QString B64(const QByteArray &bytes) {
	return QString::fromLatin1(bytes.toBase64());
}

[[nodiscard]] QByteArray FromB64(const QJsonValue &value) {
	return QByteArray::fromBase64(value.toString().toLatin1());
}

[[nodiscard]] QByteArray BufferBytes(const mtpBuffer &buffer) {
	return QByteArray(
		reinterpret_cast<const char*>(buffer.constData()),
		buffer.size() * int(sizeof(mtpPrime)));
}

[[nodiscard]] TextWithTags ParseText(
		const QString &text,
		const QString &parseMode) {
	if (parseMode.isEmpty()) {
		return TextWithTags{ text, {} };
	}
	auto marked = TextWithEntities{ text };
	TextUtilities::ParseEntities(
		marked,
		TextParseLinks
			| TextParseMentions
			| TextParseHashtags
			| TextParseMarkdown);
	return TextWithTags{
		marked.text,
		TextUtilities::ConvertEntitiesToTextTags(marked.entities),
	};
}

void ApplyUpdatesFromResponse(
		not_null<Main::Session*> session,
		const mtpBuffer &reply) {
	if (reply.isEmpty()) {
		return;
	}
	switch (mtpTypeId(reply[0])) {
	case mtpc_updates:
	case mtpc_updatesCombined:
	case mtpc_updateShort:
	case mtpc_updateShortMessage:
	case mtpc_updateShortChatMessage:
	case mtpc_updatesTooLong: {
		auto updates = MTPUpdates();
		auto from = reply.constData();
		if (updates.read(from, from + reply.size())) {
			session->api().applyUpdates(updates);
		}
	} break;
	}
}

} // namespace

Main::Session *ActiveSession() {
	if (!Core::IsAppLaunched()) {
		return nullptr;
	}
	auto &account = Core::App().domain().active();
	return account.sessionExists() ? &account.session() : nullptr;
}

Engine::Bypass::Bypass() {
	++BypassDepth;
}

Engine::Bypass::~Bypass() {
	--BypassDepth;
}

Engine &Engine::Instance() {
	static auto instance = new Engine();
	return *instance;
}

Engine::Engine() {
	_status = Tr("Starting");
}

QString Engine::RuntimeFolder() {
	return QCoreApplication::applicationDirPath() + u"/plugins-runtime"_q;
}

QString Engine::dataFolder() const {
	return QDir(cWorkingDir()).filePath(u"plugins_data"_q);
}

QString Engine::pluginsFolder() const {
	const auto path = QDir(cWorkingDir()).filePath(u"plugins"_q);
	QDir().mkpath(path);
	return path;
}

QString Engine::stateFile() const {
	return QDir(cWorkingDir()).filePath(u"tdata/ayu_plugins.json"_q);
}

void Engine::loadState() {
	auto file = QFile(stateFile());
	if (file.open(QIODevice::ReadOnly)) {
		_state = QJsonDocument::fromJson(file.readAll()).object();
	}
	if (!_state.value(u"enabled"_q).isObject()) {
		_state.insert(u"enabled"_q, QJsonObject());
	}
	if (!_state.value(u"settings"_q).isObject()) {
		_state.insert(u"settings"_q, QJsonObject());
	}
}

void Engine::saveState() {
	auto file = QFile(stateFile());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(_state).toJson(QJsonDocument::Indented));
	}
}

bool Engine::pluginsEnabled() const {
	return _state.value(u"plugins_enabled"_q).toBool(true);
}

void Engine::setPluginsEnabled(bool enabled) {
	_state.insert(u"plugins_enabled"_q, enabled);
	saveState();
	restart();
}

bool Engine::safeMode() const {
	return _state.value(u"safe_mode"_q).toBool(false);
}

void Engine::setSafeMode(bool enabled) {
	_state.insert(u"safe_mode"_q, enabled);
	saveState();
	restart();
}

// ---- process ---------------------------------------------------------------------------

std::optional<Engine::Python> Engine::FindPython(QString configured) {
	const auto code = u"import sys;print(sys.executable);"
		"print('%d.%d.%d' % sys.version_info[:3]);"
		"print(sys.version_info >= (3, 10))"_q;
	auto candidates = std::vector<std::pair<QString, QStringList>>();
	if (!configured.isEmpty()) {
		candidates.push_back({ configured, {} });
	}
	candidates.push_back({ u"py"_q, { u"-3"_q } });
	candidates.push_back({ u"python"_q, {} });
	const auto bundled = QCoreApplication::applicationDirPath()
		+ u"/python/python.exe"_q;
	if (QFileInfo::exists(bundled)) {
		candidates.push_back({ bundled, {} });
	}
	for (const auto &[exe, prefix] : candidates) {
		auto process = QProcess();
		process.start(exe, prefix + QStringList{ u"-c"_q, code });
		if (!process.waitForFinished(8000)) {
			process.kill();
			process.waitForFinished(1000);
			continue;
		}
		if (process.exitStatus() != QProcess::NormalExit
			|| process.exitCode() != 0) {
			continue;
		}
		const auto lines = QString::fromUtf8(
			process.readAllStandardOutput()).split(
				'\n',
				Qt::SkipEmptyParts);
		if (lines.size() < 3 || lines[2].trimmed() != u"True"_q) {
			continue;
		}
		const auto path = lines[0].trimmed();
		if (path.contains(u"WindowsApps"_q, Qt::CaseInsensitive)) {
			continue; // The Microsoft Store stub.
		}
		return Python{ path, lines[1].trimmed() };
	}
	return std::nullopt;
}

void Engine::start() {
	if (_started) {
		return;
	}
	_started = true;
	loadState();
	_stopping = false;
	static auto connected = false;
	if (!connected) {
		connected = true;
		InstallRequestFilter();
		QObject::connect(
			QCoreApplication::instance(),
			&QCoreApplication::aboutToQuit,
			this,
			[=] { shutdown(); });
	}
	if (!pluginsEnabled()) {
		_status = Tr("Disabled");
		rescan();
		return;
	}
	_status = Tr("Starting");
	const auto configured = _state.value(u"python_path"_q).toString();
	crl::async([=] {
		auto python = FindPython(configured);
		crl::on_main([=] {
			startHost(python);
		});
	});
}

void Engine::startHost(std::optional<Python> python) {
	if (!python) {
		_status = Tr("NoPython");
		_available = false;
		rescan();
		_changes.fire({});
		return;
	}
	const auto script = RuntimeFolder() + u"/host/plugin_host.py"_q;
	if (!QFileInfo::exists(script)) {
		_status = Tr("HostFailed", script);
		_changes.fire({});
		return;
	}
	_pythonVersion = python->version;
	QDir().mkpath(dataFolder());
	_process = std::make_unique<QProcess>();
	_process->setWorkingDirectory(dataFolder());
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert(u"PYTHONIOENCODING"_q, u"utf-8"_q);
	environment.insert(u"PYTHONDONTWRITEBYTECODE"_q, u"1"_q);
	_process->setProcessEnvironment(environment);
	QObject::connect(
		_process.get(),
		&QProcess::readyReadStandardOutput,
		this,
		[=] { readOutput(); });
	QObject::connect(
		_process.get(),
		&QProcess::readyReadStandardError,
		this,
		[=] { readErrors(); });
	QObject::connect(
		_process.get(),
		&QProcess::finished,
		this,
		[=] { onHostFinished(); });
	_process->start(
		python->exe,
		{ u"-u"_q, u"-X"_q, u"utf8"_q, script });
	if (!_process->waitForStarted(10000)) {
		_status = Tr("HostFailed", _process->errorString());
		_process = nullptr;
		_changes.fire({});
		return;
	}
	auto dirs = QJsonObject{
		{ u"plugins"_q, pluginsFolder() },
		{ u"files"_q, dataFolder() },
		{ u"cache"_q, dataFolder() + u"/cache"_q },
		{ u"images"_q, dataFolder() + u"/images"_q },
		{ u"videos"_q, dataFolder() + u"/videos"_q },
		{ u"audios"_q, dataFolder() + u"/audios"_q },
		{ u"documents"_q, dataFolder() + u"/documents"_q },
	};
	call(u"init"_q, QJsonObject{
		{ u"dirs"_q, dirs },
		{ u"packages_dir"_q, dataFolder() + u"/python-packages"_q },
		{ u"schema"_q, RuntimeFolder() + u"/api.tl"_q },
	}, [=](QJsonObject reply) {
		if (!reply.value(u"ok"_q).toBool()) {
			_status = Tr("HostFailed", reply.value(u"error"_q).toString());
			_changes.fire({});
			return;
		}
		_available = true;
		_status = Tr("Ready", _pythonVersion);
		LOG(("Plugins: engine ready, Python %1, TL layer %2").arg(
			_pythonVersion,
			QString::number(reply.value(u"value"_q).toObject().value(
				u"layer"_q).toInt())));
		rescan();
		_changes.fire({});
	});
}

void Engine::onHostFinished() {
	_available = false;
	for (auto &plugin : _plugins) {
		plugin.loaded = false;
	}
	_hooks.clear();
	_sendMessageHooks.clear();
	_menuItems.clear();
	_callbacks.clear();
	if (_process) {
		const auto raw = _process.release();
		raw->deleteLater();
	}
	_changes.fire({});
	if (_stopping) {
		return;
	}
	_status = Tr("HostStopped");
	if (_restarts++ < 2) {
		QTimer::singleShot(1500, this, [=] {
			_started = false;
			start();
		});
	}
}

void Engine::restart() {
	_stopping = true;
	shutdown();
	_stopping = false;
	_restarts = 0;
	_started = false;
	_available = false;
	start();
	_changes.fire({});
}

void Engine::shutdown() {
	_stopping = true;
	if (!_process) {
		return;
	}
	auto process = std::move(_process);
	QObject::disconnect(process.get(), nullptr, this, nullptr);
	if (process->state() == QProcess::Running) {
		process->write("{\"t\":\"exit\"}\n");
		if (!process->waitForFinished(1500)) {
			process->kill();
			process->waitForFinished(1000);
		}
	}
	_hooks.clear();
	_sendMessageHooks.clear();
	_menuItems.clear();
	for (auto &plugin : _plugins) {
		plugin.loaded = false;
	}
}

// ---- protocol --------------------------------------------------------------------------

void Engine::write(const QJsonObject &msg) {
	if (!_process || _process->state() != QProcess::Running) {
		return;
	}
	_process->write(QJsonDocument(msg).toJson(QJsonDocument::Compact) + '\n');
}

void Engine::call(
		const QString &op,
		QJsonObject fields,
		Fn<void(QJsonObject)> done) {
	if (!_process || _process->state() != QProcess::Running) {
		if (done) {
			done(QJsonObject{
				{ u"ok"_q, false },
				{ u"error"_q, _status },
			});
		}
		return;
	}
	const auto id = ++_nextId;
	if (done) {
		_callbacks.insert(id, std::move(done));
	}
	fields.insert(u"t"_q, u"call"_q);
	fields.insert(u"id"_q, id);
	fields.insert(u"op"_q, op);
	write(fields);
}

std::optional<QJsonObject> Engine::callSync(
		const QString &op,
		QJsonObject fields,
		int timeoutMs) {
	if (!_process || _process->state() != QProcess::Running) {
		return std::nullopt;
	}
	const auto id = ++_nextId;
	fields.insert(u"t"_q, u"call"_q);
	fields.insert(u"id"_q, id);
	fields.insert(u"op"_q, op);
	_syncWaiting.insert(id);
	write(fields);
	auto timer = QElapsedTimer();
	timer.start();
	while (!_syncReplies.contains(id)) {
		const auto left = timeoutMs - int(timer.elapsed());
		if (left <= 0
			|| !_process
			|| _process->state() != QProcess::Running) {
			break;
		}
		_process->waitForReadyRead(left);
		readOutput();
	}
	_syncWaiting.remove(id);
	const auto i = _syncReplies.find(id);
	if (i == _syncReplies.end()) {
		LOG(("Plugins: '%1' timed out").arg(op));
		return std::nullopt;
	}
	auto result = *i;
	_syncReplies.erase(i);
	return result;
}

void Engine::readOutput() {
	if (!_process) {
		return;
	}
	_outBuffer += _process->readAllStandardOutput();
	while (true) {
		const auto end = _outBuffer.indexOf('\n');
		if (end < 0) {
			break;
		}
		const auto line = _outBuffer.left(end).trimmed();
		_outBuffer.remove(0, end + 1);
		if (!line.isEmpty()) {
			handleLine(line);
		}
	}
}

void Engine::readErrors() {
	if (!_process) {
		return;
	}
	_errBuffer += _process->readAllStandardError();
	while (true) {
		const auto end = _errBuffer.indexOf('\n');
		if (end < 0) {
			break;
		}
		LOG(("Plugins host: %1").arg(
			QString::fromUtf8(_errBuffer.left(end).trimmed())));
		_errBuffer.remove(0, end + 1);
	}
}

void Engine::handleLine(const QByteArray &line) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(line, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("Plugins host: %1").arg(QString::fromUtf8(line)));
		return;
	}
	const auto msg = document.object();
	if (msg.value(u"t"_q).toString() == u"ret"_q) {
		const auto id = msg.value(u"id"_q).toInt();
		if (_syncWaiting.contains(id)) {
			_syncReplies.insert(id, msg);
			return;
		}
		if (auto done = _callbacks.take(id)) {
			_readyCallbacks.emplace_back(std::move(done), msg);
			scheduleInbox();
		}
		return;
	}
	// Everything else is handled later: we may be inside a hook right now.
	_inbox.push_back(msg);
	scheduleInbox();
}

void Engine::scheduleInbox() {
	if (_inboxScheduled) {
		return;
	}
	_inboxScheduled = true;
	QMetaObject::invokeMethod(this, [=] {
		_inboxScheduled = false;
		processInbox();
	}, Qt::QueuedConnection);
}

void Engine::processInbox() {
	auto callbacks = base::take(_readyCallbacks);
	auto inbox = base::take(_inbox);
	for (auto &[done, reply] : callbacks) {
		done(reply);
	}
	for (const auto &msg : inbox) {
		handleMessage(msg);
	}
}

void Engine::handleMessage(const QJsonObject &msg) {
	const auto type = msg.value(u"t"_q).toString();
	const auto pluginId = msg.value(u"plugin"_q).toString();
	if (type == u"req"_q) {
		handleRequest(msg);
	} else if (type == u"log"_q) {
		const auto text = msg.value(u"msg"_q).toString();
		LOG(("Plugin %1: %2").arg(
			pluginId.isEmpty() ? u"host"_q : pluginId,
			text));
		if (!pluginId.isEmpty()) {
			addLog(
				pluginId,
				msg.value(u"level"_q).toString().toUpper() + u"  "_q + text);
		}
	} else if (type == u"hook_add"_q) {
		const auto name = msg.value(u"name"_q).toString();
		const auto reg = HookReg{
			pluginId,
			name,
			msg.value(u"priority"_q).toInt(),
		};
		const auto same = [&](const HookReg &h) {
			return h.pluginId == pluginId && h.name == name;
		};
		if (name == u"@send_message"_q) {
			_sendMessageHooks.erase(
				ranges::remove_if(_sendMessageHooks, same),
				end(_sendMessageHooks));
			_sendMessageHooks.push_back(reg);
		} else {
			for (const auto id : msg.value(u"ids"_q).toArray()) {
				auto &list = _hooks[mtpTypeId(id.toVariant().toLongLong())];
				list.erase(ranges::remove_if(list, same), end(list));
				list.push_back(reg);
			}
		}
	} else if (type == u"hook_remove"_q) {
		const auto name = msg.value(u"name"_q).toString();
		const auto same = [&](const HookReg &h) {
			return h.pluginId == pluginId && h.name == name;
		};
		_sendMessageHooks.erase(
			ranges::remove_if(_sendMessageHooks, same),
			end(_sendMessageHooks));
		for (auto i = _hooks.begin(); i != _hooks.end();) {
			i->second.erase(
				ranges::remove_if(i->second, same),
				end(i->second));
			i = i->second.empty() ? _hooks.erase(i) : std::next(i);
		}
	} else if (type == u"menu_add"_q) {
		const auto item = msg.value(u"item"_q).toObject();
		auto menuItem = MenuItem{
			.pluginId = pluginId,
			.id = item.value(u"id"_q).toString(),
			.menuType = item.value(u"menu_type"_q).toString(),
			.text = item.value(u"text"_q).toString(),
			.subtext = item.value(u"subtext"_q).toString(),
			.icon = item.value(u"icon"_q).toString(),
			.token = item.value(u"token"_q).toString(),
			.priority = item.value(u"priority"_q).toInt(),
		};
		_menuItems.erase(ranges::remove_if(_menuItems, [&](const MenuItem &m) {
			return m.id == menuItem.id;
		}), end(_menuItems));
		_menuItems.push_back(std::move(menuItem));
		_changes.fire({});
	} else if (type == u"menu_remove"_q) {
		const auto id = msg.value(u"id"_q).toString();
		_menuItems.erase(ranges::remove_if(_menuItems, [&](const MenuItem &m) {
			return m.id == id;
		}), end(_menuItems));
		_changes.fire({});
	} else if (type == u"setting"_q) {
		auto all = _state.value(u"settings"_q).toObject();
		auto mine = all.value(pluginId).toObject();
		mine.insert(msg.value(u"key"_q).toString(), msg.value(u"value"_q));
		all.insert(pluginId, mine);
		_state.insert(u"settings"_q, all);
		saveState();
		if (msg.value(u"reload"_q).toBool()) {
			_settingsReloads.fire_copy(pluginId);
		}
	} else if (type == u"settings_all"_q) {
		auto all = _state.value(u"settings"_q).toObject();
		all.insert(pluginId, msg.value(u"values"_q).toObject());
		_state.insert(u"settings"_q, all);
		saveState();
		if (msg.value(u"reload"_q).toBool()) {
			_settingsReloads.fire_copy(pluginId);
		}
	} else if (type == u"bulletin"_q) {
		auto text = msg.value(u"text"_q).toString();
		const auto subtitle = msg.value(u"subtitle"_q).toString();
		if (!subtitle.isEmpty()) {
			text += u"\n"_q + subtitle;
		}
		const auto button = msg.value(u"button"_q).toString();
		const auto token = msg.value(u"kind"_q).toString() == u"undo"_q
			? msg.value(u"on_undo"_q).toString()
			: msg.value(u"on_click"_q).toString();
		if (!button.isEmpty() && !token.isEmpty()) {
			Ayu::Ui::ShowToastWithAction(
				::Ui::Toast::Config{ .text = TextWithEntities{ text } },
				button,
				[=] { invokeCallback(pluginId, token, {}); });
		} else {
			::Ui::Toast::Show(text);
		}
	} else if (type == u"alert"_q) {
		ShowAlert(AlertEvent{
			.pluginId = pluginId,
			.op = msg.value(u"op"_q).toString(),
			.token = msg.value(u"id"_q).toString(),
			.state = msg,
		});
	} else if (type == u"clipboard"_q) {
		QGuiApplication::clipboard()->setText(msg.value(u"text"_q).toString());
		::Ui::Toast::Show(Tr("Copied"));
	}
}

// ---- requests from plugins -------------------------------------------------------------

void Engine::reply(int id, const QJsonValue &value) {
	write(QJsonObject{
		{ u"t"_q, u"res"_q },
		{ u"id"_q, id },
		{ u"ok"_q, true },
		{ u"value"_q, value },
	});
}

void Engine::replyError(int id, int code, const QString &text) {
	write(QJsonObject{
		{ u"t"_q, u"res"_q },
		{ u"id"_q, id },
		{ u"ok"_q, false },
		{ u"error"_q, QJsonObject{
			{ u"code"_q, code },
			{ u"text"_q, text },
		} },
	});
}

void Engine::handleRequest(const QJsonObject &msg) {
	const auto id = msg.value(u"id"_q).toInt();
	const auto fn = msg.value(u"fn"_q).toString();
	const auto session = ActiveSession();
	if (!session) {
		replyError(id, 401, u"AUTH_KEY_UNREGISTERED"_q);
		return;
	}
	if (fn == u"tl"_q) {
		const auto bytes = FromB64(msg.value(u"payload"_q));
		if (bytes.isEmpty() || (bytes.size() % int(sizeof(mtpPrime)))) {
			replyError(id, 400, u"BAD_TL_PAYLOAD"_q);
			return;
		}
		auto words = mtpBuffer(bytes.size() / int(sizeof(mtpPrime)));
		memcpy(words.data(), bytes.constData(), bytes.size());
		auto request = MTP::details::SerializedRequest::Prepare(words.size());
		request->append(words);
		const auto requestId = MTP::details::GetNextRequestId();
		const auto weak = base::make_weak(session);
		auto handler = MTP::ResponseHandler{
			.done = [=](const MTP::Response &response) {
				// The client applies updates from responses itself:
				// otherwise a message edited by a plugin stays stale.
				if (const auto strong = weak.get()) {
					ApplyUpdatesFromResponse(strong, response.reply);
				}
				reply(id, B64(BufferBytes(response.reply)));
				return true;
			},
			.fail = [=](const MTP::Error &error, const MTP::Response &) {
				replyError(id, error.code(), error.type());
				return true;
			},
		};
		++PluginSending;
		session->mtp().sendSerialized(
			requestId,
			std::move(request),
			std::move(handler),
			0,
			0,
			0);
		--PluginSending;
	} else if (fn == u"my_id"_q) {
		reply(id, qint64(session->userId().bare));
	} else if (fn == u"input_peer"_q) {
		const auto peerId = PeerFromDialogId(
			session,
			msg.value(u"dialog_id"_q).toVariant().toLongLong());
		if (const auto peer = session->data().peerLoaded(peerId)) {
			reply(id, B64(SerializeTl(peer->input())));
		} else {
			replyError(id, 400, u"PEER_ID_INVALID"_q);
		}
	} else if (fn == u"peer_info"_q) {
		const auto peerId = PeerFromDialogId(
			session,
			msg.value(u"dialog_id"_q).toVariant().toLongLong());
		const auto peer = session->data().peerLoaded(peerId);
		if (!peer) {
			reply(id, QJsonValue());
			return;
		}
		auto info = QJsonObject();
		if (const auto user = peer->asUser()) {
			info.insert(u"_"_q, u"user"_q);
			info.insert(u"id"_q, qint64(peerToUser(peer->id).bare));
			info.insert(u"first_name"_q, user->firstName);
			info.insert(u"last_name"_q, user->lastName);
			info.insert(u"username"_q, user->username());
			info.insert(u"phone"_q, user->phone());
			info.insert(u"bot"_q, user->isBot());
			info.insert(u"self"_q, user->isSelf());
		} else {
			const auto channel = peer->isChannel();
			info.insert(u"_"_q, channel ? u"channel"_q : u"chat"_q);
			info.insert(u"id"_q, channel
				? qint64(peerToChannel(peer->id).bare)
				: qint64(peerToChat(peer->id).bare));
			info.insert(u"title"_q, peer->name());
			info.insert(u"username"_q, peer->username());
			info.insert(u"megagroup"_q, peer->isMegagroup());
			info.insert(u"broadcast"_q, peer->isBroadcast());
		}
		reply(id, info);
	} else if (fn == u"parse_text"_q) {
		const auto parsed = ParseText(
			msg.value(u"text"_q).toString(),
			msg.value(u"parse_mode"_q).toString());
		const auto entities = TextUtilities::ConvertTextTagsToEntities(
			parsed.tags);
		reply(id, QJsonObject{
			{ u"text"_q, parsed.text },
			{ u"entities"_q, B64(SerializeTl(Api::EntitiesToMTP(
				session,
				entities,
				Api::ConvertOption::SkipLocal))) },
		});
	} else if (fn == u"send_message"_q) {
		const auto params = msg.value(u"params"_q).toObject();
		if (!params.value(u"path"_q).toString().isEmpty()) {
			replyError(id, 501, u"FILES_NOT_SUPPORTED_YET"_q);
			return;
		}
		const auto peerId = PeerFromDialogId(
			session,
			params.value(u"peer"_q).toVariant().toLongLong());
		const auto peer = session->data().peerLoaded(peerId);
		if (!peer) {
			replyError(id, 400, u"PEER_ID_INVALID"_q);
			return;
		}
		const auto history = session->data().history(peer);
		auto action = Api::SendAction(history);
		const auto replyTo = params.value(u"replyToMsg"_q).toVariant().toLongLong();
		if (replyTo) {
			action.replyTo.messageId = FullMsgId(peer->id, MsgId(replyTo));
		}
		action.clearDraft = false;
		auto message = Api::MessageToSend(action);
		const auto text = params.value(u"message"_q).toString();
		const auto entities = DeserializeTl<MTPVector<MTPMessageEntity>>(
			FromB64(params.value(u"entities_tl"_q)));
		if (entities) {
			message.textWithTags = TextWithTags{
				text,
				TextUtilities::ConvertEntitiesToTextTags(
					Api::EntitiesFromMTP(session, entities->v)),
			};
		} else {
			message.textWithTags = ParseText(
				text,
				msg.value(u"parse_mode"_q).toString());
		}
		{
			const auto bypass = Bypass();
			session->api().sendMessage(std::move(message));
		}
		reply(id, true);
	} else {
		replyError(id, 400, u"UNKNOWN_REQUEST_"_q + fn);
	}
}

// ---- plugins ---------------------------------------------------------------------------

const PluginInfo *Engine::plugin(const QString &id) const {
	const auto i = ranges::find(_plugins, id, &PluginInfo::id);
	return (i == end(_plugins)) ? nullptr : &*i;
}

PluginInfo *Engine::pluginMutable(const QString &id) {
	const auto i = ranges::find(_plugins, id, &PluginInfo::id);
	return (i == end(_plugins)) ? nullptr : &*i;
}

std::vector<MenuItem> Engine::menuItems(const QString &type) const {
	auto result = std::vector<MenuItem>();
	for (const auto &item : _menuItems) {
		if (item.menuType == type) {
			result.push_back(item);
		}
	}
	ranges::sort(result, ranges::greater(), &MenuItem::priority);
	return result;
}

QJsonObject Engine::settingsOf(const QString &id) const {
	return _state.value(u"settings"_q).toObject().value(id).toObject();
}

void Engine::applyMeta(PluginInfo &info, const QJsonObject &meta) {
	info.name = meta.value(u"name"_q).toString(info.id);
	info.description = meta.value(u"description"_q).toString();
	info.author = meta.value(u"author"_q).toString();
	info.version = meta.value(u"version"_q).toString(u"1.0"_q);
	if (meta.contains(u"has_settings"_q)) {
		info.hasSettings = meta.value(u"has_settings"_q).toBool();
	}
}

void Engine::addLog(const QString &pluginId, const QString &line) {
	if (const auto info = pluginMutable(pluginId)) {
		info->log.push_back(
			QTime::currentTime().toString(u"HH:mm:ss"_q) + u"  "_q + line);
		while (info->log.size() > kMaxLogLines) {
			info->log.pop_front();
		}
	}
}

void Engine::rescan() {
	auto files = QDir(pluginsFolder()).entryList(
		{ u"*.plugin"_q, u"*.py"_q },
		QDir::Files,
		QDir::Name);
	for (auto &file : files) {
		file = QDir(pluginsFolder()).filePath(file);
	}
	if (!_available) {
		// Without Python we still show what is installed.
		_plugins.clear();
		for (const auto &file : files) {
			auto info = PluginInfo();
			info.id = QFileInfo(file).completeBaseName();
			info.name = info.id;
			info.path = file;
			info.enabled = _state.value(u"enabled"_q).toObject().value(
				info.id).toBool();
			info.error = _status;
			_plugins.push_back(std::move(info));
		}
		_changes.fire({});
		return;
	}
	scanNext(files, 0, std::make_shared<QSet<QString>>());
}

void Engine::scanNext(
		QStringList files,
		int index,
		std::shared_ptr<QSet<QString>> seen) {
	if (index >= files.size()) {
		_plugins.erase(ranges::remove_if(_plugins, [&](const PluginInfo &p) {
			return !seen->contains(p.id);
		}), end(_plugins));
		_changes.fire({});
		return;
	}
	const auto path = files[index];
	call(u"scan"_q, QJsonObject{ { u"path"_q, path } }, [=](QJsonObject reply) {
		if (reply.value(u"ok"_q).toBool()) {
			const auto meta = reply.value(u"value"_q).toObject();
			const auto id = meta.value(u"id"_q).toString();
			if (!seen->contains(id)) {
				seen->insert(id);
				auto info = pluginMutable(id);
				if (!info) {
					_plugins.push_back(PluginInfo{ .id = id });
					info = &_plugins.back();
				}
				info->path = path;
				applyMeta(*info, meta);
				info->enabled = _state.value(u"enabled"_q).toObject().value(
					id).toBool();
				if (!info->loaded) {
					info->error = QString();
				}
				if (info->enabled && !info->loaded && !safeMode()) {
					load(id);
				}
			}
		} else {
			const auto id = QFileInfo(path).completeBaseName();
			seen->insert(id);
			auto info = pluginMutable(id);
			if (!info) {
				_plugins.push_back(PluginInfo{ .id = id, .name = id });
				info = &_plugins.back();
			}
			info->path = path;
			info->error = reply.value(u"error"_q).toString();
		}
		scanNext(files, index + 1, seen);
	});
}

void Engine::load(const QString &id) {
	const auto info = pluginMutable(id);
	if (!info || !_available) {
		return;
	}
	info->error = QString();
	call(u"enable"_q, QJsonObject{
		{ u"path"_q, info->path },
		{ u"plugin"_q, id },
		{ u"settings"_q, settingsOf(id) },
	}, [=](QJsonObject reply) {
		const auto info = pluginMutable(id);
		if (!info) {
			return;
		}
		if (reply.value(u"ok"_q).toBool()) {
			applyMeta(*info, reply.value(u"value"_q).toObject());
			info->loaded = true;
			LOG(("Plugins: %1 %2 loaded").arg(id, info->version));
		} else {
			info->loaded = false;
			info->error = reply.value(u"error"_q).toString();
			const auto trace = reply.value(u"trace"_q).toString();
			addLog(id, u"ERROR  "_q + (trace.isEmpty() ? info->error : trace));
			dropPluginState(id);
		}
		_changes.fire({});
	});
}

void Engine::dropPluginState(const QString &id) {
	const auto mine = [&](const HookReg &h) {
		return h.pluginId == id;
	};
	_sendMessageHooks.erase(
		ranges::remove_if(_sendMessageHooks, mine),
		end(_sendMessageHooks));
	for (auto i = _hooks.begin(); i != _hooks.end();) {
		i->second.erase(ranges::remove_if(i->second, mine), end(i->second));
		i = i->second.empty() ? _hooks.erase(i) : std::next(i);
	}
	_menuItems.erase(ranges::remove_if(_menuItems, [&](const MenuItem &m) {
		return m.pluginId == id;
	}), end(_menuItems));
}

void Engine::unload(const QString &id) {
	dropPluginState(id);
	if (const auto info = pluginMutable(id)) {
		info->loaded = false;
	}
	call(u"disable"_q, QJsonObject{ { u"plugin"_q, id } });
	_changes.fire({});
}

void Engine::setEnabled(const QString &id, bool enabled) {
	auto map = _state.value(u"enabled"_q).toObject();
	map.insert(id, enabled);
	_state.insert(u"enabled"_q, map);
	saveState();
	const auto info = pluginMutable(id);
	if (!info) {
		return;
	}
	info->enabled = enabled;
	if (enabled && !info->loaded) {
		load(id);
	} else if (!enabled && info->loaded) {
		unload(id);
	}
	_changes.fire({});
}

void Engine::install(
		const QString &path,
		Fn<void(QString name, QString error)> done) {
	if (!_available) {
		if (done) {
			done(QString(), _status);
		}
		return;
	}
	call(u"scan"_q, QJsonObject{ { u"path"_q, path } }, [=](QJsonObject reply) {
		if (!reply.value(u"ok"_q).toBool()) {
			if (done) {
				done(QString(), reply.value(u"error"_q).toString());
			}
			return;
		}
		const auto meta = reply.value(u"value"_q).toObject();
		const auto id = meta.value(u"id"_q).toString();
		const auto target = QDir(pluginsFolder()).filePath(id + u".plugin"_q);
		if (const auto existing = pluginMutable(id)) {
			if (existing->loaded) {
				unload(id);
			}
			if (existing->path != target) {
				QFile::remove(existing->path);
			}
		}
		if (QFileInfo(path).absoluteFilePath()
			!= QFileInfo(target).absoluteFilePath()) {
			QFile::remove(target);
			QFile::copy(path, target);
		}
		auto map = _state.value(u"enabled"_q).toObject();
		map.insert(id, true);
		_state.insert(u"enabled"_q, map);
		saveState();
		rescan();
		if (done) {
			done(meta.value(u"name"_q).toString(id), QString());
		}
	});
}

void Engine::remove(const QString &id) {
	const auto info = pluginMutable(id);
	if (!info) {
		return;
	}
	if (info->loaded) {
		unload(id);
	}
	QFile::remove(info->path);
	auto map = _state.value(u"enabled"_q).toObject();
	map.remove(id);
	_state.insert(u"enabled"_q, map);
	auto all = _state.value(u"settings"_q).toObject();
	all.remove(id);
	_state.insert(u"settings"_q, all);
	saveState();
	_plugins.erase(ranges::remove(_plugins, id, &PluginInfo::id), end(_plugins));
	_changes.fire({});
}

void Engine::reload(const QString &id) {
	if (const auto info = pluginMutable(id); info && info->loaded) {
		unload(id);
	}
	rescan();
}

// ---- hooks -----------------------------------------------------------------------------

bool Engine::bypassed() const {
	return (BypassDepth > 0) || (PluginSending > 0);
}

bool Engine::hasHook(mtpTypeId type) const {
	return !_hooks.empty() && _hooks.contains(type);
}

bool Engine::hasSendMessageHook() const {
	return !_sendMessageHooks.empty();
}

HookOutcome Engine::runHook(
		const QString &kind,
		mtpTypeId type,
		const QByteArray &tl,
		const QJsonObject &extra) {
	auto result = HookOutcome();
	if (bypassed() || QThread::currentThread() != thread()) {
		return result;
	}
	const auto i = _hooks.find(type);
	if (i == _hooks.end()) {
		return result;
	}
	auto list = i->second;
	ranges::stable_sort(list, ranges::greater(), &HookReg::priority);
	auto current = tl;
	for (const auto &hook : list) {
		auto fields = extra;
		fields.insert(u"plugin"_q, hook.pluginId);
		fields.insert(u"kind"_q, kind);
		fields.insert(u"cid"_q, qint64(type));
		fields.insert(u"account"_q, 0);
		if (!current.isEmpty()) {
			fields.insert(u"tl"_q, B64(current));
		}
		const auto reply = callSync(u"hook"_q, fields, kHookTimeout);
		if (!reply || !reply->value(u"ok"_q).toBool()) {
			addLog(hook.pluginId, u"ERROR  "_q + kind + u" hook failed: "_q
				+ (reply ? reply->value(u"error"_q).toString() : u"timeout"_q));
			continue;
		}
		const auto value = reply->value(u"value"_q).toObject();
		const auto strategy = value.value(u"strategy"_q).toString();
		if (strategy == u"CANCEL"_q) {
			result.cancel = true;
			return result;
		} else if (strategy == u"MODIFY"_q || strategy == u"MODIFY_FINAL"_q) {
			const auto bytes = FromB64(value.value(u"tl"_q));
			if (!bytes.isEmpty()) {
				current = bytes;
				result.modified = true;
				result.tl = bytes;
			}
			if (strategy == u"MODIFY_FINAL"_q) {
				return result;
			}
		}
	}
	return result;
}

HookOutcome Engine::runSendMessageHook(const QJsonObject &params) {
	auto result = HookOutcome();
	if (bypassed()
		|| _sendMessageHooks.empty()
		|| QThread::currentThread() != thread()) {
		return result;
	}
	auto list = _sendMessageHooks;
	ranges::stable_sort(list, ranges::greater(), &HookReg::priority);
	auto current = params;
	for (const auto &hook : list) {
		const auto reply = callSync(u"hook"_q, QJsonObject{
			{ u"plugin"_q, hook.pluginId },
			{ u"kind"_q, u"send_message"_q },
			{ u"name"_q, u"@send_message"_q },
			{ u"account"_q, 0 },
			{ u"obj"_q, current },
		}, kHookTimeout);
		if (!reply || !reply->value(u"ok"_q).toBool()) {
			continue;
		}
		const auto value = reply->value(u"value"_q).toObject();
		const auto strategy = value.value(u"strategy"_q).toString();
		if (strategy == u"CANCEL"_q) {
			result.cancel = true;
			return result;
		} else if (strategy == u"MODIFY"_q || strategy == u"MODIFY_FINAL"_q) {
			const auto obj = value.value(u"obj"_q).toObject();
			if (!obj.isEmpty()) {
				current = obj;
				result.modified = true;
				result.obj = obj;
			}
			if (strategy == u"MODIFY_FINAL"_q) {
				return result;
			}
		}
	}
	return result;
}

// ---- settings and callbacks ------------------------------------------------------------

void Engine::requestSettings(
		const QString &pluginId,
		const QString &subPageToken,
		Fn<void(QJsonArray items, QJsonObject values)> done) {
	const auto handle = [=](QJsonObject reply) {
		if (!reply.value(u"ok"_q).toBool()) {
			addLog(pluginId, u"ERROR  "_q + reply.value(u"error"_q).toString());
			done(QJsonArray(), settingsOf(pluginId));
			return;
		}
		const auto value = reply.value(u"value"_q).toObject();
		auto values = settingsOf(pluginId);
		const auto fresh = value.value(u"values"_q).toObject();
		for (auto i = fresh.begin(); i != fresh.end(); ++i) {
			values.insert(i.key(), i.value());
		}
		done(value.value(u"items"_q).toArray(), values);
	};
	if (subPageToken.isEmpty()) {
		call(u"settings"_q, QJsonObject{ { u"plugin"_q, pluginId } }, handle);
	} else {
		call(u"callback"_q, QJsonObject{
			{ u"plugin"_q, pluginId },
			{ u"token"_q, subPageToken },
			{ u"args"_q, QJsonArray() },
			{ u"returns"_q, u"settings"_q },
		}, handle);
	}
}

QJsonValue Engine::settingValue(
		const QString &pluginId,
		const QString &key) const {
	return settingsOf(pluginId).value(key);
}

void Engine::setSettingFromUi(
		const QString &pluginId,
		const QString &key,
		const QJsonValue &value,
		const QString &onChangeToken) {
	auto all = _state.value(u"settings"_q).toObject();
	auto mine = all.value(pluginId).toObject();
	mine.insert(key, value);
	all.insert(pluginId, mine);
	_state.insert(u"settings"_q, all);
	saveState();
	call(u"setting_changed"_q, QJsonObject{
		{ u"plugin"_q, pluginId },
		{ u"key"_q, key },
		{ u"value"_q, value },
	});
	if (!onChangeToken.isEmpty()) {
		invokeCallback(pluginId, onChangeToken, QJsonArray{ value });
	}
}

void Engine::invokeCallback(
		const QString &pluginId,
		const QString &token,
		QJsonArray args,
		bool wrap) {
	call(u"callback"_q, QJsonObject{
		{ u"plugin"_q, pluginId },
		{ u"token"_q, token },
		{ u"args"_q, args },
		{ u"wrap"_q, wrap },
	}, [=](QJsonObject reply) {
		if (!reply.value(u"ok"_q).toBool()) {
			const auto error = reply.value(u"error"_q).toString();
			addLog(pluginId, u"ERROR  "_q + error);
			::Ui::Toast::Show(error);
		}
	});
}

void Engine::menuClick(const MenuItem &item, QJsonObject context) {
	context.insert(u"account"_q, 0);
	invokeCallback(item.pluginId, item.token, QJsonArray{ context }, true);
}

} // namespace AyuPlugins

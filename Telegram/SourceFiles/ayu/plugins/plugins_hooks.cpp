// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
#include "ayu/plugins/plugins_hooks.h"

#include "ayu/plugins/plugins_engine.h"
#include "ayu/plugins/plugins_lang.h"
#include "api/api_common.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"

namespace AyuPlugins {
namespace {

constexpr auto kBotApiChannelShift = qint64(1000000000000);

// A plugin replaced an update: the client applies the replacement right away
// through the same function, which must not filter it a second time.
bool SkipNextUpdates = false;
bool SkipNextUpdate = false;

[[nodiscard]] QByteArray WordsToBytes(const mtpBuffer &buffer) {
	return QByteArray(
		reinterpret_cast<const char*>(buffer.constData()),
		buffer.size() * int(sizeof(mtpPrime)));
}

[[nodiscard]] mtpBuffer BytesToWords(const QByteArray &bytes) {
	auto result = mtpBuffer(bytes.size() / int(sizeof(mtpPrime)));
	memcpy(result.data(), bytes.constData(), result.size() * sizeof(mtpPrime));
	return result;
}

void WrapPostHooks(mtpTypeId type, MTP::ResponseHandler &callbacks) {
	callbacks.done = [type, done = std::move(callbacks.done)](
			const MTP::Response &response) mutable {
		auto &engine = Engine::Instance();
		if (!engine.hasHook(type)) {
			return done ? done(response) : true;
		}
		const auto outcome = engine.runHook(
			u"post_request"_q,
			type,
			WordsToBytes(response.reply));
		if (outcome.cancel) {
			return true;
		} else if (outcome.modified
			&& !(outcome.tl.size() % int(sizeof(mtpPrime)))) {
			auto copy = response;
			copy.reply = BytesToWords(outcome.tl);
			return done ? done(copy) : true;
		}
		return done ? done(response) : true;
	};
	if (const auto fail = callbacks.fail) {
		callbacks.fail = [type, fail](
				const MTP::Error &error,
				const MTP::Response &response) {
			auto &engine = Engine::Instance();
			if (engine.hasHook(type)) {
				engine.runHook(u"post_request"_q, type, QByteArray(), QJsonObject{
					{ u"error"_q, QJsonObject{
						{ u"code"_q, error.code() },
						{ u"text"_q, error.type() },
					} },
				});
			}
			return fail(error, response);
		};
	}
}

bool FilterRequest(
		mtpRequestId requestId,
		mtpBuffer &body,
		MTP::ResponseHandler &callbacks) {
	auto &engine = Engine::Instance();
	if (body.isEmpty() || engine.bypassed()) {
		return true;
	}
	const auto type = mtpTypeId(body[0]);
	if (!engine.hasHook(type)) {
		return true;
	}
	const auto outcome = engine.runHook(
		u"pre_request"_q,
		type,
		WordsToBytes(body));
	if (outcome.cancel) {
		// Let the caller clean up as if the server refused the request.
		if (const auto fail = callbacks.fail) {
			InvokeQueued(&engine, [=] {
				fail(
					MTP::Error::Local(
						u"PLUGIN_CANCELLED"_q,
						Tr("Cancelled")),
					MTP::Response{ .requestId = requestId });
			});
		}
		return false;
	}
	if (outcome.modified
		&& !outcome.tl.isEmpty()
		&& !(outcome.tl.size() % int(sizeof(mtpPrime)))) {
		body = BytesToWords(outcome.tl);
	}
	WrapPostHooks(type, callbacks);
	return true;
}

} // namespace

void InstallRequestFilter() {
	MTP::SetPluginRequestFilter(FilterRequest);
}

bool FilterUpdates(
		const MTPUpdates &updates,
		std::optional<MTPUpdates> &replaced) {
	if (base::take(SkipNextUpdates)) {
		return true;
	}
	auto &engine = Engine::Instance();
	const auto type = updates.type();
	if (engine.bypassed() || !engine.hasHook(type)) {
		return true;
	}
	const auto outcome = engine.runHook(u"updates"_q, type, SerializeTl(updates));
	if (outcome.cancel) {
		return false;
	} else if (outcome.modified) {
		if (auto parsed = DeserializeTl<MTPUpdates>(outcome.tl)) {
			replaced = std::move(parsed);
			SkipNextUpdates = true;
		}
	}
	return true;
}

bool FilterUpdate(
		const MTPUpdate &update,
		std::optional<MTPUpdate> &replaced) {
	if (base::take(SkipNextUpdate)) {
		return true;
	}
	auto &engine = Engine::Instance();
	const auto type = update.type();
	if (engine.bypassed() || !engine.hasHook(type)) {
		return true;
	}
	const auto outcome = engine.runHook(u"update"_q, type, SerializeTl(update));
	if (outcome.cancel) {
		return false;
	} else if (outcome.modified) {
		if (auto parsed = DeserializeTl<MTPUpdate>(outcome.tl)) {
			replaced = std::move(parsed);
			SkipNextUpdate = true;
		}
	}
	return true;
}

bool FilterOutgoingMessage(Api::MessageToSend &message) {
	auto &engine = Engine::Instance();
	if (engine.bypassed() || !engine.hasSendMessageHook()) {
		return true;
	}
	auto &action = message.action;
	const auto peer = action.history->peer;
	const auto text = message.textWithTags.text;
	const auto params = QJsonObject{
		{ u"message"_q, text },
		{ u"peer"_q, DialogIdFromPeer(peer->id) },
		{ u"peerType"_q, PeerTypeName(peer->id) },
		{ u"replyToMsgId"_q, qint64(action.replyTo.messageId.msg.bare) },
		{ u"notify"_q, !action.options.silent },
		{ u"scheduleDate"_q, qint64(action.options.scheduled) },
		{ u"hasEntities"_q, !message.textWithTags.tags.isEmpty() },
	};
	const auto outcome = engine.runSendMessageHook(params);
	if (outcome.cancel) {
		return false;
	} else if (!outcome.modified) {
		return true;
	}
	const auto &obj = outcome.obj;
	const auto newText = obj.value(u"message"_q).toString(text);
	if (newText != text) {
		message.textWithTags = TextWithTags{ newText, {} };
	}
	if (obj.contains(u"notify"_q)) {
		action.options.silent = !obj.value(u"notify"_q).toBool(true);
	}
	if (obj.contains(u"scheduleDate"_q)) {
		action.options.scheduled = TimeId(
			obj.value(u"scheduleDate"_q).toVariant().toLongLong());
	}
	return !message.textWithTags.text.isEmpty();
}

qint64 DialogIdFromPeer(PeerId peer) {
	if (peerIsUser(peer)) {
		return qint64(peerToUser(peer).bare);
	} else if (peerIsChat(peer)) {
		return -qint64(peerToChat(peer).bare);
	} else if (peerIsChannel(peer)) {
		return -qint64(peerToChannel(peer).bare);
	}
	return 0;
}

QString PeerTypeName(PeerId peer) {
	return peerIsUser(peer)
		? u"user"_q
		: peerIsChat(peer)
		? u"chat"_q
		: u"channel"_q;
}

PeerId PeerFromDialogId(not_null<Main::Session*> session, qint64 dialogId) {
	if (dialogId > 0) {
		return peerFromUser(UserId(BareId(dialogId)));
	} else if (!dialogId) {
		return PeerId();
	}
	const auto id = -dialogId;
	if (id > kBotApiChannelShift) {
		return peerFromChannel(ChannelId(BareId(id - kBotApiChannelShift)));
	}
	if (session->data().channelLoaded(ChannelId(BareId(id)))) {
		return peerFromChannel(ChannelId(BareId(id)));
	} else if (session->data().chatLoaded(ChatId(BareId(id)))) {
		return peerFromChat(ChatId(BareId(id)));
	}
	return peerFromChannel(ChannelId(BareId(id)));
}

} // namespace AyuPlugins

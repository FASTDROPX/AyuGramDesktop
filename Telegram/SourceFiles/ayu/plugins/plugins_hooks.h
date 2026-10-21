// This is the source code of AyuGram for Desktop.
//
// exteraGram-compatible plugin engine for AyuGram Desktop.
// Integration points: the few places in the client that call plugin hooks.
#pragma once

#include "data/data_peer_id.h"

namespace Main {
class Session;
} // namespace Main

namespace Api {
struct MessageToSend;
} // namespace Api

namespace AyuPlugins {

// Installs the MTProto request filter (pre/post request hooks).
void InstallRequestFilter();

// Update containers (updates, updateShortMessage, ...) and single updates.
// Return false to drop. `replaced` is set when a plugin changed the object;
// apply it with an Engine::Bypass alive so it is not filtered twice.
[[nodiscard]] bool FilterUpdates(
	const MTPUpdates &updates,
	std::optional<MTPUpdates> &replaced);
[[nodiscard]] bool FilterUpdate(
	const MTPUpdate &update,
	std::optional<MTPUpdate> &replaced);

// on_send_message_hook. Return false when a plugin cancelled the message.
[[nodiscard]] bool FilterOutgoingMessage(Api::MessageToSend &message);

// Telegram for Android dialog ids: users > 0, chats and channels < 0.
[[nodiscard]] qint64 DialogIdFromPeer(PeerId peer);
[[nodiscard]] QString PeerTypeName(PeerId peer);
[[nodiscard]] PeerId PeerFromDialogId(not_null<Main::Session*> session, qint64 dialogId);

// TL (de)serialization of generated MTP types.
template <typename T>
[[nodiscard]] QByteArray SerializeTl(const T &value) {
	auto buffer = mtpBuffer();
	value.template write<mtpBuffer>(buffer);
	return QByteArray(
		reinterpret_cast<const char*>(buffer.constData()),
		buffer.size() * int(sizeof(mtpPrime)));
}

template <typename T>
[[nodiscard]] std::optional<T> DeserializeTl(const QByteArray &bytes) {
	if (bytes.isEmpty() || (bytes.size() % int(sizeof(mtpPrime)))) {
		return std::nullopt;
	}
	auto buffer = mtpBuffer(bytes.size() / int(sizeof(mtpPrime)));
	memcpy(buffer.data(), bytes.constData(), bytes.size());
	auto result = T();
	auto from = buffer.constData();
	if (!result.read(from, from + buffer.size())) {
		return std::nullopt;
	}
	return result;
}

} // namespace AyuPlugins

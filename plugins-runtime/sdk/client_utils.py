"""client_utils for AyuGram Desktop.

Same names as the Android SDK. Requests are TL objects (`org.telegram.tgnet.TLRPC`)
and go to Telegram through the client's own MTProto connection, so
`send_request(TLRPC.TL_messages_getHistory(...), callback)` works like on Android.
"""

import base64
import functools
from typing import Any, Callable, Optional

import _bridge
import _markup as markup
import tl
from tl import TLRPC, TL_error

STAGE_QUEUE = "stage"
GLOBAL_QUEUE = "global"
CACHE_CLEAR_QUEUE = "cache_clear"
SEARCH_QUEUE = "search"
PHONE_BOOK_QUEUE = "phone_book"
THEME_QUEUE = "theme"
EXTERNAL_NETWORK_QUEUE = "external_network"
PLUGINS_QUEUE = "plugins"

_QUEUES = {STAGE_QUEUE, GLOBAL_QUEUE, CACHE_CLEAR_QUEUE, SEARCH_QUEUE, PHONE_BOOK_QUEUE,
           THEME_QUEUE, EXTERNAL_NETWORK_QUEUE, PLUGINS_QUEUE}


def run_on_queue(fn: Callable[[], None], queue_name: str = PLUGINS_QUEUE, delay_ms: int = 0) -> None:
    _bridge.schedule(queue_name if queue_name in _QUEUES else PLUGINS_QUEUE, fn, delay_ms)


def get_queue_by_name(queue_name: str):
    return queue_name if queue_name in _QUEUES else None


# ---- raw requests -----------------------------------------------------------------------

def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _decode_response(value):
    if value is None:
        return None
    return tl.decode(base64.b64decode(value))


def _error(error):
    if isinstance(error, dict):
        return TL_error(int(error.get("code", 400)), str(error.get("text", "")))
    return TL_error(400, str(error))


class RequestCallback:
    """Kept for compatibility: wraps a `(response, error)` callable."""

    def __init__(self, fn):
        self.fn = fn

    def run(self, response, error):
        self.fn(response, error)


def send_request(request: Any, fn: Optional[Callable] = None, account: Optional[int] = None) -> int:
    """Sends a TL request through the client. `fn(response, error)` runs on the plugins queue;
    `error` is TLRPC.TL_error (code, text) like on Android."""
    if isinstance(fn, RequestCallback):
        fn = fn.fn
    payload = _b64(tl.encode(request))

    def callback(value, error):
        if fn is None:
            return
        if error is not None:
            fn(None, _error(error))
            return
        try:
            response = _decode_response(value)
        except Exception as exc:  # noqa: BLE001 - a schema mismatch must not kill the plugin
            fn(None, TL_error(500, f"RESPONSE_DECODE_FAILED: {exc}"))
            return
        fn(response, None)

    return _bridge.request_async("tl", callback, payload=payload, account=account or 0)


def request_sync(request: Any, account: Optional[int] = None, timeout: float = 30.0):
    """Desktop addition: sends a TL request and waits. Raises RuntimeError(TL_error text).
    Do not call it from inside a hook (the client waits for the hook)."""
    try:
        value = _bridge.request("tl", timeout, payload=_b64(tl.encode(request)), account=account or 0)
    except _bridge.BridgeError as exc:
        raise RuntimeError(str(exc)) from None
    return _decode_response(value)


# ---- messages ---------------------------------------------------------------------------

def _dialog_id(peer):
    """Android dialog id from an int, a peer/inputPeer TL object or a dict."""
    if isinstance(peer, int):
        return peer
    if isinstance(peer, tl.TLObject):
        if peer.user_id is not None:
            return int(peer.user_id)
        if peer.chat_id is not None:
            return -int(peer.chat_id)
        if peer.channel_id is not None:
            return -int(peer.channel_id)
    if isinstance(peer, dict):
        return int(peer.get("dialog_id") or peer.get("id"))
    return int(peer)


def _reply_id(reply):
    if reply is None:
        return None
    if isinstance(reply, int):
        return reply
    owner = getattr(reply, "messageOwner", None)
    if owner is not None:
        return owner.id
    return getattr(reply, "id", None)


def send_message(params: Any, parse_mode: Optional[str] = None, account: Optional[int] = None):
    """`params`: {"peer", "message", "replyToMsg", "notify", "scheduleDate", "path", "caption"}."""
    data = dict(params.to_dict() if hasattr(params, "to_dict") else params)
    if parse_mode is not None and parse_mode.lower() not in ("html", "markdown"):
        raise ValueError("parse_mode must be 'html' or 'markdown'")
    data["peer"] = _dialog_id(data.get("peer"))
    data["replyToMsg"] = _reply_id(data.get("replyToMsg"))
    entities = data.pop("entities", None)
    if parse_mode is not None and parse_mode.lower() == "html":
        data["message"], entities = markup.parse_html(data.get("message") or "")
        parse_mode = None
    if entities and all(isinstance(e, tl.TLObject) for e in entities):
        data["entities_tl"] = _b64(tl.encode_vector_of(entities, "MessageEntity"))
    return _bridge.request_async("send_message", None, params=data, parse_mode=parse_mode, account=account or 0)


def _format(text: str, parse_mode: Optional[str], account: int = 0):
    """(plain_text, [TLRPC.MessageEntity]) for html / markdown / plain text."""
    mode = (parse_mode or "").lower()
    if mode == "html":
        return markup.parse_html(text)
    if mode == "markdown":
        value = _bridge.request("parse_text", 10, text=text, parse_mode="markdown", account=account)
        raw = value.get("entities")
        entities = tl.decode_vector_of(base64.b64decode(raw), "MessageEntity") if raw else []
        return value.get("text", text), list(entities)
    return text, []


def send_text(peer_id, text: str, replyToMsg=None, parse_mode: Optional[str] = None):
    return send_message({"peer": peer_id, "message": text, "replyToMsg": replyToMsg}, parse_mode)


def send_photo(peer_id, path: str, caption: Optional[str] = None, high_quality: bool = False,
               parse_mode: Optional[str] = None):
    return send_message({"peer": peer_id, "path": path, "kind": "photo", "caption": caption}, parse_mode)


def send_document(peer_id, path: str, caption: Optional[str] = None):
    return send_message({"peer": peer_id, "path": path, "kind": "document", "caption": caption})


def send_video(peer_id, path: str, caption: Optional[str] = None):
    return send_message({"peer": peer_id, "path": path, "kind": "video", "caption": caption})


def send_audio(peer_id, path: str, caption: Optional[str] = None):
    return send_message({"peer": peer_id, "path": path, "kind": "audio", "caption": caption})


def edit_message(message_obj, text: Optional[str] = None, file_path: Optional[str] = None,
                 parse_mode: Optional[str] = None, with_spoiler: bool = False,
                 callback: Optional[Callable] = None, account: int = 0):
    """Edits the text of a message (TL_messages_editMessage). Not from inside a hook:
    it asks the client for the chat's InputPeer and waits for the answer."""
    if parse_mode is not None and parse_mode.lower() not in ("html", "markdown"):
        raise ValueError("parse_mode must be 'html' or 'markdown'")
    if file_path:
        raise NotImplementedError("Editing media is not supported on Desktop yet")
    owner = getattr(message_obj, "messageOwner", message_obj)
    peer = get_input_peer(MessageObject.dialog_of(owner), account)
    if peer is None:
        raise RuntimeError("PEER_ID_INVALID")
    request = TLRPC.TL_messages_editMessage(peer=peer, id=int(owner.id))
    if text is not None:
        request.message, entities = _format(text, parse_mode, account)
        if entities:
            request.entities = entities
    return send_request(request, callback, account)


class MessageObject:
    """What Android passes around as MessageObject: `.messageOwner` is the TL message."""

    def __init__(self, message_owner):
        self.messageOwner = message_owner

    @staticmethod
    def dialog_of(message):
        peer = message.peer_id
        if peer is None:
            return 0
        return _dialog_id(peer)

    def getId(self):
        return self.messageOwner.id

    def getDialogId(self):
        return self.dialog_of(self.messageOwner)

    def getFromChatId(self):
        return _dialog_id(self.messageOwner.from_id) if self.messageOwner.from_id else self.getDialogId()

    def isOut(self):
        return bool(self.messageOwner.out)

    @property
    def messageText(self):
        return self.messageOwner.message

    def __repr__(self):
        return f"MessageObject({self.messageOwner!r})"


def message_from_context(data):
    """Builds TL_message + MessageObject from the dict the client puts into menu contexts."""
    if not isinstance(data, dict):
        return data
    msg = TLRPC.TL_message()
    msg.id = data.get("id", 0)
    msg.message = data.get("text", "")
    msg.date = data.get("date", 0)
    msg.out = bool(data.get("out"))
    dialog = int(data.get("dialog_id", 0))
    msg.peer_id = _peer_for(dialog, data.get("peer_type"))
    if data.get("from_id"):
        msg.from_id = _peer_for(int(data["from_id"]), data.get("from_type"))
    if data.get("reply_to_msg_id"):
        msg.reply_to = TLRPC.TL_messageReplyHeader(reply_to_msg_id=data["reply_to_msg_id"])
    return MessageObject(msg)


def _peer_for(dialog_id, kind=None):
    if kind == "user" or (kind is None and dialog_id > 0):
        return TLRPC.TL_peerUser(user_id=abs(dialog_id))
    if kind == "chat":
        return TLRPC.TL_peerChat(chat_id=abs(dialog_id))
    return TLRPC.TL_peerChannel(channel_id=abs(dialog_id))


# ---- account helpers --------------------------------------------------------------------

def get_my_id() -> int:
    return int(_bridge.request("my_id", 10))


def get_input_peer(dialog_id: int, account: int = 0):
    """InputPeer with access hash for a dialog the client knows (Desktop addition)."""
    value = _bridge.request("input_peer", 10, dialog_id=int(dialog_id), account=account)
    return _decode_response(value)


def get_peer_info(dialog_id: int, account: int = 0):
    """User/chat as a TL object with the fields the client has (id, names, username, ...)."""
    info = _bridge.request("peer_info", 10, dialog_id=int(dialog_id), account=account)
    if not info:
        return None
    kind = info.pop("_", "user")
    cls = {"user": TLRPC.TL_user, "chat": TLRPC.TL_chat, "channel": TLRPC.TL_channel}.get(kind, TLRPC.TL_user)
    return cls(**info)


class _Unsupported:
    def __init__(self, name):
        self._name = name

    def __getattr__(self, attr):
        raise NotImplementedError(
            f"{self._name}.{attr} is part of Telegram for Android and does not exist in AyuGram Desktop")


class _Fragment(_Unsupported):
    def __init__(self):
        super().__init__("BaseFragment")

    def getParentActivity(self):
        return self

    def getContext(self):
        return self


class _UserConfig(_Unsupported):
    def __init__(self, account):
        super().__init__("UserConfig")
        self._account = account

    def getClientUserId(self):
        return get_my_id()

    def getCurrentUser(self):
        return get_peer_info(get_my_id(), self._account)


class _MessagesController(_Unsupported):
    def __init__(self, account):
        super().__init__("MessagesController")
        self._account = account

    def getUser(self, user_id):
        return get_peer_info(int(user_id), self._account)

    def getChat(self, chat_id):
        return get_peer_info(-abs(int(chat_id)), self._account)

    def getInputPeer(self, dialog_id):
        return get_input_peer(int(dialog_id), self._account)


class _ConnectionsManager(_Unsupported):
    def __init__(self, account):
        super().__init__("ConnectionsManager")
        self._account = account

    def sendRequest(self, request, delegate=None, flags=0):
        return send_request(request, getattr(delegate, "run", delegate), self._account)


def get_last_fragment():
    return _Fragment()


def get_user_config(account: int = 0):
    return _UserConfig(account)


def get_messages_controller(account: int = 0):
    return _MessagesController(account)


def get_connections_manager(account: int = 0):
    return _ConnectionsManager(account)


def get_account_instance(account: int = 0):
    return for_account(account)


def _unsupported(name):
    def getter(account: int = 0):
        return _Unsupported(name)
    return getter


get_contacts_controller = _unsupported("ContactsController")
get_media_data_controller = _unsupported("MediaDataController")
get_location_controller = _unsupported("LocationController")
get_notifications_controller = _unsupported("NotificationsController")
get_messages_storage = _unsupported("MessagesStorage")
get_send_messages_helper = _unsupported("SendMessagesHelper")
get_file_loader = _unsupported("FileLoader")
get_secret_chat_helper = _unsupported("SecretChatHelper")
get_download_controller = _unsupported("DownloadController")
get_notifications_settings = _unsupported("NotificationsSettings")
get_notification_center = _unsupported("NotificationCenter")


def get_media_controller():
    return _Unsupported("MediaController")


class _AccountClient:
    def __init__(self, account):
        self.account = account

    def send_request(self, request, fn=None):
        return send_request(request, fn, self.account)

    def request_sync(self, request, timeout=30.0):
        return request_sync(request, self.account, timeout)

    def send_message(self, params, parse_mode=None):
        return send_message(params, parse_mode, self.account)

    def send_text(self, peer_id, text, replyToMsg=None, parse_mode=None):
        return send_message({"peer": peer_id, "message": text, "replyToMsg": replyToMsg},
                            parse_mode, self.account)


@functools.lru_cache(maxsize=None)
def for_account(account: int = 0):
    return _AccountClient(account)

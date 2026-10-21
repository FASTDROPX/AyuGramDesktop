"""exteraGram plugin base classes, as on Android (SDK 1.4.x), for AyuGram Desktop (MTProto/TL)."""

import enum
import json
import re
import time
import traceback
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

import _bridge


class HookStrategy(enum.Enum):
    DEFAULT = "DEFAULT"
    CANCEL = "CANCEL"
    MODIFY = "MODIFY"
    MODIFY_FINAL = "MODIFY_FINAL"


class HookResult:
    def __init__(self, strategy: HookStrategy = HookStrategy.DEFAULT, request: Any = None,
                 response: Any = None, update: Any = None, updates: Any = None, params: Any = None):
        self.strategy = strategy
        self.request = request
        self.response = response
        self.update = update
        self.updates = updates
        self.params = params


class AppEvent(enum.Enum):
    START = "START"
    STOP = "STOP"
    PAUSE = "PAUSE"
    RESUME = "RESUME"


class MenuItemType(enum.Enum):
    MESSAGE_CONTEXT_MENU = "MESSAGE_CONTEXT_MENU"
    DRAWER_MENU = "DRAWER_MENU"
    MAIN_MENU = "MAIN_MENU"
    CHAT_ACTION_MENU = "CHAT_ACTION_MENU"
    PROFILE_ACTION_MENU = "PROFILE_ACTION_MENU"


@dataclass
class MenuItemData:
    menu_type: MenuItemType
    text: str
    on_click: Callable[[Dict[str, Any]], None]
    item_id: Optional[str] = None
    icon: Optional[str] = None
    subtext: Optional[str] = None
    condition: Optional[str] = None
    priority: int = 0


def resolve_hook_ids(name, match_substring=False):
    """Constructor ids for a hook name: "TL_messages_setTyping", "messages.setTyping",
    or with match_substring every constructor whose name contains the text."""
    import tl
    if match_substring:
        return tl.schema().ids_matching(name)
    comb = tl.schema().find(name)
    return [comb.id] if comb else []


class PluginLogger:
    LEVELS = ("debug", "info", "warning", "error")

    def __init__(self, plugin_id, child=None):
        self._pid = plugin_id
        self._prefix = f"[{child}] " if child else ""

    def _emit(self, level, msg, args):
        text = str(msg)
        if args:
            try:
                text = text % args
            except (TypeError, ValueError):
                text = " ".join([text, *map(str, args)])
        _bridge.log(self._pid, level, self._prefix + text)

    def debug(self, msg, *args):
        self._emit("debug", msg, args)

    def info(self, msg, *args):
        self._emit("info", msg, args)

    def warning(self, msg, *args):
        self._emit("warning", msg, args)

    warn = warning

    def error(self, msg, *args):
        self._emit("error", msg, args)

    def exception(self, msg, *args):
        self._emit("error", f"{msg}\n{traceback.format_exc()}", args)

    def dump(self, obj):
        to_dict = getattr(obj, "to_dict", None)
        try:
            text = json.dumps(to_dict() if callable(to_dict) else obj, ensure_ascii=False, indent=2, default=repr)
        except (TypeError, ValueError):
            text = repr(obj)
        self._emit("debug", text, ())

    def child(self, name):
        return PluginLogger(self._pid, name)

    def timer(self, label="timer"):
        logger = self

        class _Timer:
            def __enter__(self):
                self.start = time.perf_counter()
                return self

            def __exit__(self, *exc):
                logger.debug("%s: %.1f ms", label, (time.perf_counter() - self.start) * 1000)
                return False

        return _Timer()


class BasePlugin:
    """Base class for exteraGram plugins.

    Lifecycle, settings, hooks and menu items follow the Android SDK. Hook
    payloads are TL objects (`org.telegram.tgnet.TLRPC`), as on Android.
    """

    def __init__(self):
        self.id = None
        self.name = None
        self._settings = {}
        self._hooks = {}
        self._send_message_hook = False
        self._menu_items = {}
        self.logger = None

    # Called by the host before on_plugin_load.
    def _attach(self, plugin_id, name, settings):
        self.id = plugin_id
        self.name = name
        self._settings = dict(settings or {})
        self.logger = PluginLogger(plugin_id)

    # ---- lifecycle -------------------------------------------------------
    def on_plugin_load(self) -> None:
        pass

    def on_plugin_unload(self) -> None:
        pass

    def on_app_event(self, event_type: AppEvent) -> None:
        pass

    def create_settings(self) -> List[Any]:
        return []

    # ---- settings --------------------------------------------------------
    def get_setting(self, key: str, default=None) -> Any:
        return self._settings.get(key, default)

    def set_setting(self, key: str, value: Any, reload_settings: bool = False) -> None:
        self._settings[key] = value
        _bridge.send({"t": "setting", "plugin": self.id, "key": key, "value": value,
                      "reload": bool(reload_settings)})

    def export_settings(self) -> Any:
        return dict(self._settings)

    def import_settings(self, settings: Any, reload_settings: bool = True) -> None:
        self._settings.update(settings or {})
        _bridge.send({"t": "settings_all", "plugin": self.id, "values": self._settings,
                      "reload": bool(reload_settings)})

    # ---- hooks -----------------------------------------------------------
    def add_hook(self, name: str, match_substring: bool = False, priority: int = 0) -> None:
        ids = resolve_hook_ids(name, match_substring)
        if not ids:
            self.logger.warning("add_hook: %s is not in the TL schema of this client", name)
            return
        self._hooks[name] = (ids, bool(match_substring))
        _bridge.send({"t": "hook_add", "plugin": self.id, "name": name, "ids": ids,
                      "match_substring": bool(match_substring), "priority": int(priority)})

    def remove_hook(self, name: str) -> None:
        if self._hooks.pop(name, None) is not None:
            _bridge.send({"t": "hook_remove", "plugin": self.id, "name": name})

    def add_on_send_message_hook(self, priority: int = 0) -> None:
        self._send_message_hook = True
        _bridge.send({"t": "hook_add", "plugin": self.id, "name": "@send_message", "ids": [],
                      "match_substring": False, "priority": int(priority)})

    def _hook_name_for(self, constructor_id, tl_name):
        """The name the plugin registered (for substring hooks it is the pattern's match)."""
        for name, (ids, substring) in self._hooks.items():
            if constructor_id in ids:
                return tl_name if substring else name
        return tl_name

    # Default hook implementations: do nothing.
    def pre_request_hook(self, request_name: str, account: int, request: Any) -> HookResult:
        return HookResult()

    def post_request_hook(self, request_name: str, account: int, response: Any, error: Any) -> HookResult:
        return HookResult()

    def on_update_hook(self, update_name: str, account: int, update: Any) -> HookResult:
        return HookResult()

    def on_updates_hook(self, container_name: str, account: int, updates: Any) -> HookResult:
        return HookResult()

    def on_send_message_hook(self, account: int, params: Any) -> HookResult:
        return HookResult()

    # ---- menu items ------------------------------------------------------
    def add_menu_item(self, data: MenuItemData) -> str:
        item_id = data.item_id or f"{self.id}_{len(self._menu_items) + 1}_{int(time.time() * 1000) % 100000}"
        self._menu_items[item_id] = data
        menu_type = data.menu_type.value if isinstance(data.menu_type, MenuItemType) else str(data.menu_type)
        _bridge.send({"t": "menu_add", "plugin": self.id, "item": {
            "id": item_id, "menu_type": menu_type, "text": data.text, "subtext": data.subtext,
            "icon": data.icon, "condition": data.condition, "priority": data.priority,
            "token": _bridge.register_callback(data.on_click)}})
        return item_id

    def remove_menu_item(self, item_id: str) -> None:
        if self._menu_items.pop(item_id, None) is not None:
            _bridge.send({"t": "menu_remove", "plugin": self.id, "id": item_id})

    # ---- misc ------------------------------------------------------------
    def log(self, message) -> None:
        _bridge.log(self.id, "debug", message)

    def client(self, account: int = 0):
        import client_utils
        return client_utils.for_account(account)


_ID_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_-]{1,31}$")


def valid_plugin_id(value):
    return isinstance(value, str) and bool(_ID_RE.match(value))

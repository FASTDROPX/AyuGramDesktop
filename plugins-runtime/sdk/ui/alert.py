"""AlertDialogBuilder with the Android SDK method names, shown as a Windows dialog."""

from typing import Callable, Dict, List, Optional

import _bridge


class AlertDialogBuilder:
    ALERT_TYPE_MESSAGE = 0
    ALERT_TYPE_LOADING = 1
    ALERT_TYPE_SPINNER = 2

    BUTTON_POSITIVE = -1
    BUTTON_NEGATIVE = -2
    BUTTON_NEUTRAL = -3

    def __init__(self, context=None, progress_style: int = ALERT_TYPE_MESSAGE, resources_provider=None):
        self._style = progress_style
        self._title = None
        self._message = None
        self._items = None
        self._buttons = {}
        self._red = set()
        self._listeners: Dict[str, Callable] = {}
        self._cancelable = True
        self._dialog_id = None
        self._shown = False
        self._progress = 0

    # content
    def set_title(self, title: str):
        self._title = title
        self._update()
        return self

    def set_message(self, message: str):
        self._message = message
        self._update()
        return self

    def set_message_text_view_clickable(self, clickable: bool):
        return self

    def set_view(self, view, height: int = -2):
        return self

    def set_items(self, items: List[str], listener: Optional[Callable] = None, icons=None):
        self._items = [str(i) for i in items]
        if listener:
            self._listeners["items"] = listener
        return self

    # buttons
    def _button(self, which, text, listener):
        self._buttons[which] = text
        if listener:
            self._listeners[str(which)] = listener
        return self

    def set_positive_button(self, text: str, listener: Optional[Callable] = None):
        return self._button(self.BUTTON_POSITIVE, text, listener)

    def set_negative_button(self, text: str, listener: Optional[Callable] = None):
        return self._button(self.BUTTON_NEGATIVE, text, listener)

    def set_neutral_button(self, text: str, listener: Optional[Callable] = None):
        return self._button(self.BUTTON_NEUTRAL, text, listener)

    def make_button_red(self, button_type: int):
        self._red.add(button_type)
        return self

    # listeners
    def set_on_back_button_listener(self, listener=None):
        return self

    def set_on_dismiss_listener(self, listener=None):
        if listener:
            self._listeners["dismiss"] = listener
        return self

    def set_on_cancel_listener(self, listener=None):
        if listener:
            self._listeners["cancel"] = listener
        return self

    # appearance — Android-only knobs are accepted and ignored
    def set_top_image(self, *args, **kwargs):
        return self

    def set_top_drawable(self, *args, **kwargs):
        return self

    def set_top_animation(self, *args, **kwargs):
        return self

    def set_dim_enabled(self, enabled: bool):
        return self

    def set_dialog_button_color_key(self, theme_key: int):
        return self

    def set_blurred_background(self, blur: bool, blur_behind_if_possible: bool = True):
        return self

    def set_cancelable(self, cancelable: bool):
        self._cancelable = bool(cancelable)
        return self

    def set_canceled_on_touch_outside(self, cancel: bool):
        return self

    # lifecycle
    def create(self):
        return self

    def _event(self, kind, which=0):
        listener = None
        if kind == "button":
            listener = self._listeners.get(str(which))
        elif kind == "item":
            listener = self._listeners.get("items")
        elif kind in ("dismiss", "cancel"):
            listener = self._listeners.get(kind)
            if kind == "dismiss":
                self._shown = False
        if listener is None:
            if kind in ("button", "item"):
                self.dismiss()
            return
        if kind in ("dismiss", "cancel"):
            listener(self)
        else:
            listener(self, which)

    def show(self):
        if self._dialog_id is None:
            self._dialog_id = _bridge.register_callback(self._event)
        self._shown = True
        _bridge.send({"t": "alert", "plugin": _bridge.plugin_id(), "op": "show", "id": self._dialog_id,
                      **self._state()})
        return self

    def _state(self):
        return {"style": self._style, "title": self._title, "message": self._message, "items": self._items,
                "buttons": {str(k): v for k, v in self._buttons.items()}, "red": sorted(self._red),
                "cancelable": self._cancelable, "progress": self._progress}

    def _update(self):
        if self._shown:
            _bridge.send({"t": "alert", "plugin": _bridge.plugin_id(), "op": "update",
                          "id": self._dialog_id, **self._state()})

    def dismiss(self):
        if self._dialog_id is not None and self._shown:
            self._shown = False
            _bridge.send({"t": "alert", "plugin": _bridge.plugin_id(), "op": "dismiss", "id": self._dialog_id})

    def get_dialog(self):
        return self if self._shown else None

    def get_button(self, button_type: int):
        return None

    def set_progress(self, progress: int):
        self._progress = max(0, min(100, int(progress)))
        self._update()
        return self

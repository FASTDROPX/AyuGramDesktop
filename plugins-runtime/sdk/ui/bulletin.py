"""Bulletins (the small notices at the bottom of the window)."""

from typing import Callable, Optional

import _bridge


def _show(kind, text, subtitle=None, button=None, on_click=None, on_undo=None, duration=2750):
    _bridge.send({"t": "bulletin", "plugin": _bridge.plugin_id(), "kind": kind, "text": str(text),
                  "subtitle": subtitle, "button": button,
                  "on_click": _bridge.register_callback(on_click) if on_click else None,
                  "on_undo": _bridge.register_callback(on_undo) if on_undo else None,
                  "duration": int(duration)})


class BulletinHelper:
    DURATION_SHORT = 1500
    DURATION_LONG = 2750
    DURATION_PROLONG = 5000

    @staticmethod
    def show_info(message: str, fragment=None):
        _show("info", message)

    @staticmethod
    def show_error(message: str, fragment=None):
        _show("error", message)

    @staticmethod
    def show_success(message: str, fragment=None):
        _show("success", message)

    @staticmethod
    def show_simple(text: str, icon_res_id=None, fragment=None):
        _show("info", text)

    @staticmethod
    def show_two_line(title: str, subtitle: str, icon_res_id=None, fragment=None):
        _show("info", title, subtitle=subtitle)

    @staticmethod
    def show_with_button(text: str, icon_res_id, button_text: str, on_click: Optional[Callable[[], None]],
                         fragment=None, duration: int = 5000):
        _show("info", text, button=button_text, on_click=on_click, duration=duration)

    @staticmethod
    def show_undo(text: str, on_undo: Callable[[], None], on_action: Optional[Callable[[], None]] = None,
                  subtitle: Optional[str] = None, fragment=None):
        _show("undo", text, subtitle=subtitle, button="Undo", on_click=on_action, on_undo=on_undo,
              duration=5000)

    @staticmethod
    def show_copied_to_clipboard(message: Optional[str] = None, fragment=None):
        _show("success", message or "Copied to clipboard")

    @staticmethod
    def show_link_copied(is_private_link_info: bool = False, fragment=None):
        _show("success", "Link copied to clipboard")

    @staticmethod
    def show_file_saved_to_gallery(is_video: bool = False, amount: int = 1, fragment=None):
        _show("success", "Video saved" if is_video else "Photo saved")

    @staticmethod
    def show_file_saved_to_downloads(file_type_enum_name: str = "UNKNOWN", amount: int = 1, fragment=None):
        _show("success", "Saved to Downloads")

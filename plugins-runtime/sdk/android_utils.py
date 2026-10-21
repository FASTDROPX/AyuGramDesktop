"""android_utils for exteraGram Desktop: the helpers that make sense on Windows."""

import json

import _bridge


def log(data) -> None:
    if not isinstance(data, (str, int, float, bool, type(None))):
        to_dict = getattr(data, "to_dict", None)
        try:
            data = json.dumps(to_dict() if callable(to_dict) else data, ensure_ascii=False, default=repr)
        except (TypeError, ValueError):
            data = repr(data)
    _bridge.log(_bridge.plugin_id(), "debug", data)


def run_on_ui_thread(func, delay: int = 0) -> None:
    """Runs `func` on the plugin UI queue. Plugin code never touches WPF
    directly, so this is a serial queue rather than the real UI thread."""
    _bridge.schedule("ui", func, delay)


def copy_to_clipboard(text) -> bool:
    _bridge.send({"t": "clipboard", "plugin": _bridge.plugin_id(), "text": str(text)})
    return True


def R(fn):
    """Runnable wrapper: on Windows any Python callable already is one."""
    fn.run = fn
    return fn


def OnClickListener(fn):
    return fn


def OnLongClickListener(fn):
    return fn

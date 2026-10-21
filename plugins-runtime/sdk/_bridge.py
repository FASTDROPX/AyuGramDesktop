"""Channel between plugins and exteraGram Desktop.

The host process talks to the app through stdin/stdout, one JSON object per
line. Plugins never touch the channel directly: SDK modules call `send`,
`request` and `request_async`, and `plugin_host.py` feeds replies in through
`deliver`.
"""

import itertools
import json
import threading
import traceback

_out = None
_write_lock = threading.Lock()
_request_ids = itertools.count(1)
_pending = {}
_pending_lock = threading.Lock()

_callbacks = {}
_callback_ids = itertools.count(1)
_callback_owner = {}

# Set by plugin_host: schedules a callable on a named queue.
_schedule = None
# Set by plugin_host: id of the plugin whose code is running on this thread.
current_plugin = threading.local()


class BridgeError(RuntimeError):
    pass


def attach(stream, schedule):
    global _out, _schedule
    _out = stream
    _schedule = schedule


def _default(obj):
    to_dict = getattr(obj, "to_dict", None)
    if callable(to_dict):
        return to_dict()
    if callable(obj):
        return register_callback(obj)
    return repr(obj)


def send(msg):
    data = json.dumps(msg, ensure_ascii=False, default=_default)
    with _write_lock:
        _out.write(data + "\n")
        _out.flush()


def request(fn, timeout=30.0, **payload):
    """Ask the app to do something and wait for its answer."""
    rid = next(_request_ids)
    slot = {"event": threading.Event()}
    with _pending_lock:
        _pending[rid] = slot
    send({"t": "req", "id": rid, "fn": fn, "plugin": plugin_id(), **payload})
    if not slot["event"].wait(timeout):
        with _pending_lock:
            _pending.pop(rid, None)
        raise BridgeError(f"exteraGram Desktop did not answer '{fn}' in {timeout:.0f} s")
    if not slot.get("ok", True):
        raise BridgeError(slot.get("error") or f"'{fn}' failed")
    return slot.get("value")


def request_async(fn, callback=None, queue="plugins", **payload):
    """Ask the app to do something; `callback(value, error)` runs on `queue`."""
    rid = next(_request_ids)
    slot = {"callback": callback, "queue": queue, "plugin": plugin_id()}
    with _pending_lock:
        _pending[rid] = slot
    send({"t": "req", "id": rid, "fn": fn, "plugin": plugin_id(), **payload})
    return rid


def deliver(msg):
    with _pending_lock:
        slot = _pending.pop(msg.get("id"), None)
    if slot is None:
        return
    slot["ok"] = msg.get("ok", True)
    slot["value"] = msg.get("value")
    slot["error"] = msg.get("error")
    if "event" in slot:
        slot["event"].set()
        return
    callback = slot.get("callback")
    if callback is None:
        return
    value, error = (slot["value"], None) if slot["ok"] else (None, slot["error"])
    _schedule(slot["queue"], lambda: run_as(slot["plugin"], callback, value, error))


def plugin_id():
    return getattr(current_plugin, "id", None)


def run_as(pid, fn, *args):
    previous = plugin_id()
    current_plugin.id = pid
    try:
        return fn(*args)
    except Exception:
        log(pid, "error", "Unhandled exception in plugin callback:\n" + traceback.format_exc())
    finally:
        current_plugin.id = previous


def register_callback(fn):
    token = f"cb{next(_callback_ids)}"
    _callbacks[token] = fn
    _callback_owner[token] = plugin_id()
    return token


def get_callback(token):
    return _callbacks.get(token)


def drop_callbacks(pid):
    for token in [t for t, owner in _callback_owner.items() if owner == pid]:
        _callbacks.pop(token, None)
        _callback_owner.pop(token, None)


def schedule(queue, fn, delay_ms=0):
    pid = plugin_id()
    _schedule(queue, lambda: run_as(pid, fn), delay_ms)


def log(pid, level, message):
    send({"t": "log", "plugin": pid, "level": level, "msg": str(message)})

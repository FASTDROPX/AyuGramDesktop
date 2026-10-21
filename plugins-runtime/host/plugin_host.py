"""AyuGram Desktop plugin host (exteraGram plugin SDK).

Runs in its own process next to the app. Reads one JSON command per line from
stdin and answers on stdout; plugin output (print, tracebacks) is turned into
log messages so it can never corrupt the protocol.

App -> host:  {"t": "call", "id": n, "op": ..., ...}   answered with {"t": "ret", "id": n, ...}
              {"t": "res", "id": n, ...}                answer to a host request
Host -> app:  {"t": "req", "id": n, "fn": ...}          request (td, send_message, ...)
              {"t": "log" | "setting" | "hook_add" | "menu_add" | "bulletin" | "alert" | ...}
"""

import ast
import base64
import importlib.machinery
import importlib.metadata
import importlib.util
import io
import os
import queue
import subprocess
import sys
import threading
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
SDK_DIR = os.path.normpath(os.path.join(HERE, "..", "sdk"))
sys.path.insert(0, SDK_DIR)

import _bridge  # noqa: E402
import file_utils  # noqa: E402
from base_plugin import AppEvent, BasePlugin, HookResult, HookStrategy, valid_plugin_id  # noqa: E402
import tl  # noqa: E402
from td_object import TdObject, unwrap  # noqa: E402

SDK_VERSION = "1.4.4.3"
META_KEYS = ("__id__", "__name__", "__description__", "__author__", "__version__", "__icon__",
             "__app_version__", "__sdk_version__", "__requirements__", "__min_version__")

plugins = {}          # id -> LoadedPlugin
_queues = {}
_queues_lock = threading.Lock()
_packages_dir = None


class LoadedPlugin:
    def __init__(self, pid, path, module, instance):
        self.id = pid
        self.path = path
        self.module = module
        self.instance = instance


class _LogStream(io.TextIOBase):
    """Replaces sys.stdout/sys.stderr: plugin prints become log lines."""

    def __init__(self, level):
        self.level = level
        self.buffer_text = ""

    def writable(self):
        return True

    def write(self, text):
        self.buffer_text += text
        while "\n" in self.buffer_text:
            line, self.buffer_text = self.buffer_text.split("\n", 1)
            if line.strip():
                _bridge.log(_bridge.plugin_id(), self.level, line)
        return len(text)


# ---- queues ----------------------------------------------------------------------

def _worker(q):
    while True:
        fn = q.get()
        try:
            fn()
        except Exception:
            _bridge.log(None, "error", traceback.format_exc())


def schedule(name, fn, delay_ms=0):
    with _queues_lock:
        q = _queues.get(name)
        if q is None:
            q = _queues[name] = queue.Queue()
            threading.Thread(target=_worker, args=(q,), name=f"queue-{name}", daemon=True).start()
    if delay_ms and delay_ms > 0:
        timer = threading.Timer(delay_ms / 1000.0, q.put, args=(fn,))
        timer.daemon = True
        timer.start()
    else:
        q.put(fn)


# ---- metadata ----------------------------------------------------------------------

def read_metadata(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        source = f.read()
    tree = ast.parse(source, filename=path)
    meta = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            key = node.targets[0].id
            if key in META_KEYS:
                try:
                    meta[key.strip("_")] = ast.literal_eval(node.value)
                except ValueError:
                    pass
    if "min_version" in meta and "app_version" not in meta:
        meta["app_version"] = ">=" + str(meta["min_version"])
    meta.setdefault("version", "1.0")
    if not valid_plugin_id(meta.get("id")):
        raise ValueError("__id__ must be 2-32 chars: Latin letters, digits, '_' or '-', starting with a letter")
    if not meta.get("name"):
        raise ValueError("__name__ is required")
    return meta


# ---- requirements --------------------------------------------------------------

def _missing_requirements(requirements):
    missing = []
    for req in requirements or []:
        name = req.split(";")[0]
        for sep in ("==", ">=", "<=", "~=", "!=", ">", "<", "["):
            name = name.split(sep)[0]
        try:
            importlib.metadata.version(name.strip())
        except importlib.metadata.PackageNotFoundError:
            missing.append(req)
    return missing


def install_requirements(pid, requirements):
    missing = _missing_requirements(requirements)
    if not missing:
        return
    _bridge.log(pid, "info", "Installing requirements: " + ", ".join(missing))
    os.makedirs(_packages_dir, exist_ok=True)
    proc = subprocess.run([sys.executable, "-m", "pip", "install", "--disable-pip-version-check",
                           "--target", _packages_dir, *missing],
                          capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    if proc.returncode != 0:
        raise RuntimeError("pip install failed:\n" + (proc.stderr or proc.stdout)[-2000:])
    importlib.invalidate_caches()


# ---- operations ----------------------------------------------------------------

def op_init(msg):
    global _packages_dir
    if msg.get("schema"):
        os.environ["AYU_TL_SCHEMA"] = msg["schema"]
    file_utils._set_dirs(msg.get("dirs") or {})
    _packages_dir = msg.get("packages_dir") or os.path.join(file_utils.get_files_dir(), "python-packages")
    if _packages_dir not in sys.path:
        sys.path.insert(1, _packages_dir)
    return {"sdk_version": SDK_VERSION, "python": sys.version.split()[0], "layer": tl.schema().layer}


def op_scan(msg):
    return read_metadata(msg["path"])


def op_enable(msg):
    path = msg["path"]
    meta = read_metadata(path)
    pid = meta["id"]
    if pid in plugins:
        op_disable({"plugin": pid})
    _bridge.current_plugin.id = pid
    install_requirements(pid, meta.get("requirements"))
    module_name = f"exteragram_plugin_{pid}"
    loader = importlib.machinery.SourceFileLoader(module_name, path)
    spec = importlib.util.spec_from_loader(module_name, loader, origin=path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    loader.exec_module(module)
    classes = [obj for obj in vars(module).values()
               if isinstance(obj, type) and issubclass(obj, BasePlugin) and obj is not BasePlugin]
    if not classes:
        raise RuntimeError("the plugin defines no class derived from BasePlugin")
    instance = classes[-1]()
    instance._attach(pid, meta["name"], msg.get("settings"))
    plugins[pid] = LoadedPlugin(pid, path, module, instance)
    instance.on_plugin_load()
    meta["has_settings"] = type(instance).create_settings is not BasePlugin.create_settings
    return meta


def op_disable(msg):
    pid = msg["plugin"]
    loaded = plugins.pop(pid, None)
    if loaded is None:
        return None
    _bridge.current_plugin.id = pid
    try:
        loaded.instance.on_plugin_unload()
    finally:
        _bridge.drop_callbacks(pid)
        sys.modules.pop(f"exteragram_plugin_{pid}", None)
    return None


def _serialize_settings(items):
    result = []
    for item in items or []:
        to_dict = getattr(item, "to_dict", None)
        result.append(to_dict() if callable(to_dict) else {"kind": "unsupported", "text": repr(item)})
    return result


def op_settings(msg):
    loaded = plugins[msg["plugin"]]
    _bridge.current_plugin.id = loaded.id
    return {"items": _serialize_settings(loaded.instance.create_settings()),
            "values": loaded.instance.export_settings()}


def op_setting_changed(msg):
    loaded = plugins.get(msg["plugin"])
    if loaded:
        loaded.instance._settings[msg["key"]] = msg["value"]
    return None


def op_callback(msg):
    fn = _bridge.get_callback(msg["token"])
    if fn is None:
        raise KeyError("callback is gone (plugin was reloaded)")
    args = msg.get("args") or []
    if msg.get("wrap"):
        args = [_context(a) for a in args]
    value = fn(*args)
    if msg.get("returns") == "settings":
        return {"items": _serialize_settings(value)}
    return None


def _context(value):
    """Menu click context: a dict like on Android, with "message" as a MessageObject."""
    if not isinstance(value, dict):
        return value
    import client_utils
    ctx = dict(value)
    if isinstance(ctx.get("message"), dict):
        ctx["message"] = client_utils.message_from_context(ctx["message"])
    return ctx


def _result(result, original):
    if result is None:
        return {"strategy": "DEFAULT"}
    strategy = result.strategy.value if isinstance(result.strategy, HookStrategy) else str(result.strategy)
    obj = None
    if strategy in ("MODIFY", "MODIFY_FINAL"):
        for candidate in (result.request, result.response, result.update, result.updates, result.params):
            if candidate is not None:
                obj = candidate
                break
        obj = obj if obj is not None else original
        if isinstance(obj, tl.TLObject):
            return {"strategy": strategy, "tl": base64.b64encode(tl.encode(obj)).decode("ascii")}
        obj = unwrap(obj)
    return {"strategy": strategy, "obj": obj}


def _decode_tl(value):
    return tl.decode(base64.b64decode(value)) if value else None


def op_hook(msg):
    loaded = plugins.get(msg["plugin"])
    if loaded is None:
        return {"strategy": "DEFAULT"}
    inst = loaded.instance
    kind, account = msg["kind"], msg.get("account", 0)
    cid = msg.get("cid", 0)
    tl_name = tl.name_of(cid) if cid else msg.get("name", "")
    shown = inst._hook_name_for(cid, tl_name)
    if kind == "pre_request":
        obj = _decode_tl(msg.get("tl"))
        result = inst.pre_request_hook(shown, account, obj)
    elif kind == "post_request":
        obj = _decode_tl(msg.get("tl"))
        error = msg.get("error")
        err = tl.TL_error(int(error.get("code", 400)), str(error.get("text", ""))) if error else None
        result = inst.post_request_hook(shown, account, obj, err)
    elif kind == "update":
        obj = _decode_tl(msg.get("tl"))
        result = inst.on_update_hook(shown, account, obj)
    elif kind == "updates":
        obj = _decode_tl(msg.get("tl"))
        result = inst.on_updates_hook(shown, account, obj)
    elif kind == "send_message":
        obj = TdObject(msg.get("obj") or {})
        result = inst.on_send_message_hook(account, obj)
    else:
        raise ValueError(f"unknown hook kind {kind}")
    return _result(result, obj)


def op_app_event(msg):
    event = AppEvent(msg["event"])
    for loaded in list(plugins.values()):
        _bridge.run_as(loaded.id, loaded.instance.on_app_event, event)
    return None


OPS = {
    "init": op_init, "scan": op_scan, "enable": op_enable, "disable": op_disable,
    "settings": op_settings, "setting_changed": op_setting_changed, "callback": op_callback,
    "hook": op_hook, "app_event": op_app_event,
}


def handle_call(msg):
    pid = msg.get("plugin")
    _bridge.current_plugin.id = pid
    try:
        value = OPS[msg["op"]](msg)
        _bridge.send({"t": "ret", "id": msg["id"], "ok": True, "value": value})
    except Exception as exc:
        tb = traceback.format_exc()
        if msg["op"] not in ("scan",):
            _bridge.log(_bridge.plugin_id(), "error", tb)
        _bridge.send({"t": "ret", "id": msg["id"], "ok": False, "error": f"{type(exc).__name__}: {exc}",
                      "trace": tb})
    finally:
        _bridge.current_plugin.id = None


def main():
    stdin = io.TextIOWrapper(sys.stdin.buffer, encoding="utf-8")
    stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", newline="\n", write_through=True)
    sys.stdout = _LogStream("info")
    sys.stderr = _LogStream("error")
    _bridge.attach(stdout, schedule)
    _bridge.send({"t": "hello", "sdk_version": SDK_VERSION, "python": sys.version.split()[0]})
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = __import__("json").loads(line)
        except ValueError:
            _bridge.log(None, "error", f"bad command: {line[:200]}")
            continue
        t = msg.get("t")
        if t == "res":
            _bridge.deliver(msg)
        elif t == "call":
            # Hooks and lifecycle calls run on one queue, in order; plugin
            # background work (run_on_queue) has its own queues.
            schedule("hooks", lambda m=msg: handle_call(m))
        elif t == "exit":
            break
    for loaded in list(plugins.values()):
        try:
            _bridge.run_as(loaded.id, loaded.instance.on_plugin_unload)
        except Exception:
            pass


if __name__ == "__main__":
    main()

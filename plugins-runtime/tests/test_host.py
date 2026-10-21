"""Drives plugin_host.py the way AyuGram Desktop does (TL payloads) and checks the answers.

Run: python plugins-runtime/tests/test_host.py
"""

import base64
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "host", "plugin_host.py")
EXAMPLES = os.path.join(ROOT, "examples")
SCHEMA = os.path.normpath(os.path.join(ROOT, "..", "Telegram", "SourceFiles", "mtproto", "scheme", "api.tl"))

sys.path.insert(0, os.path.join(ROOT, "sdk"))
import tl  # noqa: E402

with open(SCHEMA, encoding="utf-8") as _f:
    tl.set_schema_text(_f.read())
TLRPC = tl.TLRPC


def b64(obj):
    return base64.b64encode(tl.encode(obj)).decode("ascii")


def unb64(text):
    return tl.decode(base64.b64decode(text))


class FakeClient:
    """Plays AyuGram's side: answers plugin requests with TL bytes."""

    def __init__(self):
        self.proc = subprocess.Popen([sys.executable, "-u", HOST], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.next_id = 1
        self.replies = {}
        self.events = queue.Queue()
        self.sent_requests = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            msg = json.loads(raw.decode("utf-8"))
            if msg["t"] == "ret":
                self.replies[msg["id"]].put(msg)
            elif msg["t"] == "req":
                self.events.put(msg)
                self.write(self._answer(msg))
            else:
                self.events.put(msg)

    def _answer(self, msg):
        rid = msg["id"]
        if msg["fn"] == "tl":
            request = unb64(msg["payload"])
            self.sent_requests.append(request)
            if isinstance(request, TLRPC.TL_users_getUsers):
                user = TLRPC.TL_user(id=42, first_name="Ayu", self=True)
                vec = tl.Writer()
                vec.uint(tl.VECTOR_ID)
                vec.int(1)
                vec.raw(tl.encode(user))
                return {"t": "res", "id": rid, "ok": True, "value": base64.b64encode(vec.data()).decode()}
            if isinstance(request, TLRPC.TL_messages_getHistory):
                return {"t": "res", "id": rid, "ok": False, "error": {"code": 400, "text": "PEER_ID_INVALID"}}
            return {"t": "res", "id": rid, "ok": True, "value": b64(TLRPC.TL_boolTrue())}
        if msg["fn"] == "my_id":
            return {"t": "res", "id": rid, "ok": True, "value": 42}
        if msg["fn"] == "peer_info":
            return {"t": "res", "id": rid, "ok": True,
                    "value": {"_": "user", "id": msg["dialog_id"], "first_name": "Bob", "username": "bob"}}
        return {"t": "res", "id": rid, "ok": True, "value": True}

    def write(self, msg):
        self.proc.stdin.write((json.dumps(msg) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def call(self, op, **fields):
        rid = self.next_id
        self.next_id += 1
        self.replies[rid] = queue.Queue()
        self.write({"t": "call", "id": rid, "op": op, **fields})
        reply = self.replies[rid].get(timeout=30)
        if not reply["ok"]:
            raise AssertionError(f"{op} failed: {reply['error']}\n{reply.get('trace', '')}")
        return reply["value"]

    def hook(self, plugin, kind, obj=None, **extra):
        cid = type(obj)._comb.id if isinstance(obj, tl.TLObject) else extra.pop("cid", 0)
        fields = {"plugin": plugin, "kind": kind, "cid": cid, "account": 0, **extra}
        if isinstance(obj, tl.TLObject):
            fields["tl"] = b64(obj)
        elif obj is not None:
            fields["obj"] = obj
        return self.call("hook", **fields)

    def drain(self, wait=0.4):
        out = []
        while True:
            try:
                out.append(self.events.get(timeout=wait))
            except queue.Empty:
                return out

    def close(self):
        self.write({"t": "exit"})
        self.proc.wait(timeout=10)


failed = 0


def check(cond, label):
    global failed
    print(("PASS " if cond else "FAIL ") + label)
    if not cond:
        failed += 1


def write_plugin(folder, name, source):
    path = os.path.join(folder, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(source)
    return path


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    app = FakeClient()
    data = tempfile.mkdtemp()
    info = app.call("init", dirs={"files": data, "plugins": data}, schema=SCHEMA)
    check(info["layer"] == tl.schema().layer, f"init: SDK {info['sdk_version']}, Python {info['python']}, TL layer {info['layer']}")

    ghost = os.path.join(EXAMPLES, "ghost_mode.plugin")
    app.call("enable", path=ghost, settings={})
    events = app.drain()
    hooks = {e["name"]: e["ids"] for e in events if e["t"] == "hook_add"}
    set_typing = tl.schema().find("messages.setTyping").id
    check(hooks.get("TL_messages_setTyping") == [set_typing] and "TL_channels_readHistory" in hooks,
          f"hooks registered by TL constructor ids: {sorted(hooks)}")

    typing = TLRPC.TL_messages_setTyping(peer=TLRPC.TL_inputPeerSelf(), action=TLRPC.TL_sendMessageTypingAction())
    check(app.hook("ghost_mode", "pre_request", typing)["strategy"] == "CANCEL", "Android-style ghost mode cancels messages.setTyping")
    read = TLRPC.TL_channels_readHistory(channel=TLRPC.TL_inputChannel(channel_id=5, access_hash=1), max_id=100)
    check(app.hook("ghost_mode", "pre_request", read)["strategy"] == "CANCEL", "…and channels.readHistory")

    # A plugin that rewrites requests and updates, reads TL fields like on Android.
    rewrite = write_plugin(data, "rewrite.plugin", '''
from base_plugin import BasePlugin, HookResult, HookStrategy, MenuItemData, MenuItemType
from org.telegram.tgnet import TLRPC
from org.telegram.messenger import R
from client_utils import send_request, get_messages_controller
__id__ = "rewrite"
__name__ = "Rewrite"
class P(BasePlugin):
    def on_plugin_load(self):
        self.add_hook("TL_messages_sendMessage")
        self.add_hook("TL_updateNewMessage")
        self.add_hook("TL_messages_get", match_substring=True)
        self.add_menu_item(MenuItemData(menu_type=MenuItemType.MESSAGE_CONTEXT_MENU, text="Info", on_click=self.info))
        req = TLRPC.TL_users_getUsers()
        req.id = [TLRPC.TL_inputUserSelf()]
        send_request(req, self.on_users)
        send_request(TLRPC.TL_messages_getHistory(peer=TLRPC.TL_inputPeerSelf()), self.on_history)
        self.logger.info("bob=%s", get_messages_controller().getUser(7).username)
        self.logger.info("R=%s", R.drawable.msg_info)
    def on_users(self, response, error):
        self.logger.info("users=%s %s", type(response).__name__, response[0].first_name)
    def on_history(self, response, error):
        self.logger.info("history error=%s %s", error.code, error.text)
    def info(self, context):
        m = context["message"]
        self.logger.info("menu=%s|%s|%s", m.messageOwner.message, m.getDialogId(), m.messageOwner.peer_id.channel_id)
    def pre_request_hook(self, name, account, request):
        if isinstance(request, TLRPC.TL_messages_sendMessage):
            request.message = request.message.upper()
            return HookResult(strategy=HookStrategy.MODIFY, request=request)
        self.logger.info("seen=%s", name)
        return HookResult()
    def on_update_hook(self, name, account, update):
        if update.message.message == "spam":
            return HookResult(strategy=HookStrategy.CANCEL)
        update.message.message += " ✓"
        return HookResult(strategy=HookStrategy.MODIFY, update=update)
''')
    app.call("enable", path=rewrite, settings={})
    events = app.drain(0.8)
    logs = [e["msg"] for e in events if e["t"] == "log"]
    check("users=list Ayu" in logs, f"plugin's TLRPC request gets a decoded TL answer: {logs}")
    check("history error=400 PEER_ID_INVALID" in logs, "RPC errors arrive as TL_error(code, text)")
    check("bob=bob" in logs and "R=0" in logs, "getUser via peer_info; org.telegram.messenger.R stub")
    substring_ids = next(e["ids"] for e in events if e["t"] == "hook_add" and e["name"] == "TL_messages_get")
    check(len(substring_ids) > 20 and tl.schema().find("messages.getHistory").id in substring_ids,
          f"match_substring hook covers {len(substring_ids)} constructors")

    send = TLRPC.TL_messages_sendMessage(peer=TLRPC.TL_inputPeerSelf(), message="hello", random_id=1)
    r = app.hook("rewrite", "pre_request", send)
    check(r["strategy"] == "MODIFY" and unb64(r["tl"]).message == "HELLO", "request modified, sent back as TL bytes")
    r = app.hook("rewrite", "pre_request", TLRPC.TL_messages_getHistory(peer=TLRPC.TL_inputPeerSelf()))
    events = app.drain()
    check(r["strategy"] == "DEFAULT" and any(e.get("msg") == "seen=TL_messages_getHistory" for e in events),
          "substring hook receives the real request name")

    msg = TLRPC.TL_message(id=1, peer_id=TLRPC.TL_peerUser(user_id=3), date=1, message="hi")
    r = app.hook("rewrite", "update", TLRPC.TL_updateNewMessage(message=msg, pts=1, pts_count=1))
    check(r["strategy"] == "MODIFY" and unb64(r["tl"]).message.message == "hi ✓", "update modified")
    msg.message = "spam"
    r = app.hook("rewrite", "update", TLRPC.TL_updateNewMessage(message=msg, pts=1, pts_count=1))
    check(r["strategy"] == "CANCEL", "update cancelled")

    app.call("disable", plugin="rewrite")
    app.call("enable", path=rewrite, settings={})
    events = app.drain(0.8)
    menu = next(e for e in events if e["t"] == "menu_add")
    app.call("callback", token=menu["item"]["token"], wrap=True, plugin="rewrite", args=[{
        "account": 0, "dialog_id": -1001,
        "message": {"id": 9, "dialog_id": -1001, "peer_type": "channel", "text": "hello menu", "date": 5, "out": False}}])
    logs = [e["msg"] for e in app.drain() if e["t"] == "log"]
    check("menu=hello menu|-1001|1001" in logs, f"menu context: message.messageOwner like Android ({logs})")

    replace = os.path.join(EXAMPLES, "text_replace.plugin")
    app.call("enable", path=replace, settings={})
    app.drain()
    r = app.hook("text_replace", "send_message", {"peer": 1, "message": "hi .shrug"})
    check(r["strategy"] == "MODIFY" and r["obj"]["message"] == "hi ¯\\_(ツ)_/¯", "on_send_message_hook")

    crash = write_plugin(data, "crash.plugin", 'from base_plugin import BasePlugin\n__id__ = "crash"\n__name__ = "Crash"\n'
                                               'class P(BasePlugin):\n    def on_plugin_load(self):\n        raise ValueError("boom")\n')
    try:
        app.call("enable", path=crash, settings={})
        check(False, "crashing plugin reports an error")
    except AssertionError as e:
        check("boom" in str(e), "crashing plugin reports its error, host keeps running")
    check(app.hook("ghost_mode", "pre_request", typing)["strategy"] == "CANCEL", "other plugins still work")

    app.close()
    print("\nALL PASS" if not failed else f"\n{failed} FAILED")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

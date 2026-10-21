"""TL codec checks against the client's api.tl.

Run: python plugins-runtime/tests/test_tl.py [path/to/api.tl]
"""

import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk"))

import tl  # noqa: E402

failed = 0


def check(cond, label):
    global failed
    print(("PASS " if cond else "FAIL ") + label)
    if not cond:
        failed += 1


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    default = os.path.join(ROOT, "..", "Telegram", "SourceFiles", "mtproto", "scheme", "api.tl")
    path = sys.argv[1] if len(sys.argv) > 1 else default
    with open(path, encoding="utf-8") as f:
        tl.set_schema_text(f.read())
    s = tl.schema()
    check(s.layer > 200 and len(s.by_id) > 1500, f"schema: layer {s.layer}, {len(s.by_id)} constructors")

    TLRPC = tl.TLRPC
    req = TLRPC.TL_messages_setTyping()
    req.peer = TLRPC.TL_inputPeerSelf()
    req.action = TLRPC.TL_sendMessageTypingAction()
    data = tl.encode(req)
    expected = struct.pack("<IIII", 0x58943EE2, 0, 0x7DA07EC9, 0x16BF744E)
    check(data == expected, f"messages.setTyping bytes {data.hex()}")
    back = tl.decode(data)
    check(isinstance(back, TLRPC.TL_messages_setTyping) and isinstance(back.peer, TLRPC.TL_inputPeerSelf),
          "decode gives TLRPC classes (isinstance works)")
    check(back.getClass().getSimpleName() == "TL_messages_setTyping", "getClass().getSimpleName() like Java")
    check(back.top_msg_id is None and back.flags == 0, "absent optional field reads as None, flags readable")

    req.top_msg_id = 777
    data = tl.encode(req)
    check(struct.unpack("<I", data[4:8])[0] == 1 and tl.decode(data).top_msg_id == 777, "flags computed from set fields")

    # strings: short, long (> 253 bytes), unicode
    for text in ["", "hi", "привет 👋", "x" * 300]:
        m = TLRPC.TL_messages_sendMessage()
        m.peer = TLRPC.TL_inputPeerUser(user_id=123456789012, access_hash=-5)
        m.message = text
        m.random_id = 2 ** 62 + 5
        m.silent = True
        out = tl.decode(tl.encode(m))
        check(out.message == text and out.silent is True and out.random_id == 2 ** 62 + 5
              and out.peer.user_id == 123456789012 and out.peer.access_hash == -5,
              f"sendMessage round trip, message of {len(text.encode())} bytes")
    check(len(tl.encode(m)) % 4 == 0, "serialized size is 4-byte aligned")

    # message# with flags2, nested Peer, entities vector
    msg = TLRPC.TL_message()
    msg.id = 42
    msg.peer_id = TLRPC.TL_peerChannel(channel_id=1001)
    msg.from_id = TLRPC.TL_peerUser(user_id=7)
    msg.date = 1_700_000_000
    msg.message = "**hello** world"
    msg.out = True
    msg.offline = True  # a flags2 bit
    msg.entities = [TLRPC.TL_messageEntityBold(offset=0, length=9)]
    upd = TLRPC.TL_updateNewMessage(message=msg, pts=10, pts_count=1)
    got = tl.decode(tl.encode(upd))
    check(isinstance(got, TLRPC.TL_updateNewMessage) and got.message.message == "**hello** world"
          and got.message.out and got.message.offline and got.message.entities[0].length == 9
          and got.message.peer_id.channel_id == 1001, "updateNewMessage with flags2 + vector round trip")

    # Vector<long> response without item constructors
    raw = struct.pack("<Ii", tl.VECTOR_ID, 2) + struct.pack("<qq", 5, -6)
    check(tl.decode_vector_of(raw, "long") == [5, -6], "Vector<long> response")
    check(tl.decode(struct.pack("<I", tl.BOOL_TRUE)) is True, "Bool response")

    # Every constructor encodes/decodes with defaults filled for required numbers/strings.
    bad = []
    for comb in s.by_id.values():
        obj = tl.class_for(comb)()
        try:
            for p in comb.params:
                if p.is_flags or p.flag_field:
                    continue
                if p.type in ("int", "long", "double"):
                    setattr(obj, p.name, 1)
                elif p.type in ("string",):
                    setattr(obj, p.name, "s")
                elif p.type == "bytes":
                    setattr(obj, p.name, b"b")
                elif p.type in ("int128", "int256"):
                    setattr(obj, p.name, b"\1")
                elif p.type == "Bool":
                    setattr(obj, p.name, True)
                elif p.type.lower().startswith("vector<"):
                    setattr(obj, p.name, [])
                else:
                    raise LookupError("needs nested object")
        except LookupError:
            continue
        if comb.result == "Bool":
            continue  # boolTrue/boolFalse decode to Python True/False
        try:
            once = tl.encode(obj)
            if tl.encode(tl.decode(once)) != once:
                bad.append(comb.class_name)
        except Exception as exc:  # noqa: BLE001
            bad.append(f"{comb.class_name}: {exc}")
    check(not bad, f"flat constructors: bytes survive decode+encode ({len(bad)} bad) {bad[:3]}")

    check(0x58943EE2 in s.ids_matching("TL_messages_setTyping"), "substring hook ids")
    check(tl.name_of(0x1F2B0AFD) == "TL_updateNewMessage", "name_of(constructor)")

    print("\nALL PASS" if not failed else f"\n{failed} FAILED")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

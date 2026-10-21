"""HTML parse mode and entity vectors.

Run: python plugins-runtime/tests/test_markup.py
"""

import os
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
    path = os.path.join(ROOT, "..", "Telegram", "SourceFiles", "mtproto", "scheme", "api.tl")
    with open(path, encoding="utf-8") as f:
        tl.set_schema_text(f.read())
    import _markup

    text, ents = _markup.parse_html("<b>Hi</b> 👋 <a href='https://x.org'>link</a> &lt;3")
    check(text == "Hi 👋 link <3", "plain text and entities unescaped")
    names = [e.getClass().getSimpleName() for e in ents]
    check(names == ["TL_messageEntityBold", "TL_messageEntityTextUrl"], f"entity types {names}")
    check((ents[0].offset, ents[0].length) == (0, 2), "bold offset")
    # "Hi " = 3, the emoji is 2 UTF-16 units, then a space: link starts at 6.
    check((ents[1].offset, ents[1].length, ents[1].url) == (6, 4, "https://x.org"), "UTF-16 offsets for the link")

    text, ents = _markup.parse_html('<pre><code class="language-py">x = 1</code></pre>')
    check(text == "x = 1" and len(ents) == 1, "pre with code")
    check(ents[0].getClass().getSimpleName() == "TL_messageEntityPre" and ents[0].language == "py", "pre language")

    text, ents = _markup.parse_html("<blockquote expandable>q</blockquote><tg-spoiler>s</tg-spoiler>")
    check(ents[0].collapsed is True and ents[1].getClass().getSimpleName() == "TL_messageEntitySpoiler",
          "expandable quote and spoiler")

    data = tl.encode_vector_of(ents, "MessageEntity")
    back = tl.decode_vector_of(data, "MessageEntity")
    check([tl.encode(e) for e in back] == [tl.encode(e) for e in ents], "entity vector round trip")
    check(data[:4] == bytes.fromhex("15c4b51c"), "boxed vector id")

    request = tl.TLRPC.TL_messages_editMessage(
        peer=tl.TLRPC.TL_inputPeerSelf(), id=5, message="x", entities=ents)
    again = tl.decode(tl.encode(request))
    check(again.message == "x" and len(again.entities) == 2 and again.id == 5, "editMessage with entities")

    print("ALL PASS" if not failed else f"{failed} FAILED")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

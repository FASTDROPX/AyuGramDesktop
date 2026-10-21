"""HTML parse mode for texts sent or edited by plugins.

Turns Telegram-style HTML (the tags the Bot API accepts) into plain text plus
TLRPC.MessageEntity objects. Offsets and lengths are in UTF-16 code units, as
Telegram counts them.
"""

from html.parser import HTMLParser

from tl import TLRPC


def _u16(text):
    return len(text.encode("utf-16-le")) // 2


class _Parser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.length = 0
        self.stack = []  # (tag, start, attrs)
        self.entities = []

    def handle_starttag(self, tag, attrs):
        if tag == "br":
            self._append("\n")
            return
        self.stack.append((tag, self.length, dict(attrs)))

    def handle_startendtag(self, tag, attrs):
        if tag == "br":
            self._append("\n")

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                _, start, attrs = self.stack.pop(i)
                self._entity(tag, start, attrs)
                return

    def handle_data(self, data):
        self._append(data)

    def _append(self, text):
        self.parts.append(text)
        self.length += _u16(text)

    def _entity(self, tag, start, attrs):
        length = self.length - start
        if length <= 0:
            return
        cls, extra = None, {}
        if tag in ("b", "strong"):
            cls = TLRPC.TL_messageEntityBold
        elif tag in ("i", "em"):
            cls = TLRPC.TL_messageEntityItalic
        elif tag in ("u", "ins"):
            cls = TLRPC.TL_messageEntityUnderline
        elif tag in ("s", "strike", "del"):
            cls = TLRPC.TL_messageEntityStrike
        elif tag in ("tg-spoiler", "spoiler") or (tag == "span" and attrs.get("class") == "tg-spoiler"):
            cls = TLRPC.TL_messageEntitySpoiler
        elif tag == "code":
            pre = next((a for t, _, a in reversed(self.stack) if t == "pre"), None)
            if pre is not None:
                language = attrs.get("class") or ""
                if language.startswith("language-"):
                    pre.setdefault("language", language[len("language-"):])
                return
            cls = TLRPC.TL_messageEntityCode
        elif tag == "pre":
            cls = TLRPC.TL_messageEntityPre
            extra["language"] = attrs.get("language") or ""
        elif tag == "a":
            cls = TLRPC.TL_messageEntityTextUrl
            extra["url"] = attrs.get("href") or ""
        elif tag == "blockquote":
            cls = TLRPC.TL_messageEntityBlockquote
            if "expandable" in attrs:
                extra["collapsed"] = True
        elif tag == "tg-emoji":
            emoji_id = attrs.get("emoji-id")
            if emoji_id and emoji_id.isdigit():
                cls = TLRPC.TL_messageEntityCustomEmoji
                extra["document_id"] = int(emoji_id)
        if cls is not None:
            self.entities.append(cls(offset=start, length=length, **extra))


def parse_html(text):
    """Returns (plain_text, [TLRPC.MessageEntity])."""
    parser = _Parser()
    parser.feed(text or "")
    parser.close()
    entities = sorted(parser.entities, key=lambda e: (e.offset, -e.length))
    return "".join(parser.parts), entities

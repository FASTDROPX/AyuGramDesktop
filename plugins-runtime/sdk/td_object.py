"""TDLib objects for plugins.

exteraGram Desktop speaks TDLib, so requests, responses and updates are TDLib
JSON objects (`{"@type": "sendMessage", "chat_id": ...}`). `TdObject` wraps
such a dict so plugin code can read and change it the way Android plugins
change TLRPC objects:

    request.chat_id            # snake_case key
    request.chatId             # camelCase works too
    request.type               # the "@type" field
    request.input_message_content.text.text = "hello"

Changes go straight into the underlying dict, so a hook can modify the object
it was given and return it.
"""

import re

_camel = re.compile(r"(?<!^)(?=[A-Z])")


def _key(name):
    if name in ("type", "_type", "TYPE"):
        return "@type"
    if name == "extra":
        return "@extra"
    return _camel.sub("_", name).lower()


def wrap(value):
    if isinstance(value, dict):
        return TdObject(value)
    if isinstance(value, list):
        return TdList(value)
    return value


def unwrap(value):
    if isinstance(value, TdObject):
        return value._d
    if isinstance(value, TdList):
        return value._l
    if isinstance(value, (list, tuple)):
        return [unwrap(v) for v in value]
    if isinstance(value, dict):
        return {k: unwrap(v) for k, v in value.items()}
    return value


class TdObject:
    __slots__ = ("_d",)

    def __init__(self, data=None, **fields):
        if isinstance(data, str):
            data = {"@type": data}
        elif isinstance(data, TdObject):
            data = data._d
        object.__setattr__(self, "_d", data if data is not None else {})
        for k, v in fields.items():
            self._d[_key(k)] = unwrap(v)

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        d = object.__getattribute__(self, "_d")
        key = name if name in d else _key(name)
        if key not in d:
            return None
        return wrap(d[key])

    def __setattr__(self, name, value):
        key = name if name in self._d else _key(name)
        self._d[key] = unwrap(value)

    def __getitem__(self, key):
        return wrap(self._d[key])

    def __setitem__(self, key, value):
        self._d[key] = unwrap(value)

    def __contains__(self, key):
        return key in self._d or _key(key) in self._d

    def __eq__(self, other):
        return unwrap(other) == self._d

    def __repr__(self):
        return f"TdObject({self._d!r})"

    def get(self, key, default=None):
        return wrap(self._d.get(key, default))

    def keys(self):
        return self._d.keys()

    def to_dict(self):
        return self._d

    def getClass(self):
        # Android plugins sometimes call request.getClass().getSimpleName().
        name = self._d.get("@type", "")
        return type("TdClass", (), {"getSimpleName": staticmethod(lambda: name),
                                    "getName": staticmethod(lambda: name)})()


class TdList:
    __slots__ = ("_l",)

    def __init__(self, items):
        self._l = items

    def __getitem__(self, i):
        if isinstance(i, slice):
            return [wrap(v) for v in self._l[i]]
        return wrap(self._l[i])

    def __setitem__(self, i, value):
        self._l[i] = unwrap(value)

    def __len__(self):
        return len(self._l)

    def __iter__(self):
        return (wrap(v) for v in self._l)

    def append(self, value):
        self._l.append(unwrap(value))

    def remove(self, value):
        self._l.remove(unwrap(value))

    def clear(self):
        self._l.clear()

    def size(self):
        return len(self._l)

    def get(self, i):
        return wrap(self._l[i])

    def add(self, value):
        self._l.append(unwrap(value))

    def to_dict(self):
        return self._l

    def __repr__(self):
        return f"TdList({self._l!r})"


def td(type_name, **fields):
    """Build a TDLib object: td("sendChatAction", chat_id=1, action=td("chatActionCancel"))."""
    return TdObject(type_name, **fields)

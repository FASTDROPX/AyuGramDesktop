"""Telegram TL (MTProto API) objects for plugins, built from the client's own api.tl.

AyuGram Desktop speaks raw MTProto, like Telegram for Android, so plugins get the
same objects Android plugins get: `TLRPC.TL_messages_setTyping`,
`TLRPC.TL_updateNewMessage`, ... with the schema's field names
(`request.peer`, `update.message.message`, `request.top_msg_id`).

    from org.telegram.tgnet import TLRPC
    req = TLRPC.TL_messages_setTyping()
    req.peer = TLRPC.TL_inputPeerSelf()
    req.action = TLRPC.TL_sendMessageCancelAction()
    data = encode(req)                       # bytes, ready for the client
    obj = decode(data)                       # -> TL_messages_setTyping

`flags` fields are computed on encoding from which optional fields are set,
so plugins don't have to manage flag bits (they still can read `obj.flags`).
"""

import os
import re
import struct

VECTOR_ID = 0x1CB5C415
BOOL_TRUE = 0x997275B5
BOOL_FALSE = 0xBC799737

_line_re = re.compile(r"^([\w.]+)#([0-9a-fA-F]+)\s+(.*?)=\s*([\w.<>%]+)\s*;\s*$")


class Param:
    __slots__ = ("name", "type", "flag_field", "flag_bit")

    def __init__(self, name, type_, flag_field=None, flag_bit=None):
        self.name = name
        self.type = type_
        self.flag_field = flag_field
        self.flag_bit = flag_bit

    @property
    def is_flags(self):
        return self.type == "#"

    def __repr__(self):
        if self.flag_field:
            return f"{self.name}:{self.flag_field}.{self.flag_bit}?{self.type}"
        return f"{self.name}:{self.type}"


class Combinator:
    __slots__ = ("predicate", "id", "params", "result", "is_function", "class_name")

    def __init__(self, predicate, id_, params, result, is_function):
        self.predicate = predicate
        self.id = id_
        self.params = params
        self.result = result
        self.is_function = is_function
        # Telegram for Android names: messages.setTyping -> TL_messages_setTyping
        self.class_name = "TL_" + predicate.replace(".", "_")


class Schema:
    def __init__(self, text):
        self.by_id = {}
        self.by_name = {}
        self.by_class = {}
        self.layer = 0
        functions = False
        for raw in text.splitlines():
            line = raw.strip()
            if not line:
                continue
            if line.startswith("//"):
                m = re.match(r"//\s*LAYER\s+(\d+)", line)
                if m:
                    self.layer = int(m.group(1))
                continue
            if line == "---functions---":
                functions = True
                continue
            if line == "---types---":
                functions = False
                continue
            m = _line_re.match(line)
            if not m:
                continue
            predicate, hex_id, body, result = m.groups()
            if predicate in ("vector",):
                continue
            params = []
            for token in body.split():
                if token.startswith("{") or token == "#" or token.startswith("["):
                    continue  # generic type parameters
                if ":" not in token:
                    continue
                name, type_ = token.split(":", 1)
                flag_field = flag_bit = None
                if "?" in type_ and "." in type_.split("?")[0]:
                    cond, type_ = type_.split("?", 1)
                    flag_field, bit = cond.split(".")
                    flag_bit = int(bit)
                params.append(Param(name, type_, flag_field, flag_bit))
            comb = Combinator(predicate, int(hex_id, 16), params, result, functions)
            self.by_id[comb.id] = comb
            self.by_name[predicate] = comb
            self.by_class[comb.class_name] = comb

    def find(self, name):
        """By class name (TL_messages_setTyping), predicate (messages.setTyping) or id."""
        if isinstance(name, int):
            return self.by_id.get(name)
        return self.by_class.get(name) or self.by_name.get(name)

    def ids_matching(self, text):
        """Constructor ids whose name contains `text` (add_hook(..., match_substring=True))."""
        needle = text[3:] if text.startswith("TL_") else text
        needle = needle.lower()
        return sorted(c.id for c in self.by_id.values()
                      if needle in c.class_name[3:].lower() or needle in c.predicate.lower())


_schema = None


def schema():
    global _schema
    if _schema is None:
        here = os.path.dirname(os.path.abspath(__file__))
        candidates = [os.environ.get("AYU_TL_SCHEMA", ""),
                      os.path.join(here, "api.tl"),
                      os.path.join(here, "..", "api.tl")]
        for path in candidates:
            if path and os.path.exists(path):
                with open(path, encoding="utf-8") as f:
                    _schema = Schema(f.read())
                break
        else:
            raise FileNotFoundError("api.tl (the TL schema of the client) is missing")
    return _schema


def set_schema_text(text):
    global _schema
    _schema = Schema(text)


# ---- objects ------------------------------------------------------------------------

_classes = {}


class TLObject:
    """A TL constructor. Unknown attributes read as None, like empty Java fields."""

    _comb = None

    def __init__(_self, /, **fields):
        # Positional-only: TL has fields named "self" (user.self), they must fit in **fields.
        object.__setattr__(_self, "_fields", {})
        for k, v in fields.items():
            _self._fields[k] = v

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        fields = object.__getattribute__(self, "_fields")
        if name in fields:
            return fields[name]
        comb = type(self)._comb
        if comb is not None:
            for p in comb.params:
                if p.name == name:
                    return _default_for(p) if p.is_flags else None
        return None

    def __setattr__(self, name, value):
        self._fields[name] = value

    def __eq__(self, other):
        return isinstance(other, TLObject) and type(self) is type(other) and self._fields == other._fields

    def __repr__(self):
        name = type(self).__name__
        inner = ", ".join(f"{k}={v!r}" for k, v in self._fields.items())
        return f"{name}({inner})"

    # Java-ish helpers Android plugins use
    def getClass(self):
        cls = type(self)
        return type("JavaClass", (), {"getSimpleName": staticmethod(lambda: cls.__name__),
                                      "getName": staticmethod(lambda: "org.telegram.tgnet.TLRPC$" + cls.__name__)})()

    @property
    def constructor(self):
        comb = type(self)._comb
        return _signed(comb.id) if comb else 0

    def to_dict(self):
        comb = type(self)._comb
        result = {"_": type(self).__name__}
        for k, v in self._fields.items():
            result[k] = _to_plain(v)
        if comb is not None and "flags" not in result and any(p.is_flags for p in comb.params):
            result["flags"] = _compute_flags(self, comb, "flags")
        return result


def _to_plain(v):
    if isinstance(v, TLObject):
        return v.to_dict()
    if isinstance(v, list):
        return [_to_plain(x) for x in v]
    if isinstance(v, bytes):
        return v.hex()
    return v


def _signed(u):
    return u - (1 << 32) if u >= (1 << 31) else u


def class_for(comb):
    cls = _classes.get(comb.id)
    if cls is None:
        cls = type(comb.class_name, (TLObject,), {"_comb": comb, "__module__": "org.telegram.tgnet.TLRPC"})
        _classes[comb.id] = cls
    return cls


class _Namespace:
    """`TLRPC.TL_messages_setTyping` -> class, created on first use."""

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        if name == "TL_error":
            return TL_error
        comb = schema().find(name)
        if comb is None:
            raise AttributeError(f"TLRPC has no class {name} in layer {schema().layer}")
        return class_for(comb)

    def __dir__(self):
        return sorted(schema().by_class)


TLRPC = _Namespace()


class TL_error(TLObject):
    """rpc_error, as on Android: .code and .text."""

    def __init__(self, code=0, text=""):
        super().__init__(code=code, text=text)


# ---- encoding -------------------------------------------------------------------------


def _default_for(p):
    return 0


def _present(value, type_):
    if type_ == "true":
        return bool(value)
    return value is not None


def _compute_flags(obj, comb, flag_name):
    flags = 0
    for p in comb.params:
        if p.flag_field == flag_name and _present(obj._fields.get(p.name), p.type):
            flags |= 1 << p.flag_bit
    return flags


class Writer:
    def __init__(self):
        self.parts = []

    def int(self, v):
        # Wrap into int32 range, so both signed and unsigned values encode as on the wire.
        self.parts.append(struct.pack("<i", ((int(v) + (1 << 31)) % (1 << 32)) - (1 << 31)))

    def uint(self, v):
        self.parts.append(struct.pack("<I", v & 0xFFFFFFFF))

    def long(self, v):
        v = int(v)
        if v >= (1 << 63):
            v -= 1 << 64
        self.parts.append(struct.pack("<q", v))

    def double(self, v):
        self.parts.append(struct.pack("<d", float(v)))

    def raw(self, b):
        self.parts.append(bytes(b))

    def bytes(self, b):
        if isinstance(b, str):
            b = b.encode("utf-8")
        b = bytes(b or b"")
        n = len(b)
        if n < 254:
            head = bytes([n])
            pad = (-(n + 1)) % 4
        else:
            head = bytes([254]) + struct.pack("<I", n)[:3]
            pad = (-n) % 4
        self.parts.append(head + b + b"\0" * pad)

    def data(self):
        return b"".join(self.parts)


def _write_value(w, type_, value):
    if type_ == "int" or type_ == "#":
        w.int(int(value or 0))
    elif type_ == "long":
        w.long(value or 0)
    elif type_ == "double":
        w.double(value or 0)
    elif type_ in ("string", "bytes"):
        w.bytes(value if value is not None else b"")
    elif type_ in ("int128", "int256"):
        size = 16 if type_ == "int128" else 32
        if isinstance(value, int):
            value = value.to_bytes(size, "little", signed=False)
        w.raw((value or b"").ljust(size, b"\0")[:size])
    elif type_ == "Bool":
        w.uint(BOOL_TRUE if value else BOOL_FALSE)
    elif type_ == "true":
        pass
    elif type_.startswith("Vector<") or type_.startswith("vector<"):
        inner = type_[7:-1]
        items = list(value or [])
        if type_[0] == "V":
            w.uint(VECTOR_ID)
        w.int(len(items))
        for item in items:
            _write_value(w, inner, item)
    elif type_.startswith("!"):
        _write_object(w, value)
    else:
        if value is None:
            raise ValueError(f"missing required {type_}")
        _write_object(w, value)


def _write_object(w, obj):
    if isinstance(obj, (bytes, bytearray)):
        w.raw(obj)  # already serialized
        return
    if not isinstance(obj, TLObject) or type(obj)._comb is None:
        raise TypeError(f"cannot serialize {obj!r}")
    comb = type(obj)._comb
    w.uint(comb.id)
    for p in comb.params:
        if p.is_flags:
            w.uint(_compute_flags(obj, comb, p.name))
            continue
        value = obj._fields.get(p.name)
        if p.flag_field:
            if not _present(value, p.type):
                # Another field on the same bit may have set it: then send a default.
                if not (_compute_flags(obj, comb, p.flag_field) >> p.flag_bit) & 1:
                    continue
                value = [] if p.type.lower().startswith("vector<") else (0 if p.type in ("int", "long", "double") else value)
            if p.type == "true":
                continue
        try:
            _write_value(w, p.type, value)
        except (ValueError, TypeError) as exc:
            raise type(exc)(f"{comb.class_name}.{p.name}: {exc}") from None


def encode(obj):
    w = Writer()
    _write_object(w, obj)
    return w.data()


# ---- decoding -------------------------------------------------------------------------


class Reader:
    def __init__(self, data, pos=0):
        self.data = memoryview(bytes(data))
        self.pos = pos

    def take(self, n):
        if self.pos + n > len(self.data):
            raise EOFError("TL data ended early")
        chunk = self.data[self.pos:self.pos + n].tobytes()
        self.pos += n
        return chunk

    def int(self):
        return struct.unpack("<i", self.take(4))[0]

    def uint(self):
        return struct.unpack("<I", self.take(4))[0]

    def long(self):
        return struct.unpack("<q", self.take(8))[0]

    def double(self):
        return struct.unpack("<d", self.take(8))[0]

    def bytes(self):
        n = self.take(1)[0]
        if n == 254:
            n = int.from_bytes(self.take(3), "little")
            data = self.take(n)
            self.take((-n) % 4)
        else:
            data = self.take(n)
            self.take((-(n + 1)) % 4)
        return data


def _read_value(r, type_):
    if type_ == "int" or type_ == "#":
        return r.int()
    if type_ == "long":
        return r.long()
    if type_ == "double":
        return r.double()
    if type_ == "string":
        return r.bytes().decode("utf-8", errors="replace")
    if type_ == "bytes":
        return r.bytes()
    if type_ == "int128":
        return r.take(16)
    if type_ == "int256":
        return r.take(32)
    if type_ == "Bool":
        return r.uint() == BOOL_TRUE
    if type_ == "true":
        return True
    if type_.startswith("Vector<") or type_.startswith("vector<"):
        inner = type_[7:-1]
        if type_[0] == "V":
            cid = r.uint()
            if cid != VECTOR_ID:
                raise ValueError(f"expected vector, got {cid:#x}")
        return [_read_value(r, inner) for _ in range(r.int())]
    return _read_object(r)


def _read_object(r):
    cid = r.uint()
    if cid == VECTOR_ID:  # raw vector of objects (e.g. a Vector<Peer> response)
        return [_read_object(r) for _ in range(r.int())]
    if cid == BOOL_TRUE:
        return True
    if cid == BOOL_FALSE:
        return False
    comb = schema().find(cid)
    if comb is None:
        raise ValueError(f"unknown TL constructor {cid:#010x} (layer {schema().layer})")
    obj = class_for(comb)()
    flags = {}
    for p in comb.params:
        if p.is_flags:
            flags[p.name] = r.uint()
            obj._fields[p.name] = flags[p.name]
            continue
        if p.flag_field and not (flags.get(p.flag_field, 0) >> p.flag_bit) & 1:
            if p.type == "true":
                obj._fields[p.name] = False
            continue
        obj._fields[p.name] = _read_value(r, p.type)
    return obj


def decode(data):
    r = Reader(data)
    return _read_object(r)


def decode_vector_of(data, inner):
    """Responses typed Vector<int>/Vector<long> carry no constructors for the items."""
    r = Reader(data)
    return _read_value(r, f"Vector<{inner}>")


def encode_vector_of(items, inner):
    """Boxed Vector<inner> of items, e.g. entities for a request field."""
    w = Writer()
    _write_value(w, f"Vector<{inner}>", list(items or []))
    return w.data()


def name_of(cid):
    comb = schema().find(cid)
    return comb.class_name if comb else f"TL_unknown_{cid:08x}"

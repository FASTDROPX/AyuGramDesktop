"""Settings rows returned from BasePlugin.create_settings(), as on Android."""

from dataclasses import dataclass, field
from typing import Any, Callable, List, Optional

import _bridge


def _token(fn):
    return _bridge.register_callback(fn) if fn is not None else None


@dataclass
class Header:
    text: str

    def to_dict(self):
        return {"kind": "header", "text": self.text}


@dataclass
class Divider:
    text: Optional[str] = None

    def to_dict(self):
        return {"kind": "divider", "text": self.text}


@dataclass
class Switch:
    key: str
    text: str
    default: bool
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_change: Optional[Callable[[bool], None]] = None
    on_long_click: Optional[Callable[[Any], None]] = None
    link_alias: Optional[str] = None

    def to_dict(self):
        return {"kind": "switch", "key": self.key, "text": self.text, "default": bool(self.default),
                "subtext": self.subtext, "icon": self.icon, "on_change": _token(self.on_change),
                "on_long_click": _token(self.on_long_click)}


@dataclass
class Selector:
    key: str
    text: str
    default: int
    items: List[str] = field(default_factory=list)
    icon: Optional[str] = None
    on_change: Optional[Callable[[int], None]] = None
    on_long_click: Optional[Callable[[Any], None]] = None
    link_alias: Optional[str] = None

    def to_dict(self):
        return {"kind": "selector", "key": self.key, "text": self.text, "default": int(self.default),
                "items": [str(i) for i in self.items], "icon": self.icon,
                "on_change": _token(self.on_change), "on_long_click": _token(self.on_long_click)}


@dataclass
class Input:
    key: str
    text: str
    default: Optional[str] = ""
    subtext: Optional[str] = None
    icon: Optional[str] = None
    on_change: Optional[Callable[[str], None]] = None
    on_long_click: Optional[Callable[[Any], None]] = None
    link_alias: Optional[str] = None

    def to_dict(self):
        return {"kind": "input", "key": self.key, "text": self.text, "default": self.default or "",
                "subtext": self.subtext, "icon": self.icon, "on_change": _token(self.on_change),
                "on_long_click": _token(self.on_long_click)}


@dataclass
class Text:
    text: str
    subtext: Optional[str] = None
    icon: Optional[str] = None
    accent: bool = False
    red: bool = False
    on_click: Optional[Callable[[Any], None]] = None
    on_long_click: Optional[Callable[[Any], None]] = None
    create_sub_fragment: Optional[Callable[[], List[Any]]] = None
    link_alias: Optional[str] = None

    def to_dict(self):
        return {"kind": "text", "text": self.text, "subtext": self.subtext, "icon": self.icon,
                "accent": bool(self.accent), "red": bool(self.red), "on_click": _token(self.on_click),
                "on_long_click": _token(self.on_long_click),
                "sub_page": _token(self.create_sub_fragment)}


@dataclass
class EditText:
    key: str
    hint: str = ""
    default: Optional[str] = ""
    multiline: bool = False
    max_length: Optional[int] = None
    mask: Optional[str] = None
    on_change: Optional[Callable[[str], None]] = None

    def to_dict(self):
        return {"kind": "edit_text", "key": self.key, "hint": self.hint, "default": self.default or "",
                "multiline": bool(self.multiline), "max_length": self.max_length, "mask": self.mask,
                "on_change": _token(self.on_change)}


@dataclass
class Custom:
    """Android-only row type. Rows built from a View/UItem/factory cannot be
    drawn on Windows; they show as a note so the rest of the page still works."""
    item: Any = None
    view: Any = None
    factory: Any = None
    factory_args: Any = None
    on_click: Optional[Callable[[Any], None]] = None
    on_long_click: Optional[Callable[[Any], None]] = None
    create_sub_fragment: Optional[Callable[[], List[Any]]] = None
    link_alias: Optional[str] = None

    def to_dict(self):
        return {"kind": "unsupported", "text": "Custom row (Android only)",
                "on_click": _token(self.on_click), "sub_page": _token(self.create_sub_fragment)}

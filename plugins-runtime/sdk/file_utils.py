"""file_utils for exteraGram Desktop. Directories live under the app's data folder."""

import os
import traceback
from typing import List, Optional

import _bridge

_DIRS = {}


def _set_dirs(dirs):
    _DIRS.update(dirs)


def _dir(name):
    path = _DIRS.get(name) or os.path.join(_DIRS.get("files", os.getcwd()), name)
    os.makedirs(path, exist_ok=True)
    return path


def get_plugins_dir() -> str:
    return _dir("plugins")


def get_cache_dir() -> str:
    return _dir("cache")


def get_files_dir() -> str:
    return _dir("files")


def get_images_dir() -> str:
    return _dir("images")


def get_videos_dir() -> str:
    return _dir("videos")


def get_audios_dir() -> str:
    return _dir("audios")


def get_documents_dir() -> str:
    return _dir("documents")


def ensure_dir_exists(path: str) -> None:
    os.makedirs(path, exist_ok=True)


def list_dir(path: str, recursive: bool = False, include_files: bool = True, include_dirs: bool = False,
             extensions: Optional[List[str]] = None) -> List[str]:
    exts = tuple(e.lower() for e in extensions) if extensions else None
    result = []
    for root, dirs, files in os.walk(path):
        if include_dirs:
            result.extend(os.path.join(root, d) for d in dirs)
        if include_files:
            for f in files:
                if exts is None or f.lower().endswith(exts):
                    result.append(os.path.join(root, f))
        if not recursive:
            break
    return result


def _log_failure(action, path):
    _bridge.log(_bridge.plugin_id(), "error", f"{action} {path} failed:\n{traceback.format_exc()}")


def write_file(path: str, content: str) -> None:
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
    except OSError:
        _log_failure("write_file", path)


def read_file(path: str) -> Optional[str]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read()
    except OSError:
        _log_failure("read_file", path)
        return None


def write_file_bytes(path: str, content: bytes) -> None:
    try:
        with open(path, "wb") as f:
            f.write(content)
    except OSError:
        _log_failure("write_file_bytes", path)


def read_file_bytes(path: str) -> Optional[bytes]:
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        _log_failure("read_file_bytes", path)
        return None


def delete_file(path: str) -> bool:
    try:
        os.remove(path)
        return True
    except OSError:
        return False

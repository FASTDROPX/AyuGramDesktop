"""hook_utils exists on Android to reflect into Telegram's Java classes.

exteraGram Desktop has no Java runtime, so every call explains that instead of
failing with an obscure ImportError. Use request hooks (`add_hook`) and
`client_utils.send_request` with TDLib objects instead.
"""

_MESSAGE = ("hook_utils reflects into Telegram for Android's Java classes; exteraGram Desktop runs on "
            "TDLib and has no Java runtime. Use add_hook()/send_request() with TDLib objects instead.")


def _unsupported(*args, **kwargs):
    raise NotImplementedError(_MESSAGE)


find_class = _unsupported
get_private_field = _unsupported
set_private_field = _unsupported
get_static_private_field = _unsupported
set_static_private_field = _unsupported


def __getattr__(name):
    return _unsupported

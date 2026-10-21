"""Small stand-ins for Telegram for Android classes that plugins commonly touch."""

from client_utils import MessageObject  # noqa: F401


class R:
    """Drawable/raw resource ids do not exist on desktop; any attribute is 0."""

    class _Res:
        def __getattr__(self, name):
            return 0

    drawable = _Res()
    raw = _Res()
    string = _Res()


class AndroidUtilities:
    @staticmethod
    def dp(value):
        return int(round(value))

    @staticmethod
    def runOnUIThread(fn, delay=0):
        from android_utils import run_on_ui_thread
        run_on_ui_thread(getattr(fn, "run", fn), delay)

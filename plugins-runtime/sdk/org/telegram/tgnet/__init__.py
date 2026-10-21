"""`from org.telegram.tgnet import TLRPC` as on Android: TL classes from the client's api.tl."""

from tl import TLObject, TLRPC, TL_error  # noqa: F401

TLRPC.TLObject = TLObject


class ConnectionsManager:
    """Android's request API, mapped to client_utils.send_request."""

    RequestFlagFailOnServerErrors = 2
    RequestFlagWithoutLogin = 8
    RequestFlagInvokeAfter = 64

    def __init__(self, account=0):
        self.account = account

    @staticmethod
    def getInstance(account=0):
        return ConnectionsManager(account)

    def sendRequest(self, request, delegate=None, flags=0):
        from client_utils import send_request
        callback = getattr(delegate, "run", delegate)
        return send_request(request, callback, self.account)

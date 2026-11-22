"""Sessões do Hermes fora do WS do painel (Telegram, desktop): fim detectado pelo REST."""
import asyncio
import logging
from types import SimpleNamespace

from hub.hermes_client import HermesClient
from hub.state import HubState


class FakeHub:
    def __init__(self):
        self.state = HubState()
        self.finished = []
        self.log = logging.getLogger("test")

    def upsert_session(self, s):
        self.state.upsert_session(s)

    async def push_finished(self, sid, title):
        self.finished.append((sid, title))

    async def push_sessions(self):
        pass


def make(rows, tails):
    hub = FakeHub()
    cfg = SimpleNamespace(ws_url="ws://x/api/ws", rest_base="http://x", token=None, reconnect_delay_s=1,
                          ping_interval_s=1, cron_refresh_s=60)
    c = HermesClient.__new__(HermesClient)
    HermesClient.__init__(c, hub, cfg)

    async def rest(path):
        if path.startswith("/api/sessions?"):
            return {"sessions": rows}
        sid = path.split("/")[3]
        return {"messages": [tails[sid]]}
    c._rest_get = rest
    return c, hub


def test_classify_tail():
    k = HermesClient.classify_tail
    assert k({"role": "assistant", "tool_calls": None})[0] == "finished"
    assert k({"role": "assistant", "tool_calls": [{"function": {"name": "terminal"}}]}) == ("working", "tool: terminal")
    assert k({"role": "assistant", "tool_calls": [{"function": {"name": "clarify"}}]})[0] == "needs-you"
    assert k({"role": "tool"})[0] == "working"


def test_telegram_session_finishes_with_plim_and_oneshot_is_silent():
    async def go():
        rows = [{"id": "tg", "source": "telegram", "title": "Fabrica", "message_count": 3, "last_active": 1, "ended_at": None},
                {"id": "os", "source": "oneshot", "preview": "voz", "message_count": 1, "last_active": 1, "ended_at": None}]
        tails = {"tg": {"role": "assistant", "tool_calls": [{"function": {"name": "terminal"}}]},
                 "os": {"role": "user"}}
        c, hub = make(rows, tails)
        await c._poll_activity()                      # retrato inicial: sem som
        assert hub.finished == [] and hub.state.sessions["tg"].status == "working"
        rows[0].update(message_count=5, last_active=2); tails["tg"] = {"role": "assistant", "tool_calls": None}
        rows[1].update(message_count=2, last_active=2, ended_at=3); tails["os"] = {"role": "assistant"}
        await c._poll_activity()
        assert hub.finished == [("tg", "Fabrica")]
        await c._poll_activity()                      # sem mudança: não repete
        assert hub.finished == [("tg", "Fabrica")]
        await c._session.close()
    asyncio.run(go())

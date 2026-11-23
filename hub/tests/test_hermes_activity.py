"""Sessões do Hermes: execução visível e sessões paradas."""
import asyncio
import time

from hub.hermes_client import HermesClient
from hub.state import HubState, Session


class FakeHub:
    def __init__(self):
        self.state = HubState()
        self.log = __import__("logging").getLogger("t")
        self.finished = []

    def upsert_session(self, s):
        self.state.upsert_session(s)

    async def push_finished(self, sid, title, source=""):
        self.finished.append(sid)

    async def push_sessions(self):
        pass


def _src(rows, tails):
    src = HermesClient.__new__(HermesClient)
    src.hub = FakeHub()
    src.log = src.hub.log
    src._activity = {}
    src._activity_primed = True
    src._known_sessions = set()
    src._live_ids = set()
    src.finish_skip_sources = set()
    src.stale_after = 900

    async def rest_get(path):
        if path.startswith("/api/profiles/sessions") or path.startswith("/api/sessions?"):
            return {"sessions": rows}
        sid = path.split("/")[3]
        return {"messages": [tails[sid]]}

    src._rest_get = rest_get
    return src


def test_running_session_stays_working_after_sessions_refresh():
    now = time.time()
    rows = [{"id": "a", "title": "Rodando", "message_count": 3, "last_active": now, "source": "local"}]
    src = _src(rows, {"a": {"role": "user", "content": "faz x"}})
    src.hub.state.upsert_session(Session(id="a", title="Rodando", source="hermes", status="idle"))
    asyncio.run(src._poll_activity())
    assert src.hub.state.sessions["a"].status == "working"
    asyncio.run(src._refresh_rest_sessions())          # sessions.changed
    assert src.hub.state.sessions["a"].status == "working"


def test_old_unfinished_session_is_idle():
    rows = [{"id": "b", "title": "Velha", "message_count": 9, "last_active": time.time() - 7200, "source": "local"}]
    src = _src(rows, {"b": {"role": "tool", "content": "x"}})
    asyncio.run(src._poll_activity())
    assert src.hub.state.sessions["b"].status == "idle"

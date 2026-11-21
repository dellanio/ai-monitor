"""Hub v0.1 tests: fake Hermes dashboard + fake device + Claude fixtures.

No test talks to a real Hermes or a real device. The live-dashboard check
is a separate manual step (see README.md).
"""
import asyncio
import json

import aiohttp
import pytest
from websockets.asyncio.client import connect as ws_connect
from websockets.asyncio.server import serve as ws_serve

from hub.claude_source import parse_session_file
from hub.config import (ApprovalConfig, ClaudeConfig, Config, DeviceConfig, HermesConfig,
                        HostStatsConfig, TimonConfig)
from hub.host_stats import HostStats
from hub.hub import Hub


# ---------------------------------------------------------------------------
# Fakes
# ---------------------------------------------------------------------------
class FakeHermes:
    """Minimal JSON-RPC Hermes dashboard: gateway.ready, active_list,
    events.since, ping, usage.bars, plus proactive server->client approvals."""

    def __init__(self, active_sessions, send_approval=None, close_after_approval=False,
                 open_requests=None):
        self.active_sessions = active_sessions
        self.send_approval = send_approval  # dict params for one approval request
        self.close_after_approval = close_after_approval
        self.open_requests = open_requests or []
        self.approval_responses = asyncio.Queue()
        self.received = []
        self.connections = 0
        self._server = None
        self._ws = None

    async def start(self):
        self._server = await ws_serve(self._handler, "127.0.0.1", 0)
        self.port = self._server.sockets[0].getsockname()[1]
        return self.port

    async def stop(self):
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    async def send_event(self, params):
        if self._ws is not None:
            await self._ws.send(json.dumps({"jsonrpc": "2.0", "method": "event", "params": params}))

    async def _handler(self, ws):
        self.connections += 1
        self._ws = ws
        await ws.send(json.dumps({
            "jsonrpc": "2.0", "method": "event",
            "params": {"type": "gateway.ready",
                       "payload": {"skin": {}, "change_events": True, "heartbeat": True, "replay_epoch": 1}},
        }))
        try:
            async for raw in ws:
                msg = json.loads(raw)
                self.received.append(msg)
                method = msg.get("method")
                if method == "client.capabilities":
                    await ws.send(json.dumps({"jsonrpc": "2.0", "result": {"ok": True}, "id": msg["id"]}))
                    if self.send_approval is not None:
                        asyncio.create_task(self._proactive_approval(ws))
                elif method == "session.active_list":
                    await ws.send(json.dumps({"jsonrpc": "2.0",
                                              "result": {"sessions": self.active_sessions},
                                              "id": msg["id"]}))
                elif method == "session.events.since":
                    await ws.send(json.dumps({"jsonrpc": "2.0",
                                              "result": {"frames": [], "epoch": 1,
                                                         "open_requests": self.open_requests},
                                              "id": msg["id"]}))
                elif method == "session.resume":
                    await ws.send(json.dumps({"jsonrpc": "2.0",
                                              "result": {"open_requests": self.open_requests},
                                              "id": msg["id"]}))
                elif method == "gateway.ping":
                    await ws.send(json.dumps({"jsonrpc": "2.0", "result": {"ok": True}, "id": msg["id"]}))
                elif method == "usage.bars":
                    await ws.send(json.dumps({"jsonrpc": "2.0",
                                              "result": {"ok": True, "available": False},
                                              "id": msg["id"]}))
                elif isinstance(msg.get("id"), str) and msg["id"].startswith("srq-") and "result" in msg:
                    await self.approval_responses.put(msg)
                elif "id" in msg:
                    await ws.send(json.dumps({"jsonrpc": "2.0", "result": {"ok": True}, "id": msg["id"]}))
        except Exception:
            pass

    async def _proactive_approval(self, ws):
        await asyncio.sleep(0.2)
        params = self.send_approval
        await ws.send(json.dumps({
            "jsonrpc": "2.0", "id": params["request_id"],
            "method": "approval", "params": params,
        }))
        if self.close_after_approval:
            await asyncio.sleep(0.5)
            await ws.close()


TOKEN = "teste-token-1234567"


class FakeDevice:
    """A fake AI Monitor: connects to the Hub device server, records
    frames, and can answer approvals."""

    def __init__(self):
        self._queue = asyncio.Queue()
        self._buffer: list[dict] = []
        self._ws = None
        self._reader: asyncio.Task | None = None

    async def connect(self, port, token=TOKEN):
        url = f"ws://127.0.0.1:{port}/" + (f"?token={token}" if token else "")
        self._ws = await ws_connect(url)
        self._reader = asyncio.create_task(self._read_loop())

    async def close(self):
        if self._reader:
            self._reader.cancel()
        if self._ws:
            await self._ws.close()

    async def _read_loop(self):
        try:
            async for raw in self._ws:
                try:
                    self._queue.put_nowait(json.loads(raw))
                except ValueError:
                    pass
        except Exception:
            pass

    async def wait_for(self, mtype, timeout=10, predicate=None):
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout
        while True:
            for i, m in enumerate(self._buffer):
                if m.get("type") == mtype and (predicate is None or predicate(m)):
                    return self._buffer.pop(i)
            remaining = deadline - loop.time()
            if remaining <= 0:
                raise TimeoutError(f"no {mtype} frame within {timeout}s (got: {self._buffer})")
            try:
                m = await asyncio.wait_for(self._queue.get(), remaining)
            except TimeoutError:
                raise TimeoutError(f"no {mtype} frame within {timeout}s (got: {self._buffer})")
            self._buffer.append(m)

    async def count(self, mtype, predicate=None):
        n = 0
        for m in self._buffer:
            if m.get("type") == mtype and (predicate is None or predicate(m)):
                n += 1
        return n

    async def approve(self, request_id, choice="once"):
        await self._ws.send(json.dumps({"type": "approve", "request_id": request_id, "choice": choice}))


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def make_config(hermes=None, claude=None, approval_timeout=30.0, host=None,
                allow_actions=True, allow_messages=False, timon=None):
    return Config(
        device=DeviceConfig(host="127.0.0.1", port=0, mdns=False, token=TOKEN,
                            allow_actions=allow_actions, allow_messages=allow_messages),
        hermes=hermes or HermesConfig(enabled=False),
        claude=claude or ClaudeConfig(enabled=False),
        approval=ApprovalConfig(timeout_s=approval_timeout),
        host=host or HostStatsConfig(enabled=False),
        timon=timon or TimonConfig(enabled=False),
    )


async def wait_until(predicate, timeout=10, interval=0.1):
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while loop.time() < deadline:
        if predicate():
            return True
        await asyncio.sleep(interval)
    raise TimeoutError("condition not met")


# ---------------------------------------------------------------------------
# Hermes tests
# ---------------------------------------------------------------------------
ACTIVE = [{
    "id": "h1", "title": "Refatorar o monitor", "status": "working",
    "model": "claude-opus-4-6", "source": "cli", "started_at": 1,
    "last_active": 1, "message_count": 3, "preview": "trabalhando no monitor",
    "current": True, "session_key": "agent:main",
}]


async def test_hermes_working_then_finished():
    fake = FakeHermes(active_sessions=list(ACTIVE))
    port = await fake.start()
    cfg = make_config(hermes=HermesConfig(
        enabled=True, ws_url=f"ws://127.0.0.1:{port}/api/ws",
        rest_base="http://127.0.0.1:1", reconnect_delay_s=0.2, ping_interval_s=60,
    ))
    hub = Hub(cfg)
    device = FakeDevice()
    try:
        await hub.start()
        await device.connect(hub.device.actual_port)
        # PROTOCOL.md shape: sessions list with a working Hermes session.
        snap = await device.wait_for("sessions", timeout=10,
                                     predicate=lambda m: any(
                                         s["id"] == "h1" and s["status"] == "working"
                                         for s in m["sessions"]))
        assert snap["sessions"][0]["source"] == "hermes"
        assert snap["sessions"][0]["title"] == "Refatorar o monitor"
        # Turn ends -> device gets the blue "finished" notice.
        await fake.send_event({"type": "message.complete", "session_id": "h1", "payload": {}})
        fin = await device.wait_for("finished", timeout=10,
                                    predicate=lambda m: m["session_id"] == "h1")
        assert fin["title"] == "Refatorar o monitor"
        snap2 = await device.wait_for("sessions", timeout=10,
                                      predicate=lambda m: any(
                                          s["id"] == "h1" and s["status"] == "finished"
                                          for s in m["sessions"]))
        assert snap2 is not None
    finally:
        await device.close()
        await hub.stop()
        await fake.stop()


async def test_hermes_approval_roundtrip():
    approval_params = {
        "request_id": "srq-test-1", "session_id": "h1",
        "command": "rm -rf /tmp/build", "description": "Remove diretorio de build",
        "choices": ["once", "session", "always", "deny"],
    }
    fake = FakeHermes(active_sessions=list(ACTIVE), send_approval=approval_params)
    port = await fake.start()
    cfg = make_config(hermes=HermesConfig(
        enabled=True, ws_url=f"ws://127.0.0.1:{port}/api/ws",
        rest_base="http://127.0.0.1:1", reconnect_delay_s=0.2, ping_interval_s=60,
    ))
    hub = Hub(cfg)
    device = FakeDevice()
    try:
        await hub.start()
        await device.connect(hub.device.actual_port)
        ap = await device.wait_for("approval", timeout=10,
                                   predicate=lambda m: m["request_id"] == "srq-test-1")
        assert ap["command"] == "rm -rf /tmp/build"
        assert ap["choices"] == ["once", "session", "always", "deny"]
        await device.approve("srq-test-1", "once")
        resp = await asyncio.wait_for(fake.approval_responses.get(), timeout=10)
        assert resp["id"] == "srq-test-1"
        assert resp["result"] == {"choice": "once"}
    finally:
        await device.close()
        await hub.stop()
        await fake.stop()


async def test_hermes_reconnect_replays_open_approval():
    approval_params = {
        "request_id": "srq-test-2", "session_id": "h1",
        "command": "rm -rf /tmp/build", "description": "Remove diretorio de build",
        "choices": ["once", "deny"],
    }
    fake = FakeHermes(active_sessions=list(ACTIVE), send_approval=approval_params,
                      close_after_approval=True)
    port = await fake.start()
    cfg = make_config(hermes=HermesConfig(
        enabled=True, ws_url=f"ws://127.0.0.1:{port}/api/ws",
        rest_base="http://127.0.0.1:1", reconnect_delay_s=0.2, ping_interval_s=60,
    ))
    hub = Hub(cfg)
    device = FakeDevice()
    try:
        await hub.start()
        await device.connect(hub.device.actual_port)
        await device.wait_for("approval", timeout=10,
                              predicate=lambda m: m["request_id"] == "srq-test-2")
        # The fake drops the connection; on reconnect, events.since must
        # return the same approval as open_requests.
        fake.open_requests = [{"id": "srq-test-2", "method": "approval", "params": approval_params}]
        await wait_until(lambda: fake.connections >= 2, timeout=10)
        ap2 = await device.wait_for("approval", timeout=10,
                                    predicate=lambda m: m["request_id"] == "srq-test-2")
        assert ap2["command"] == "rm -rf /tmp/build"
        # The second wait_for above only succeeds if a SECOND approval frame
        # arrived after the reconnect (the first was already popped by the
        # first wait_for) — that is the replay working.
    finally:
        await device.close()
        await hub.stop()
        await fake.stop()


# ---------------------------------------------------------------------------
# Claude Code tests
# ---------------------------------------------------------------------------
def _write_jsonl(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for rec in records:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")


async def test_claude_watcher_lists_sessions(tmp_path):
    projects = tmp_path / "projects"
    _write_jsonl(projects / "proj-a" / "aaa111.jsonl", [
        {"type": "system", "subtype": "init", "cwd": "/tmp/proj-a"},
        {"type": "user", "message": {"role": "user", "content": "Refatorar o monitor"}},
        {"type": "assistant", "message": {"role": "assistant",
         "content": [{"type": "tool_use", "name": "Bash", "input": {}}], "stop_reason": None}},
        {"type": "assistant", "message": {"role": "assistant",
         "content": [{"type": "text", "text": "Pronto."}], "stop_reason": "end_turn"}},
    ])
    _write_jsonl(projects / "proj-b" / "bbb222.jsonl", [
        {"type": "user", "message": {"role": "user", "content": "Revisar o PR #42"}},
        {"type": "assistant", "message": {"role": "assistant",
         "content": [{"type": "tool_use", "name": "Read", "input": {}}], "stop_reason": None}},
    ])
    cfg = make_config(claude=ClaudeConfig(
        enabled=True, projects_root=str(projects), poll_interval_s=0.2,
        hook_host="127.0.0.1", hook_port=0,
    ))
    hub = Hub(cfg)
    try:
        await hub.start()
        await wait_until(lambda: len(hub.state.sessions) >= 2, timeout=10)
        s1 = hub.state.sessions["aaa111"]
        assert s1.title == "Refatorar o monitor"
        assert s1.status == "finished"
        assert s1.source == "claude"
        s2 = hub.state.sessions["bbb222"]
        assert s2.title == "Revisar o PR #42"
        assert s2.status == "working"
        assert s2.detail == "tool: Read"
    finally:
        await hub.stop()


async def _post_hook(port, payload):
    async with aiohttp.ClientSession() as session:
        async with session.post(f"http://127.0.0.1:{port}/hooks/claude",
                                json=payload, timeout=aiohttp.ClientTimeout(total=60)) as resp:
            return resp.status, await resp.json()


async def test_claude_hook_approval_allow(tmp_path):
    projects = tmp_path / "projects"
    projects.mkdir(parents=True)
    cfg = make_config(claude=ClaudeConfig(
        enabled=True, projects_root=str(projects), poll_interval_s=60,
        hook_host="127.0.0.1", hook_port=0,
    ), approval_timeout=30.0)
    hub = Hub(cfg)
    device = FakeDevice()
    try:
        await hub.start()
        await device.connect(hub.device.actual_port)
        claude = hub.source("claude")
        payload = {"hook_event_name": "PermissionRequest", "session_id": "ccc333",
                   "cwd": "/tmp/proj", "tool_name": "Bash",
                   "tool_input": {"command": "rm -rf /tmp/build"}}
        post_task = asyncio.create_task(_post_hook(claude.actual_hook_port, payload))
        ap = await device.wait_for("approval", timeout=10,
                                   predicate=lambda m: "Bash" in m["command"])
        assert ap["session_id"] == "ccc333"
        await device.approve(ap["request_id"], "once")
        status, body = await asyncio.wait_for(post_task, timeout=10)
        assert status == 200
        assert body == {"decision": "allow", "choice": "once"}
    finally:
        await device.close()
        await hub.stop()


async def test_claude_hook_timeout_leaves_decision_to_terminal(tmp_path):
    projects = tmp_path / "projects"
    projects.mkdir(parents=True)
    cfg = make_config(claude=ClaudeConfig(
        enabled=True, projects_root=str(projects), poll_interval_s=60,
        hook_host="127.0.0.1", hook_port=0,
    ), approval_timeout=1.0)
    hub = Hub(cfg)
    try:
        await hub.start()
        claude = hub.source("claude")
        payload = {"hook_event_name": "PermissionRequest", "session_id": "ddd444",
                   "cwd": "/tmp/proj", "tool_name": "Bash",
                   "tool_input": {"command": "rm -rf /tmp/build"}}
        status, body = await _post_hook(claude.actual_hook_port, payload)
        assert status == 200
        assert body == {"decision": "none", "choice": "timeout"}
    finally:
        await hub.stop()


def test_parse_session_file_unit(tmp_path):
    p = tmp_path / "s.jsonl"
    _write_jsonl(p, [
        {"type": "user", "message": {"role": "user", "content": "Ola mundo"}},
        {"type": "assistant", "message": {"role": "assistant",
         "content": [{"type": "text", "text": "ok"}], "stop_reason": "end_turn"}},
    ])
    parsed = parse_session_file(p)
    assert parsed["title"] == "Ola mundo"
    assert parsed["status"] == "finished"


# ---------------------------------------------------------------------------
# Host stats (tela COMPUTADOR)
# ---------------------------------------------------------------------------
class _StubProc:
    def __init__(self, name, pct):
        self.info = {"name": name}
        self._pct = pct

    def cpu_percent(self):
        return self._pct


class _StubCounters:
    def __init__(self, recv, sent):
        self.bytes_recv = recv
        self.bytes_sent = sent


class _StubBatt:
    def __init__(self, pct, secs):
        self.percent = pct
        self.secsleft = secs


class _StubPsutil:
    """Fake psutil with fixed samples (no real hardware needed)."""

    def __init__(self):
        self._net = [(1_000_000, 500_000), (3_000_000, 900_000)]
        self._net_i = 0
        self._batt = _StubBatt(12, 1500)

    def cpu_percent(self, interval=None):
        return 34.4

    def virtual_memory(self):
        class V:
            percent = 61.2

        return V()

    def process_iter(self, attrs=None):
        return [_StubProc("Chrome", 41.0), _StubProc("Code", 12.0)]

    def net_io_counters(self):
        c = self._net[min(self._net_i, len(self._net) - 1)]
        self._net_i += 1
        return _StubCounters(*c)

    def sensors_battery(self):
        return self._batt


def test_host_stats_collect_stub():
    hs = HostStats(psutil_mod=_StubPsutil(), gpu_cmd="/bin/false")
    assert hs.available
    hs._clock = lambda: 1000.0
    first = hs.collect()
    assert first["host_cpu_pct"] == 34
    assert first["host_mem_pct"] == 61
    assert first["host_gpu_pct"] is None  # /bin/false: sem GPU
    assert first["host_top_name"] == "Chrome"
    assert first["host_top_pct"] == 41
    assert first["host_net_down_mbps"] == 0.0  # primeira amostra: sem taxa
    assert first["host_net_up_mbps"] == 0.0
    assert first["host_batt_pct"] == 12
    assert first["host_batt_mins"] == 25  # 1500 s // 60
    # Segunda amostra, 2 s depois: taxa a partir do delta de bytes.
    hs._clock = lambda: 1002.0
    second = hs.collect()
    assert second["host_net_down_mbps"] == 1.0  # 2 MB / 2 s
    assert second["host_net_up_mbps"] == 0.2  # 0,4 MB / 2 s


def test_host_stats_no_battery():
    stub = _StubPsutil()
    stub._batt = None  # desktop sem bateria
    hs = HostStats(psutil_mod=stub, gpu_cmd="/bin/false")
    sample = hs.collect()
    assert sample["host_batt_pct"] is None
    assert sample["host_batt_mins"] is None


async def test_host_stats_broadcast():
    """O Hub envia o frame `host` ao dispositivo com todos os campos."""
    cfg = make_config(host=HostStatsConfig(enabled=True, interval_s=60.0))
    hub = Hub(cfg)
    device = FakeDevice()
    try:
        await hub.start()
        await device.connect(hub.device.actual_port)
        hub.state.set_host_stats({
            "host_cpu_pct": 92, "host_mem_pct": 78, "host_gpu_pct": 71,
            "host_top_name": "Chrome", "host_top_pct": 41,
            "host_net_down_mbps": 48.0, "host_net_up_mbps": 1.2,
            "host_batt_pct": None, "host_batt_mins": None,
        })
        frame = await device.wait_for(
            "host", timeout=10,
            predicate=lambda m: m["host_cpu_pct"] == 92)
        assert frame["host_mem_pct"] == 78
        assert frame["host_gpu_pct"] == 71
        assert frame["host_top_name"] == "Chrome"
        assert frame["host_top_pct"] == 41
        assert frame["host_net_down_mbps"] == 48.0
        assert frame["host_net_up_mbps"] == 1.2
        assert frame["host_batt_pct"] is None
    finally:
        await device.close()
        await hub.stop()


# ---------------------------------------------------------------------------
# Segurança: pareamento e modo só leitura
# ---------------------------------------------------------------------------
async def test_device_without_token_is_rejected():
    hub = Hub(make_config())
    try:
        await hub.start()
        for bad in (None, "token-errado-123456"):
            with pytest.raises(Exception) as exc:
                await FakeDevice().connect(hub.device.actual_port, token=bad)
            assert "401" in str(exc.value)
        assert not hub.device.clients
    finally:
        await hub.stop()


async def test_read_only_ignores_approve_and_message():
    fake = FakeHermes(ACTIVE, send_approval={
        "request_id": "srq-ro", "session_id": "h1", "command": "rm -rf /",
        "description": "perigoso", "choices": ["once", "deny"]})
    port = await fake.start()
    cfg = make_config(hermes=HermesConfig(enabled=True, ws_url=f"ws://127.0.0.1:{port}/api/ws",
                                          rest_base="http://127.0.0.1:1", token="x"),
                      allow_actions=False)
    hub = Hub(cfg)
    device = FakeDevice()
    try:
        await hub.start()
        await device.connect(hub.device.actual_port)
        hello = await device.wait_for("hello")
        assert hello["actions"] is False and hello["messages"] is False
        await device.wait_for("approval", timeout=10)
        await device.approve("srq-ro", "once")
        await device._ws.send(json.dumps({"type": "message", "session_id": "h1", "text": "oi"}))
        with pytest.raises(asyncio.TimeoutError):
            await asyncio.wait_for(fake.approval_responses.get(), 1.5)
        assert not any(m.get("method") == "prompt.submit" for m in fake.received)
    finally:
        await device.close()
        await hub.stop()
        await fake.stop()


async def test_pre_tool_use_never_blocks(tmp_path):
    projects = tmp_path / "projects"
    projects.mkdir(parents=True)
    cfg = make_config(claude=ClaudeConfig(
        enabled=True, projects_root=str(projects), poll_interval_s=60,
        hook_host="127.0.0.1", hook_port=0), approval_timeout=30.0)
    hub = Hub(cfg)
    try:
        await hub.start()
        claude = hub.source("claude")
        payload = {"hook_event_name": "PreToolUse", "session_id": "eee555",
                   "tool_name": "Read", "tool_input": {"file_path": "x"}}
        status, body = await asyncio.wait_for(_post_hook(claude.actual_hook_port, payload), 3)
        assert status == 200 and body == {"decision": "none"}
        assert not hub.state.pending
    finally:
        await hub.stop()


async def test_permission_request_read_only_does_not_wait(tmp_path):
    projects = tmp_path / "projects"
    projects.mkdir(parents=True)
    cfg = make_config(claude=ClaudeConfig(
        enabled=True, projects_root=str(projects), poll_interval_s=60,
        hook_host="127.0.0.1", hook_port=0), approval_timeout=30.0, allow_actions=False)
    hub = Hub(cfg)
    try:
        await hub.start()
        claude = hub.source("claude")
        payload = {"hook_event_name": "PermissionRequest", "session_id": "fff666",
                   "tool_name": "Bash", "tool_input": {"command": "ls"}}
        status, body = await asyncio.wait_for(_post_hook(claude.actual_hook_port, payload), 3)
        assert body == {"decision": "none"}
        assert hub.state.sessions["fff666"].status == "needs-you"
    finally:
        await hub.stop()


def test_ensure_device_token(tmp_path):
    from hub.config import ensure_device_token
    cfg = Config()
    p = tmp_path / "tok"
    tok, created = ensure_device_token(cfg, str(p))
    assert created and len(tok) >= 12 and p.read_text().strip() == tok
    tok2, created2 = ensure_device_token(Config(), str(p))
    assert tok2 == tok and not created2
    cfg3 = Config(); cfg3.device.token = "curto"
    with pytest.raises(ValueError):
        ensure_device_token(cfg3, str(p))


# ---------------------------------------------------------------------------
# Timon
# ---------------------------------------------------------------------------
from aiohttp import web  # noqa: E402
import time as _time  # noqa: E402

from hub.timon_source import mission_to_session  # noqa: E402


def _mission(mid, **kw):
    base = {"id": mid, "agent_name": "Coder", "agent_role": "coder", "model": "sonnet",
            "title": f"Missao {mid}", "status": "running", "running": True, "turn_active": True,
            "turn_complete": False, "pending_action_at": None, "ended_at": None,
            "tokens_in": 10, "tokens_out": 20, "total_cost_usd": 0.05, "span_tools": 3}
    base.update(kw)
    return base


def test_timon_mapping():
    now = _time.time() * 1000
    s = mission_to_session(_mission("a"), now, 600)
    assert s.id == "timon:a" and s.status == "working" and s.source == "timon" and s.profile == "Coder"
    assert mission_to_session(_mission("b", pending_action_at=now), now, 600).status == "needs-you"
    assert mission_to_session(_mission("c", turn_active=False, turn_complete=True), now, 600).status == "idle"
    done = _mission("d", status="completed", running=False, turn_active=False, ended_at=now - 1000)
    assert mission_to_session(done, now, 600).status == "finished"
    old = dict(done, ended_at=now - 3600_000)
    assert mission_to_session(old, now, 600) is None
    killed = _mission("e", status="terminated", running=False, turn_active=False)
    assert mission_to_session(killed, now, 600) is None


class FakeTimon:
    def __init__(self, token):
        self.token = token
        self.missions = []
        self.auth_seen = []
        self.methods = []

    async def start(self):
        app = web.Application()
        app.router.add_route("*", "/api/missions", self._handler)
        self.runner = web.AppRunner(app)
        await self.runner.setup()
        site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await site.start()
        return site._server.sockets[0].getsockname()[1]

    async def stop(self):
        await self.runner.cleanup()

    async def _handler(self, request):
        self.methods.append(request.method)
        self.auth_seen.append(request.headers.get("Authorization"))
        if request.headers.get("Authorization") != f"Bearer {self.token}":
            return web.json_response({"error": "unauthorized"}, status=401)
        return web.json_response({"missions": self.missions})


async def test_timon_source_lists_finishes_and_removes(tmp_path):
    tok = tmp_path / "daemon-token"
    tok.write_text("tok-timon-123\n")
    fake = FakeTimon("tok-timon-123")
    port = await fake.start()
    fake.missions = [_mission("m1"), _mission("m2", pending_action_at=1)]
    cfg = make_config(timon=TimonConfig(enabled=True, api_url=f"http://127.0.0.1:{port}",
                                        token=f"file:{tok}", poll_interval_s=0.2))
    hub = Hub(cfg)
    device = FakeDevice()
    try:
        await hub.start()
        await device.connect(hub.device.actual_port)
        snap = await device.wait_for("sessions", predicate=lambda m: len(
            [s for s in m["sessions"] if s["source"] == "timon"]) == 2)
        by = {s["id"]: s for s in snap["sessions"]}
        assert by["timon:m1"]["status"] == "working"
        assert by["timon:m2"]["status"] == "needs-you"
        # m1 termina, m2 é interrompida -> aviso azul de m1, m2 some
        fake.missions = [_mission("m1", status="completed", running=False, turn_active=False,
                                  turn_complete=True, ended_at=_time.time() * 1000)]
        fin = await device.wait_for("finished", predicate=lambda m: m["session_id"] == "timon:m1")
        assert fin["title"] == "Missao m1"
        await wait_until(lambda: "timon:m2" not in hub.state.sessions)
        assert set(fake.methods) == {"GET"}
    finally:
        await device.close()
        await hub.stop()
        await fake.stop()


async def test_timon_refuses_remote_url(tmp_path):
    cfg = make_config(timon=TimonConfig(enabled=True, api_url="http://192.168.0.10:3010",
                                        token="segredo-qualquer"))
    hub = Hub(cfg)
    try:
        await hub.start()
        await asyncio.sleep(0.3)
        assert hub.source("timon")._task is None
    finally:
        await hub.stop()

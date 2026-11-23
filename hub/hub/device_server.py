"""WebSocket server for the AI Monitor device (ESP32-S3).

Speaks firmware/PROTOCOL.md exactly: the device connects to
``ws://<pc-ip>:<port>/`` and receives ``hello`` / ``sessions`` / ``approval``
/ ``finished`` / ``usage`` / ``cron`` frames; it answers with ``approve`` /
``message`` / ``ping``. The Hub also announces itself via mDNS
(``_dellmonitor-hub._tcp.local.``) so the device can find it.
"""
from __future__ import annotations

import asyncio
import time
import hmac
import json
import socket
from http import HTTPStatus
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import serve

from .state import HubState, PendingApproval


def _lan_ip() -> str | None:
    """Best-effort LAN address (no traffic is sent)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return None


class DeviceServer:
    def __init__(self, state: HubState, host: str, port: int, mdns: bool = True, log=None,
                 token: str | None = None, allow_actions: bool = False,
                 allow_messages: bool = False) -> None:
        if not token:
            raise ValueError("DeviceServer exige um token de pareamento")
        self.token = token
        self.allow_actions = bool(allow_actions)
        self.allow_messages = bool(allow_messages)
        self.state = state
        self.host = host
        self.port = port
        self.mdns = mdns
        self.log = log
        self.clients: set = set()
        # Callbacks set by Hub: approve(request_id, choice), message(session_id, text)
        self.on_approve = None
        self.on_message = None
        self._server = None
        self._snapshot_event = asyncio.Event()
        self._last_sent: dict[str, str] = {}
        self._last_full = 0.0
        self._snapshot_task: asyncio.Task | None = None
        self._zc = None
        self._mdns_info = None
        self.actual_port = port
        self.idle_timeout_s = 45.0  # aparelho manda ping a cada 15 s

    async def start(self) -> None:
        self._server = await serve(
            self._handler, self.host, self.port,
            process_request=self._authorize,
            max_size=64 * 1024,
            # Sem ping de protocolo: o arduinoWebSockets do firmware não devolve o
            # pong a tempo e a conexão caía a cada ~50 s ("keepalive ping timeout").
            # A vida do link é de aplicação: o aparelho manda {"type":"ping"} a cada
            # 15 s e o Hub fecha após idle_timeout_s sem nada.
            ping_interval=None,
        )
        if self._server.sockets:
            self.actual_port = self._server.sockets[0].getsockname()[1]
        self.state.add_listener(self._request_snapshot)
        self._snapshot_task = asyncio.create_task(self._snapshot_loop())
        if self.mdns:
            await self._mdns_start()
        if self.log:
            self.log.info("device server on %s:%s", self.host, self.actual_port)

    async def stop(self) -> None:
        if self._snapshot_task:
            self._snapshot_task.cancel()
        if self._server:
            self._server.close()
            await self._server.wait_closed()
        await self._mdns_stop()

    # -- mDNS ------------------------------------------------------------
    async def _mdns_start(self) -> None:
        try:
            from zeroconf import ServiceInfo, Zeroconf
        except ImportError:
            if self.log:
                self.log.warning("zeroconf not installed; mDNS announce disabled")
            return
        ip = _lan_ip()
        if not ip:
            if self.log:
                self.log.warning("could not determine LAN IP; mDNS announce disabled")
            return

        def _register():
            zc = Zeroconf()
            info = ServiceInfo(
                "_dellmonitor-hub._tcp.local.",
                "hub-stack-chan._dellmonitor-hub._tcp.local.",
                addresses=[socket.inet_aton(ip)],
                port=self.actual_port,
                properties={b"version": b"1", b"path": b"/"},
            )
            zc.register_service(info)
            return zc, info

        try:
            self._zc, self._mdns_info = await asyncio.to_thread(_register)
            if self.log:
                self.log.info("mDNS: hub-stack-chan._dellmonitor-hub._tcp.local. -> %s:%s", ip, self.actual_port)
        except Exception as exc:  # best-effort only
            if self.log:
                self.log.warning("mDNS announce failed: %s", exc)

    async def _mdns_stop(self) -> None:
        if self._zc is None:
            return

        def _unregister():
            try:
                self._zc.unregister_service(self._mdns_info)
            finally:
                self._zc.close()

        try:
            await asyncio.to_thread(_unregister)
        except Exception:
            pass
        self._zc = None

    # -- snapshots -------------------------------------------------------
    def _request_snapshot(self) -> None:
        self._snapshot_event.set()

    async def _snapshot_loop(self) -> None:
        while True:
            try:
                await self._snapshot_event.wait()
            except asyncio.CancelledError:
                return
            self._snapshot_event.clear()
            await asyncio.sleep(0.05)  # coalesce bursts
            # Só manda o que mudou (o aparelho é lento para ler; reenviar tudo a
            # cada mudança enchia o TCP e travava os envios). Tudo de novo a cada 30 s.
            now = time.monotonic()
            if now - self._last_full > 30:
                self._last_sent.clear()
                self._last_full = now
            msgs = [self.state.sessions_payload(), *self.state.usage_payloads(), self.state.cron_payload()]
            host = self.state.host_payload()
            if host is not None:
                msgs.append(host)
            for payload in msgs:
                key = f"{payload.get('type')}:{payload.get('session_id', '')}"
                raw = json.dumps(payload, sort_keys=True)
                if self._last_sent.get(key) == raw:
                    continue
                if await self.broadcast(payload):
                    self._last_sent[key] = raw  # pulado por demora: tenta de novo na próxima

    # -- auth ------------------------------------------------------------
    def _token_ok(self, presented: str | None) -> bool:
        if not presented:
            return False
        return hmac.compare_digest(presented.encode(), self.token.encode())

    def _authorize(self, connection, request):
        """Recusa o handshake (401) sem o token certo, antes de enviar dados."""
        presented = None
        auth = request.headers.get("Authorization", "")
        if auth.lower().startswith("bearer "):
            presented = auth[7:].strip()
        if presented is None:
            qs = parse_qs(urlsplit(request.path).query)
            presented = (qs.get("token") or [None])[0]
        if self._token_ok(presented):
            return None
        if self.log:
            self.log.warning("device rejected (bad/missing token) from %s", connection.remote_address)
        return connection.respond(HTTPStatus.UNAUTHORIZED, "token invalido\n")

    # -- wire ------------------------------------------------------------
    async def broadcast(self, msg: dict) -> bool:
        """Envia a todos; False se algum envio estourou o prazo."""
        if not self.clients:
            return True
        raw = json.dumps(msg)
        dead = []
        ok = True
        for ws in list(self.clients):
            try:
                # Aparelho ocupado (tocando áudio) pode demorar a ler: não trava o Hub.
                await asyncio.wait_for(ws.send(raw), timeout=5)
            except asyncio.TimeoutError:
                ok = False
                if self.log:
                    self.log.info("envio ao aparelho demorou >5 s (%s): pulado", msg.get("type"))
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)
        return ok

    async def _send(self, ws, msg: dict) -> None:
        await ws.send(json.dumps(msg))

    async def _handler(self, ws) -> None:
        self.clients.add(ws)
        if self.log:
            self.log.info("device connected: %s", ws.remote_address)
        try:
            await self._send(ws, {"type": "hello", "hub": "dellmonitor-hub", "version": 2,
                                  "actions": self.allow_actions,
                                  "messages": self.allow_messages})
            await self._send(ws, self.state.sessions_payload())
            for payload in self.state.usage_payloads():
                await self._send(ws, payload)
            await self._send(ws, self.state.cron_payload())
            host = self.state.host_payload()
            if host is not None:
                await self._send(ws, host)
            for ap in list(self.state.pending.values()):
                if ap.kind == "approval":
                    await self._send(ws, ap.to_protocol())
            while True:
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=self.idle_timeout_s)
                except asyncio.TimeoutError:
                    if self.log:
                        self.log.info("device idle %.0fs -> fechando", self.idle_timeout_s)
                    await ws.close(code=1001, reason="idle")
                    break
                try:
                    msg = json.loads(raw)
                except (ValueError, TypeError):
                    continue
                await self._handle_message(ws, msg)
        except Exception as exc:
            if self.log:
                self.log.info("device link error: %r", exc)
        finally:
            self.clients.discard(ws)
            if self.log:
                self.log.info("device disconnected: %s (code=%s reason=%r)", ws.remote_address,
                              getattr(ws, "close_code", None), getattr(ws, "close_reason", None))

    async def _handle_message(self, ws, msg: dict) -> None:
        mtype = msg.get("type")
        if mtype == "hello":
            if self.log:
                self.log.info("device hello: %s", msg.get("device"))
        elif mtype == "ping":
            await self._send(ws, {"type": "pong"})
        elif mtype == "approve":
            if not self.allow_actions:
                if self.log:
                    self.log.warning("approve ignored: device.allow_actions is off")
                return
            if self.on_approve is not None:
                await self.on_approve(msg.get("request_id", ""), msg.get("choice", "deny"))
        elif mtype == "message":
            if not self.allow_messages:
                if self.log:
                    self.log.warning("message ignored: device.allow_messages is off")
                return
            if self.on_message is not None:
                await self.on_message(msg.get("session_id", ""), msg.get("text", ""))
        elif mtype:
            if self.log:
                self.log.debug("unknown device message type: %s", mtype)

    # -- pushes used by Hub/sources --------------------------------------
    async def push_approval(self, ap: PendingApproval) -> None:
        await self.broadcast(ap.to_protocol())

    async def push_approval_cancelled(self, request_id: str) -> None:
        await self.broadcast({"type": "approval_cancelled", "request_id": request_id})

    async def push_finished(self, session_id: str, title: str, source: str = "") -> None:
        from .state import device_text

        s = self.state.sessions.get(session_id)
        await self.broadcast({"type": "finished", "session_id": session_id, "title": device_text(title),
                              "source": source or (s.source if s else "")})

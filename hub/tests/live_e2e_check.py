"""End-to-end check: real Hub <-> real Hermes dashboard (with token).

Starts the Hub in-process, connects a fake device, and prints what the
device receives. Run while `hermes dashboard` is up with a known token.
"""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from tests.test_hub import FakeDevice
from hub.config import ApprovalConfig, ClaudeConfig, Config, DeviceConfig, HermesConfig
from hub.hub import Hub


async def main():
    token = os.environ["HERMES_PROBE_TOKEN"]
    cfg = Config(
        device=DeviceConfig(host="127.0.0.1", port=0, mdns=False),
        hermes=HermesConfig(
            enabled=True, ws_url="ws://127.0.0.1:9119/api/ws",
            rest_base="http://127.0.0.1:9119", token=token,
            reconnect_delay_s=2, ping_interval_s=15,
        ),
        claude=ClaudeConfig(enabled=False),
        approval=ApprovalConfig(timeout_s=300),
    )
    hub = Hub(cfg)
    device = FakeDevice()
    await hub.start()
    try:
        # Wait for the Hermes WS handshake + initial sync before judging.
        hermes = hub.source("hermes")
        for _ in range(60):
            if hermes._ws is not None:
                break
            await asyncio.sleep(0.25)
        print("hermes WS connected:", hermes._ws is not None)
        await device.connect(hub.device.actual_port)
        hello = await device.wait_for("hello", timeout=10)
        print("device hello:", hello)
        snap = await device.wait_for("sessions", timeout=10)
        print("device sessions:", snap)
        cron = await device.wait_for("cron", timeout=10)
        print("device cron:", cron)
        hermes = hub.source("hermes")
        print("hermes WS connected:", hermes._ws is not None)
        print("hermes disabled:", hermes._disabled)
        print("hub sessions in state:", list(hub.state.sessions))
        await asyncio.sleep(6)
        print("still connected after 6s:", hermes._ws is not None)
        print("E2E LIVE OK")
    finally:
        await device.close()
        await hub.stop()


asyncio.run(main())

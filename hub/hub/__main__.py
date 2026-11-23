"""hub-stack-chan (AI Monitor) entry point: ``python3 -m hub``."""
from __future__ import annotations

import argparse
import asyncio
import logging

import sys

from .config import ensure_device_token, load_config
from .hub import Hub


def _start_lag_watchdog() -> None:
    """Se o loop do asyncio ficar travado > 2 s, registra onde ele está preso.
    (Loop travado = o Hub para de ler os aparelhos e a conexão cai.)"""
    import threading
    import time
    import traceback

    log = logging.getLogger("dellmonitor.hub.watchdog")
    main_id = threading.get_ident()
    state = {"beat": time.monotonic()}

    async def beat():
        while True:
            state["beat"] = time.monotonic()
            await asyncio.sleep(0.5)

    def watch():
        reported = 0.0
        while True:
            time.sleep(0.5)
            lag = time.monotonic() - state["beat"]
            if lag > 2.0 and state["beat"] != reported:
                reported = state["beat"]
                frame = sys._current_frames().get(main_id)
                stack = "".join(traceback.format_stack(frame)[-8:]) if frame else "?"
                log.warning("loop travado ha %.1f s em:\n%s", lag, stack)

    threading.Thread(target=watch, daemon=True, name="lag-watchdog").start()
    asyncio.get_running_loop().create_task(beat())


async def _run(cfg) -> None:
    _start_lag_watchdog()
    # O Hub cria sessões aiohttp no __init__: precisa do loop já rodando.
    hub = Hub(cfg)
    await hub.start()
    try:
        await asyncio.Event().wait()  # run until cancelled (Ctrl-C / SIGTERM)
    finally:
        await hub.stop()


def main() -> None:
    parser = argparse.ArgumentParser(description="hub-stack-chan (AI Monitor) v0.2")
    parser.add_argument("--config", default="config.yaml", help="path to config.yaml")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )
    cfg = load_config(args.config)
    token, created = ensure_device_token(cfg)
    if created:
        # Mostrado só no console, uma vez: digite no portal do aparelho.
        print(f"\n  Token de pareamento do aparelho: {token}\n"
              f"  (salvo em ~/.dellmonitor/device_token)\n", file=sys.stderr)
    cfg.device.token = token
    try:
        asyncio.run(_run(cfg))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

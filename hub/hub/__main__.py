"""hub-stack-chan (AI Monitor) entry point: ``python3 -m hub``."""
from __future__ import annotations

import argparse
import asyncio
import logging

import sys

from .config import ensure_device_token, load_config
from .hub import Hub


async def _run(cfg) -> None:
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

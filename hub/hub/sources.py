"""Base class for Hub sources.

A source observes one AI tool (Hermes, Claude Code, ...) and writes into
the shared HubState. v0.2 ships Hermes, Claude Code and Timon; Codex and others are
future sources implementing this same interface (see README.md).
"""
from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .hub import Hub


class Source:
    """One AI tool observed by the Hub."""

    name = "base"

    def __init__(self, hub: "Hub", cfg) -> None:
        self.hub = hub
        self.cfg = cfg
        self.log = hub.log.getChild(f"source.{self.name}")

    async def start(self) -> None:
        """Start observing. Return quickly; spawn background tasks."""
        raise NotImplementedError

    async def stop(self) -> None:
        """Stop observing and release resources."""

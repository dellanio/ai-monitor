"""Configuration for the hub-stack-chan (AI Monitor) (config.yaml + defaults)."""
from __future__ import annotations

import os
from dataclasses import dataclass, field


@dataclass
class DeviceConfig:
    host: str = "0.0.0.0"
    port: int = 8790
    mdns: bool = True
    # Segredo compartilhado com o aparelho (literal ou "file:<caminho>").
    # Sem ele o Hub gera um em ~/.dellmonitor/device_token na 1a subida.
    token: str | None = None
    # Seguro por padrão: o aparelho só MOSTRA. Aprovar pelo toque e mandar
    # texto para uma sessão são opt-in explícitos.
    allow_actions: bool = False
    allow_messages: bool = False


@dataclass
class HermesConfig:
    enabled: bool = True
    ws_url: str = "ws://127.0.0.1:9119/api/ws"
    rest_base: str = "http://127.0.0.1:9119"
    token: str | None = None
    reconnect_delay_s: float = 5.0
    ping_interval_s: float = 15.0
    cron_refresh_s: float = 300.0


@dataclass
class ClaudeConfig:
    enabled: bool = True
    projects_root: str = "~/.claude/projects"
    poll_interval_s: float = 5.0
    hook_host: str = "127.0.0.1"
    hook_port: int = 8767
    recent_hours: float = 6.0      # só sessões mexidas nas últimas N horas
    stale_after_s: float = 600.0   # sem fim de turno e parado há 10 min -> "parada"


@dataclass
class TimonConfig:
    enabled: bool = False
    api_url: str = "http://127.0.0.1:3010"   # só loopback (token de operador)
    token: str | None = "file:~/.timon/daemon-token"
    poll_interval_s: float = 5.0
    finished_keep_s: float = 600.0           # "terminou" fica 10 min na tela


@dataclass
class CodexConfig:
    enabled: bool = False
    sessions_root: str = "~/.codex/sessions"
    poll_interval_s: float = 3.0
    recent_hours: float = 6.0
    stale_after_s: float = 600.0


@dataclass
class HostStatsConfig:
    enabled: bool = True
    interval_s: float = 2.0


@dataclass
class ApprovalConfig:
    timeout_s: float = 120.0


@dataclass
class VoiceConfig:
    """Voz: "Hey Hermes" detectado no notebook + canal de voz da assinatura (voz-hermes)."""
    enabled: bool = False
    host: str = "0.0.0.0"
    port: int = 8788
    canal_url: str = "ws://127.0.0.1:4850/voz/ws"   # canal local; o Hub não fala com a nuvem
    engine: str = "codex"                            # motor da assinatura ChatGPT
    session: str = "aimonitor"
    wake_model: str = "~/models/vosk-model-small-en-us-0.15"
    wake_phrases: list = field(default_factory=lambda: ["hey hermes", "hi hermes"])
    follow_up_s: float = 12.0   # sem você falar por N s depois da resposta -> volta à espera
    keep_s: float = 300.0       # conversa no canal fica aberta N s após a última ativação
    sounds_dir: str = "~/.dellmonitor/sounds"  # needyou.* / finished.* / new.* (wav, mp3, ogg...)
    sound_max_s: float = 6.0    # corta sons longos
    sound_gain: float = 1.0


@dataclass
class Config:
    device: DeviceConfig = field(default_factory=DeviceConfig)
    hermes: HermesConfig = field(default_factory=HermesConfig)
    claude: ClaudeConfig = field(default_factory=ClaudeConfig)
    approval: ApprovalConfig = field(default_factory=ApprovalConfig)
    host: HostStatsConfig = field(default_factory=HostStatsConfig)
    timon: TimonConfig = field(default_factory=TimonConfig)
    codex: CodexConfig = field(default_factory=CodexConfig)
    voice: VoiceConfig = field(default_factory=VoiceConfig)


def _merge(dc, data: dict):
    if not isinstance(data, dict):
        return dc
    for k, v in data.items():
        if hasattr(dc, k):
            setattr(dc, k, v)
    return dc


def load_config(path: str) -> Config:
    """Load config.yaml over the defaults. Missing file -> defaults."""
    cfg = Config()
    if path and os.path.exists(path):
        import yaml

        with open(path, encoding="utf-8") as f:
            data = yaml.safe_load(f) or {}
        cfg.device = _merge(DeviceConfig(), data.get("device", {}) or {})
        cfg.hermes = _merge(HermesConfig(), data.get("hermes", {}) or {})
        cfg.claude = _merge(ClaudeConfig(), data.get("claude", {}) or {})
        cfg.approval = _merge(ApprovalConfig(), data.get("approval", {}) or {})
        cfg.host = _merge(HostStatsConfig(), data.get("host", {}) or {})
        cfg.timon = _merge(TimonConfig(), data.get("timon", {}) or {})
        cfg.codex = _merge(CodexConfig(), data.get("codex", {}) or {})
        cfg.voice = _merge(VoiceConfig(), data.get("voice", {}) or {})
    # Environment overrides (handy for tests and containers).
    if os.environ.get("DELLMONITOR_DEVICE_PORT"):
        cfg.device.port = int(os.environ["DELLMONITOR_DEVICE_PORT"])
    if os.environ.get("DELLMONITOR_DEVICE_TOKEN"):
        cfg.device.token = os.environ["DELLMONITOR_DEVICE_TOKEN"]
    if os.environ.get("DELLMONITOR_HERMES_WS_URL"):
        cfg.hermes.ws_url = os.environ["DELLMONITOR_HERMES_WS_URL"]
    if os.environ.get("DELLMONITOR_HERMES_TOKEN"):
        cfg.hermes.token = os.environ["DELLMONITOR_HERMES_TOKEN"]
    if os.environ.get("DELLMONITOR_CLAUDE_PROJECTS"):
        cfg.claude.projects_root = os.environ["DELLMONITOR_CLAUDE_PROJECTS"]
    if os.environ.get("DELLMONITOR_HOOK_PORT"):
        cfg.claude.hook_port = int(os.environ["DELLMONITOR_HOOK_PORT"])
    return cfg


def resolve_secret(value: str | None) -> str | None:
    """Aceita um segredo literal ou ``file:<caminho>``."""
    if not value:
        return None
    value = str(value).strip()
    if value.startswith("file:"):
        path = os.path.expanduser(value[5:])
        with open(path, encoding="utf-8") as f:
            return f.read().strip() or None
    return value


def ensure_device_token(cfg: Config, path: str = "~/.dellmonitor/device_token") -> tuple[str, bool]:
    """Garante um token de pareamento. Devolve (token, gerado_agora)."""
    token = resolve_secret(cfg.device.token)
    if token:
        if len(token) < 12:
            raise ValueError("device.token precisa ter pelo menos 12 caracteres")
        return token, False
    import secrets

    p = os.path.expanduser(path)
    if os.path.exists(p):
        with open(p, encoding="utf-8") as f:
            token = f.read().strip()
        if len(token) >= 12:
            cfg.device.token = token
            return token, False
    os.makedirs(os.path.dirname(p), exist_ok=True)
    token = secrets.token_urlsafe(12)  # 16 caracteres, ~96 bits
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(token + "\n")
    cfg.device.token = token
    return token, True

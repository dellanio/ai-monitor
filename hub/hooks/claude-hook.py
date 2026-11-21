#!/usr/bin/env python3
"""Claude Code hook -> hub-stack-chan (AI Monitor) (stdlib only, no dependencies).

Registrado em ~/.claude/settings.json (ver ../README.md).

* ``PreToolUse``, ``Stop``, ``SessionStart``, ``Notification``: só avisam o
  Hub (status na tela). Timeout curto e nenhuma decisão impressa.
* ``PermissionRequest``: só dispara quando o Claude Code ia perguntar. Se o
  Hub tiver ``device.allow_actions`` ligado, espera o toque no aparelho e
  imprime ``allow``/``deny``. Em qualquer outro caso (Hub fora do ar,
  modo só leitura, timeout) sai em silêncio e o prompt normal continua.
"""
import json
import os
import sys
import urllib.request

HUB_URL = os.environ.get("DELLMONITOR_HOOK_URL", "http://127.0.0.1:8767/hooks/claude")
# Maior que o approval.timeout_s do Hub (padrão 120 s).
PERMISSION_TIMEOUT = int(os.environ.get("DELLMONITOR_HOOK_TIMEOUT", "150"))
NOTIFY_TIMEOUT = 2


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except Exception:
        return 0
    event = payload.get("hook_event_name", "")
    timeout = PERMISSION_TIMEOUT if event == "PermissionRequest" else NOTIFY_TIMEOUT
    try:
        req = urllib.request.Request(
            HUB_URL,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            result = json.load(resp)
    except Exception:
        return 0
    if event != "PermissionRequest":
        return 0
    decision = result.get("decision")
    if decision not in ("allow", "deny"):
        return 0  # sem decisão: o Claude Code mostra o prompt normal
    behavior = {"behavior": decision}
    if decision == "deny":
        behavior["message"] = "Negado no AI Monitor."
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest",
                                             "decision": behavior}}))
    return 0


if __name__ == "__main__":
    sys.exit(main())

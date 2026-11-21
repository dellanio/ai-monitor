"""Host resource stats for the AI Monitor device (PC side).

Best-effort collectors for the "COMPUTADOR" screen: CPU/RAM (always,
via psutil), GPU (``nvidia-smi`` only; None without an NVIDIA GPU),
top process by CPU, network up/down rates, battery. Anything unavailable
comes back as None and the firmware shows "--" instead of a number.
"""
from __future__ import annotations

import subprocess
import time

try:
    import psutil
except ImportError:  # Hub still runs without it; stats are just off.
    psutil = None


class HostStats:
    """Collects one sample per call. Pass ``psutil_mod=...`` in tests
    to inject a stub instead of the real psutil."""

    def __init__(self, psutil_mod=None, gpu_cmd: str = "nvidia-smi") -> None:
        self._psutil = psutil_mod if psutil_mod is not None else psutil
        self._gpu_cmd = gpu_cmd
        self._gpu_checked = False
        self._gpu_available = False
        self._prev_net = None  # (bytes_recv, bytes_sent, monotonic)
        self._clock = time.monotonic  # override in tests for stable rates
        if self._psutil is not None:
            self._prime_processes()

    @property
    def available(self) -> bool:
        return self._psutil is not None

    def _prime_processes(self) -> None:
        # Process.cpu_percent() only means something after a previous
        # call, so warm it up once here and throw the values away.
        try:
            for proc in self._psutil.process_iter(["cpu_percent"]):
                try:
                    proc.cpu_percent()
                except Exception:
                    pass
        except Exception:
            pass

    def collect(self) -> dict | None:
        """One sample, or None when psutil is unavailable."""
        if self._psutil is None:
            return None
        cpu = self._psutil.cpu_percent(interval=None)
        mem = self._psutil.virtual_memory().percent
        top_name, top_pct = self._top_process()
        down, up = self._net_rates()
        batt_pct, batt_mins = self._battery()
        return {
            "host_cpu_pct": int(round(cpu)),
            "host_mem_pct": int(round(mem)),
            "host_gpu_pct": self._gpu(),
            "host_top_name": top_name,
            "host_top_pct": top_pct,
            "host_net_down_mbps": down,
            "host_net_up_mbps": up,
            "host_batt_pct": batt_pct,
            "host_batt_mins": batt_mins,
        }

    # -- pieces ----------------------------------------------------------
    def _top_process(self):
        best_name, best_pct = None, None
        try:
            for proc in self._psutil.process_iter(["name", "cpu_percent"]):
                try:
                    pct = proc.cpu_percent()
                    name = proc.info.get("name") or "?"
                except Exception:
                    continue
                if pct is None:
                    continue
                if best_pct is None or pct > best_pct:
                    best_pct, best_name = pct, name
        except Exception:
            return None, None
        if best_name is None:
            return None, None
        return best_name[:20], int(round(best_pct))

    def _net_rates(self):
        try:
            counters = self._psutil.net_io_counters()
            now = self._clock()
            cur = (counters.bytes_recv, counters.bytes_sent, now)
            if self._prev_net is None:
                self._prev_net = cur
                return 0.0, 0.0
            prev_recv, prev_sent, prev_t = self._prev_net
            self._prev_net = cur
            dt = now - prev_t
            if dt <= 0:
                return 0.0, 0.0
            down = (cur[0] - prev_recv) / dt / 1e6
            up = (cur[1] - prev_sent) / dt / 1e6
            return round(max(down, 0.0), 1), round(max(up, 0.0), 1)
        except Exception:
            return None, None

    def _battery(self):
        try:
            batt = self._psutil.sensors_battery()
        except Exception:
            return None, None
        if batt is None:
            return None, None
        pct = int(round(batt.percent))
        mins = None
        if batt.secsleft is not None and batt.secsleft > 0:
            mins = int(batt.secsleft // 60)
        return pct, mins

    def _gpu(self):
        if not self._gpu_checked:
            self._gpu_checked = True
            self._gpu_available = self._probe_gpu() is not None
            if not self._gpu_available:
                return None
        if not self._gpu_available:
            return None
        return self._probe_gpu()

    def _probe_gpu(self):
        try:
            out = subprocess.run(
                [self._gpu_cmd, "--query-gpu=utilization.gpu",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=5,
            )
            if out.returncode != 0:
                return None
            return int(out.stdout.strip().splitlines()[0].strip())
        except Exception:
            return None

#!/usr/bin/env python3
"""Export one local Mooncake master's capacity via node_exporter textfile.

Linux CLI, Python standard library only. No server, credentials or user content.
"""

import argparse
from decimal import Decimal, InvalidOperation
import http.client
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


POOL = "deepseek-v41-usw5"
MAX_RESPONSE = 1 << 20
METRICS = {
    "master_total_capacity_bytes": ("registered_bytes", "dram"),
    "master_total_file_capacity_bytes": ("registered_bytes", "ssd"),
    "master_allocated_bytes": ("allocated_bytes", "dram"),
    "master_allocated_file_size_bytes": ("allocated_bytes", "ssd"),
    "master_key_count": ("keys", None),
}
NAME = re.compile(r"^([a-zA-Z_:][a-zA-Z0-9_:]*)")
VALUE = re.compile(
    r"^[a-zA-Z_:][a-zA-Z0-9_:]*[ \t]+([+\-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+\-]?\d+)?)[ \t]*$"
)


def parse_metrics(payload):
    if len(payload) > MAX_RESPONSE:
        raise ValueError("oversized response")
    found = {}
    for line in payload.decode("utf-8", errors="strict").splitlines():
        line = line.strip()
        name_match = NAME.match(line)
        if not name_match or name_match[1] not in METRICS:
            continue
        name = name_match[1]
        match = VALUE.fullmatch(line)
        if name in found or match is None:
            raise ValueError("duplicate or invalid metric")
        try:
            value = Decimal(match[1])
            if (
                not value.is_finite()
                or value < 0
                or value > 2**63 - 1
                or value != value.to_integral_value()
            ):
                raise ValueError("invalid gauge value")
            found[name] = int(value)
        except InvalidOperation as exc:
            raise ValueError("invalid gauge value") from exc
    if found.keys() != METRICS.keys():
        raise ValueError("missing metric")
    return found


def fetch_metrics(container):
    # The explicit local socket prevents DOCKER_HOST or contexts redirecting us.
    result = subprocess.run(
        [
            "/usr/bin/docker",
            "--host",
            "unix:///var/run/docker.sock",
            "inspect",
            "--format",
            "{{json .NetworkSettings.Networks}}",
            container,
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=3,
    )
    networks = json.loads(result.stdout)
    if not isinstance(networks, dict) or not all(
        isinstance(n, dict) for n in networks.values()
    ):
        raise ValueError("invalid container networks")
    addresses = [n["IPAddress"] for n in networks.values() if n.get("IPAddress")]
    if len(addresses) != 1:
        raise ValueError("expected one container IPv4 network")
    address = ipaddress.IPv4Address(addresses[0])
    if not address.is_private or address.is_loopback or address.is_unspecified:
        raise ValueError("expected private container IPv4")
    connection = http.client.HTTPConnection(str(address), 9003, timeout=3)
    try:
        connection.request("GET", "/metrics", headers={"Accept": "text/plain"})
        response = connection.getresponse()
        if response.status != 200:
            raise ValueError("HTTP status")
        length = response.getheader("Content-Length")
        if length is not None and (int(length) < 0 or int(length) > MAX_RESPONSE):
            raise ValueError("oversized response")
        payload = response.read(MAX_RESPONSE + 1)
        if len(payload) > MAX_RESPONSE:
            raise ValueError("oversized response")
        return payload
    finally:
        connection.close()


def render_metrics(values, now):
    if not math.isfinite(now) or now < 0:
        raise ValueError("invalid clock")
    prefix = "mooncake_shared_cache_"
    labels = f'pool="{POOL}"'
    lines = [
        f"# TYPE {prefix}collection_success gauge",
        f"{prefix}collection_success{{{labels}}} {int(values is not None)}",
        f"# TYPE {prefix}collection_timestamp_seconds gauge",
        f"{prefix}collection_timestamp_seconds{{{labels}}} {now:.6f}",
    ]
    if values is not None:
        for family in ("registered_bytes", "allocated_bytes", "keys"):
            lines.append(f"# TYPE {prefix}{family} gauge")
            for raw_name, (name, tier) in METRICS.items():
                if name != family:
                    continue
                metric_labels = labels + (f',tier="{tier}"' if tier else "")
                lines.append(f"{prefix}{name}{{{metric_labels}}} {values[raw_name]}")
    return "\n".join(lines) + "\n"


def atomic_write(output, text):
    # Same-directory replacement: node_exporter sees a complete old or new file.
    # Temporary suffix is not .prom, so it is never scraped while being written.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=output.parent,
            prefix=".mooncake-capacity-",
            suffix=".tmp",
            delete=False,
        ) as handle:
            temporary = Path(handle.name)
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def collect_once(container, output):
    try:
        values = parse_metrics(fetch_metrics(container))
    except (OSError, ValueError, subprocess.SubprocessError, http.client.HTTPException):
        values = None
    # Cancel the CLI's whole-collection deadline before writing the failure file.
    # main owns this timer; library/unit-test callers need no signal support.
    if hasattr(signal, "SIGALRM"):
        signal.alarm(0)
    text = render_metrics(values, time.time())
    try:
        atomic_write(output, text)
    except OSError:
        print("mooncake capacity: output write failed", file=sys.stderr)
        return 2
    if values is None:
        print("mooncake capacity: collection failed", file=sys.stderr)
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_.-]{0,127}", args.container):
        parser.error("invalid container name")
    if args.output.suffix != ".prom" or not args.output.is_absolute():
        parser.error("output must be an absolute .prom path")
    if not hasattr(signal, "SIGALRM"):
        parser.error("Linux SIGALRM is required for the total collection deadline")

    def deadline(signum, frame):
        raise TimeoutError("collection deadline")

    signal.signal(signal.SIGALRM, deadline)
    signal.alarm(8)  # Total docker+HTTP deadline, including slow-drip bodies.
    return collect_once(args.container, args.output)


if __name__ == "__main__":
    sys.exit(main())

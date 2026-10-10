# Mooncake shared-cache capacity textfile collector

Exports five allowlisted totals from one local Docker master to an existing
node_exporter textfile collector. Python standard library only; no new port,
model-service token, request content, GPU access or engine aggregation. This
deployment example represents **one** pool shared by four TP2 services; never
sum four copies of it.

The example fixes pool `deepseek-v41-usw5`, container
`mooncake-master-dsv41-sglang-tp2`, master metrics port 9003, and textfile directory
`/var/lib/prometheus/node-exporter`. Each run resolves only that container's
single private IPv4 using the local Docker socket, so a container recreation
does not leave a stale IP. Multiple attached IPv4 networks fail closed instead
of choosing an arbitrary endpoint. No complete Docker inspect or metrics body
is logged or forwarded. Root is used for this read-only Docker lookup and the
root-owned textfile directory; the service has no Docker mutation code.

## Metrics and failure behavior

| Source | Output | Fixed tier |
|---|---|---|
| `master_total_capacity_bytes` | `mooncake_shared_cache_registered_bytes` | `dram` |
| `master_total_file_capacity_bytes` | `mooncake_shared_cache_registered_bytes` | `ssd` |
| `master_allocated_bytes` | `mooncake_shared_cache_allocated_bytes` | `dram` |
| `master_allocated_file_size_bytes` | `mooncake_shared_cache_allocated_bytes` | `ssd` |
| `master_key_count` | `mooncake_shared_cache_keys` | none |

All outputs carry only fixed `pool="deepseek-v41-usw5"`, with `tier` where shown.
`mooncake_shared_cache_collection_success` is 1 only when all five unique,
unlabeled input gauges are valid nonnegative integers. Missing/duplicate/labeled,
nonfinite, negative, fractional or out-of-range inputs fail the whole sample.
An observed zero capacity is valid and must trigger the capacity alert.

`mooncake_shared_cache_collection_timestamp_seconds` is the completion time of
the collection attempt, including failure. A failed read/parse atomically
replaces the file with **only success=0 and its timestamp**: no capacity gauges,
old capacities, or fabricated healthy zeros. Exit codes: 0 success, 1 collection
failure, 2 output-write failure. On output failure the old complete file may
remain, with its original timestamp; the stale-data alert is mandatory. A fresh
node_exporter HTTP scrape does not make an old textfile fresh. Dashboards should
gate capacities on success=1 and collection age <=90 seconds.

HTTP connect/read timeout is 3 seconds, Docker inspect timeout 3 seconds,
whole collection deadline 8 seconds (Linux SIGALRM), response limit 1 MiB, and
systemd hard service limit 12 seconds. Temporary files use `.tmp`, never `.prom`;
fsync plus same-directory `os.replace` publishes a complete file with mode 0644.
The 30-second timer serializes its oneshot service; do not run a second collector
for the same output. No historical sample state or retry framework is needed.

## Test and install

Run CPU unit tests from this directory:

```sh
python3 -m unittest -v test_mooncake_capacity_textfile.py
```

Before installation, publish this source/configuration in `Phala-Network/Mooncake`
at a fixed commit and record that full commit in the deployment evidence. Use a
checkout of that published commit, not a working-tree overlay or mutable main.
Installation below is an operator action, separate from preparing this source.

1. Verify the existing node_exporter help/argv and textfile collector health;
   this example uses its existing default directory and needs **no exporter
   config change or restart**. Confirm the named Docker container, its one IPv4,
   and all five metrics. Keep the current private metrics port unexposed.
2. Preserve existing files at these exact destinations when upgrading. Verify
   the new script/unit hashes against the published commit. Run unit tests and
   `systemd-analyze verify` on the candidate service/timer. Then install:

```sh
# Run from phala/monitoring at the recorded, published commit.
sudo install -d -m 0755 /opt/mooncake-capacity
sudo install -m 0644 mooncake_capacity_textfile.py /opt/mooncake-capacity/
sudo install -m 0644 mooncake-capacity.service mooncake-capacity.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl start mooncake-capacity.service
sudo systemctl enable --now mooncake-capacity.timer
```

3. Verify service exit status, timer schedule, owned `.prom` content and the
   seven exported samples through existing node_exporter. Then verify the same
   exact pool/tier values and timestamps in the existing Prometheus/Grafana
   datasource. Registered capacities should be 1040 GiB and 27 TiB for this
   deployment; allocated/keys are time-varying, not fixed acceptance constants.
   `node_textfile_scrape_error` must stay 0. Confirm old node metrics still flow.

For rollback, stop/disable only `mooncake-capacity.timer`, stop its oneshot if
active, remove only its owned `/var/lib/prometheus/node-exporter/mooncake-capacity.prom`,
and restore the backed-up collector/units (or remove the two units on first
install), followed by daemon-reload. Do not remove the shared directory, other
textfiles, restart Docker/node_exporter, touch Mooncake containers, or change PIG.
If restoring an earlier collector, start it once before enabling its timer so
an old backup `.prom` is not presented as a new sample. Missing monitoring during
rollback is intentionally detectable; reuse normal maintenance notification
handling rather than fabricating health.

## Grafana alerts on existing node_exporter scrape

Datasource example: `ZGYO8utnk`. Reuse the existing notification policy; no new
receiver or test notification is required. These expressions return numeric
0/1 for Grafana threshold `> 0`. Do not use `success != 1` directly as the numeric
alert condition: its failure sample is 0. Do not `or` same-label bool predicates,
because a left-side 0 masks a right-side 1. The health rule uses a count of
filtered failures and an explicit zero for the empty set.

Capacity below target, pending period **2 minutes**:

```promql
max(
  (mooncake_shared_cache_registered_bytes{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5",tier="dram"} < bool 1116691496960)
  or
  (mooncake_shared_cache_registered_bytes{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5",tier="ssd"} < bool 29686813949952)
)
```

Collection failed/stale/missing, pending period **1 minute**:

```promql
(
  count(
    (mooncake_shared_cache_collection_success{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5"} != 1)
    or (time() - mooncake_shared_cache_collection_timestamp_seconds{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5"} > 90)
    or absent(mooncake_shared_cache_collection_success{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5"})
    or absent(mooncake_shared_cache_collection_timestamp_seconds{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5"})
    or absent(mooncake_shared_cache_registered_bytes{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5",tier="dram"})
    or absent(mooncake_shared_cache_registered_bytes{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts",pool="deepseek-v41-usw5",tier="ssd"})
    or (up{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts"} != 1)
    or (node_textfile_scrape_error{job="node-exporter",instance="tdx-usw5.phala.systems",function="tdx-hosts"} != 0)
  ) or vector(0)
) > bool 0
```

Treat datasource query errors as Alerting using the existing policy. Capacity
No Data is covered by the dedicated health rule; do not force missing capacity
to 0 or treat it as healthy. Staleness is 90 seconds plus the rule's 1-minute
pending window and scrape/evaluation latency. These are scrape-operational
thresholds, not a serving SLA. A future clock timestamp should be investigated
as host clock skew; normal host time synchronization remains necessary.

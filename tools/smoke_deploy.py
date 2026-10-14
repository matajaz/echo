#!/usr/bin/env python3
"""End-to-end OTA deploy smoke test against a REAL board.

This is the integration check the host unit tests cannot provide: it publishes
the built image to the firmware host and triggers the board's /ota endpoint
over the wire, then confirms the board rebooted into the exact ELF identity.
It exercises the one path mocked unit tests skip -- the client <-> nginx <->
firmware contract (the url-encoding mismatch that broke every wireless deploy
lived exactly here).

OPT-IN: with no SMOKE_BOARD set it prints SKIP and exits 0, so it is safe to
call from a host test runner that has no hardware.

Usage:
    SMOKE_BOARD=192.168.2.198 python3 tools/smoke_deploy.py

Env (all optional except SMOKE_BOARD to actually run):
    SMOKE_BOARD   board IP/host (unset -> SKIP)
    SMOKE_HOST    firmware/nginx host     (default 192.168.2.7)
    SMOKE_USER    ssh user on the host    (default mathias)
    SMOKE_DIR     firmware dir on host    (default ~/vegena-ota/firmware/echo)
    SMOKE_BIN     built image             (default build/echo.bin)
    SMOKE_SECRETS secrets header          (default secrets/secrets.h)
    SMOKE_TIMEOUT confirm window seconds   (default 240)
"""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('maintenance', HERE / 'maintenance.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def main():
    board = os.environ.get('SMOKE_BOARD')
    if not board:
        print('SKIP: smoke_deploy needs SMOKE_BOARD=<board ip> and real hardware')
        return 0

    bin_path = os.environ.get('SMOKE_BIN', str(HERE.parent / 'build' / 'echo.bin'))
    if not Path(bin_path).exists():
        print('SKIP: no built image at ' + bin_path + ' (run idf.py build first)')
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        ledger = str(Path(tmp) / 'smoke-ledger.jsonl')
        args = SimpleNamespace(
            command='deploy', bin=bin_path, project='echo',
            secrets=os.environ.get('SMOKE_SECRETS', str(HERE.parent / 'secrets' / 'secrets.h')),
            prefix_define='OTA_URL_PREFIX', key_define='OTA_KEY', key_env=None,
            board=board, host=os.environ.get('SMOKE_HOST', '192.168.2.7'),
            user=os.environ.get('SMOKE_USER', 'mathias'),
            directory=os.environ.get('SMOKE_DIR', '~/vegena-ota/firmware/echo'),
            ca=os.environ.get('OTA_CA'),
            timeout=float(os.environ.get('SMOKE_TIMEOUT', '240')),
            interval=5, soak=4, ledger=ledger,
        )
        try:
            m.deploy(args)
        except Exception as error:  # maintenance raises on any failure
            print('FAIL: deploy did not confirm (' + type(error).__name__ + ')')
            _print_ledger(ledger)
            return 1

        record = _last_record(ledger)
        if not record or record.get('result') != 'success':
            print('FAIL: ledger did not record success')
            _print_ledger(ledger)
            return 1
        print('PASS: board confirmed running image ' + str(record.get('app_sha'))[:16]
              + ' (boot ' + str(record.get('from_boot_id')) + ' -> '
              + str(record.get('to_boot_id')) + ')')
        return 0


def _last_record(ledger):
    try:
        lines = [l for l in Path(ledger).read_text().splitlines() if l.strip()]
        return json.loads(lines[-1]) if lines else None
    except (OSError, ValueError):
        return None


def _print_ledger(ledger):
    record = _last_record(ledger)
    if record:
        print('  ledger: result=' + str(record.get('result'))
              + ' http_status=' + str(record.get('http_status'))
              + ' reason=' + str(record.get('reason'))
              + ' error=' + str(record.get('error_type')))


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Board-independent log protocol and exact-image pull deployment client.

Kept identical in Echo and Vegena so each repository remains self-contained.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import ssl
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class MaintenanceError(ValueError):
    """A fixed diagnostic that is safe to print without a URL or credential."""

def parse_log(text):
    lines = text.splitlines()
    boot = None
    cursor = None
    i = 0
    if lines and lines[0].startswith('boot: '):
        boot = int(lines[0][6:])
        if not 0 < boot <= 0xffffffff:
            raise MaintenanceError('invalid boot ID')
        i += 1
    if i >= len(lines) or not re.fullmatch(r'id: [0-9]+', lines[i]):
        raise MaintenanceError('missing log cursor')
    cursor = int(lines[i][4:])
    if not 0 <= cursor <= 0xffffffff:
        raise MaintenanceError('invalid cursor')
    return boot, cursor, '\n'.join(lines[i + 1:])


def image_identity(data):
    # ESP application image header + first segment header precede esp_app_desc_t.
    # Validate the expected offset instead of searching arbitrary image bytes.
    if len(data) < 208 or data[0] != 0xe9 or struct.unpack_from('<I', data, 32)[0] != 0xabcd5432:
        raise MaintenanceError('not an ESP application image with an app descriptor')
    digest = data[32 + 144:32 + 176].hex()
    if digest == '0' * 64:
        raise MaintenanceError('image has no ELF identity')
    return digest


def accepted(info, expected, baseline):
    # No reachability/version heuristic: demand a new boot and the exact ELF.
    boot = info.get('boot_id')
    return (type(boot) is int and boot != 0 and boot != baseline
            and info.get('app_sha_kind') == 'elf_sha256'
            and info.get('app_sha') == expected
            and info.get('img_state') == 'valid'
            and info.get('ota_in_progress') is False)


def fetch(url, timeout=5, ca=None, limit=8 * 1024 * 1024):
    context = ssl.create_default_context(cafile=ca)
    request = urllib.request.Request(url, headers={'Cache-Control': 'no-cache'})
    # Do not follow redirects: verify and trigger the configured endpoint only.
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None
    opener = urllib.request.build_opener(NoRedirect(), urllib.request.HTTPSHandler(context=context))
    with opener.open(request, timeout=timeout) as response:
        started = time.monotonic()
        data = bytearray()
        while True:
            chunk = response.read1(min(65536, limit + 1 - len(data)))
            if not chunk:
                return bytes(data)
            data.extend(chunk)
            if len(data) > limit:
                raise MaintenanceError('HTTP response exceeded limit')
            if time.monotonic() - started > timeout:
                raise MaintenanceError('HTTP response exceeded total deadline')


def info_at(board):
    result = json.loads(fetch('http://' + board + '/info', limit=16384))
    if not isinstance(result, dict):
        raise MaintenanceError('invalid info response')
    return result


def remote_path(path):
    if path.startswith('~/'):
        return '"$HOME"/' + shlex.quote(path[2:])
    if path.startswith('~'):
        raise MaintenanceError('only ~/ or absolute remote directories are supported')
    return shlex.quote(path)


def define(path, name):
    match = re.search(r'^\s*#define\s+' + re.escape(name) + r'\s+"([^"\n]+)"', Path(path).read_text(), re.M)
    if not match:
        raise MaintenanceError('missing configuration: ' + name)
    return match[1]


def deploy(args):
    record = {'ts': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'project': args.project,
              'board': args.board, 'result': 'failed'}
    started = time.monotonic()
    target = folder = None
    try:
        data = Path(args.bin).read_bytes()
        expected = image_identity(data)
        sha = hashlib.sha256(data).hexdigest()
        record.update(size=len(data), sha256=sha, app_sha=expected)
        baseline = info_at(args.board)
        boot = baseline.get('boot_id')
        if type(boot) is not int or not boot:
            # Vegena releases before the common /info contract expose boot ID here.
            boot = json.loads(fetch('http://' + args.board + '/api/v1/capabilities', limit=16384)).get('boot_id')
        if type(boot) is not int or not boot:
            raise MaintenanceError('cannot establish pre-trigger boot identity')
        record['from_boot_id'] = boot
        prefix = define(args.secrets, args.prefix_define)
        key = os.environ.get(args.key_env) if args.key_env else None
        if not key:
            key = define(args.secrets, args.key_define)
        parts = urllib.parse.urlsplit(prefix)
        if parts.scheme not in ('http', 'https') or not parts.netloc or not prefix.endswith('/') or parts.query or parts.fragment or parts.username or parts.password:
            raise MaintenanceError('invalid firmware URL prefix')
        filename = sha + '.bin'
        url = prefix + filename
        target = args.user + '@' + args.host
        folder = remote_path(args.directory)
        # Content-addressed final name + unique upload temporary file.
        temporary = filename + '.tmp.' + str(os.getpid()) + '.' + str(time.time_ns())
        subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', target,
                        'mkdir -p ' + folder], check=True, timeout=15)
        subprocess.run(['scp', '-o', 'BatchMode=yes', args.bin,
                        target + ':' + args.directory.rstrip('/') + '/' + temporary], check=True, timeout=120)
        subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', target,
                        'mv ' + folder + '/' + shlex.quote(temporary) + ' ' + folder + '/' + filename], check=True, timeout=15)
        served = fetch(url, timeout=20, ca=args.ca)
        if hashlib.sha256(served).hexdigest() != sha:
            raise MaintenanceError('served image does not match build')
        record['firmware_url'] = url
        response = fetch('http://' + args.board + '/ota?' + urllib.parse.urlencode({'key': key, 'url': url}), timeout=15, limit=4096)
        if b'OTA queued' not in response:
            raise MaintenanceError('device did not acknowledge OTA queue')
        deadline = time.monotonic() + args.timeout
        consecutive = 0
        soak_boot = None
        while time.monotonic() < deadline:
            time.sleep(args.interval)
            try:
                current = info_at(args.board)
                if accepted(current, expected, boot):
                    consecutive = consecutive + 1 if current['boot_id'] == soak_boot else 1
                    soak_boot = current['boot_id']
                else:
                    consecutive, soak_boot = 0, None
            except (OSError, ValueError, urllib.error.URLError):
                consecutive, soak_boot = 0, None
                continue
            if consecutive >= args.soak:
                record.update(result='success', to_boot_id=current['boot_id'], fw=current.get('fw'))
                print('Verified new boot running confirmed image ' + expected)
                return
        raise MaintenanceError('device did not confirm the expected image within the deployment window')
    except Exception as error:
        record['error_type'] = type(error).__name__
        if isinstance(error, MaintenanceError): record['reason'] = str(error)
        if isinstance(error, urllib.error.HTTPError):
            record['http_status'] = error.code
            error.close()
        raise
    finally:
        record['elapsed_s'] = round(time.monotonic() - started, 1)
        line = json.dumps(record, separators=(',', ':')) + '\n'
        with Path(args.ledger).open('a') as log:
            log.write(line)
        if target and folder:
            try:
                subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', target,
                                'cat >> ' + folder + '/deploy-log.jsonl'], input=line, text=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            except (OSError, subprocess.SubprocessError):
                print('Server ledger unavailable; local ledger saved', file=sys.stderr)


def collect(args):
    cursor = 0
    boot = None
    poll = 0
    output = sys.stdout if args.output == '-' else open(args.output, 'a', buffering=1)
    try:
        while True:
            try:
                current_boot, newest, payload = parse_log(fetch('http://' + args.board + '/log.txt?since=' + str(cursor), limit=65536).decode())
                if (boot is not None and current_boot != boot) or newest < cursor:
                    output.write('--- board reboot / log reset ---\n')
                    # A reboot may already have more lines than the old cursor.
                    current_boot, newest, payload = parse_log(fetch('http://' + args.board + '/log.txt?since=0', limit=65536).decode())
                if payload:
                    output.write(payload + '\n')
                    output.flush()
                boot, cursor = current_boot, newest
                if args.info_every and poll % args.info_every == 0 and args.output != '-':
                    with open(args.output + '.info.jsonl', 'a') as log:
                        log.write(json.dumps({'ts': time.time(), 'info': info_at(args.board)}) + '\n')
            except (OSError, ValueError, urllib.error.URLError) as error:
                print('Collection retry: ' + type(error).__name__, file=sys.stderr)
            poll += 1
            time.sleep(args.interval)
    finally:
        if output is not sys.stdout:
            output.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('deploy')
    for name in ('bin', 'project', 'secrets', 'prefix-define', 'key-define', 'board', 'host', 'user', 'directory'):
        p.add_argument('--' + name, required=True)
    p.add_argument('--key-env')
    p.add_argument('--ca', default=os.environ.get('OTA_CA'))
    p.add_argument('--ledger', default='deploy-log.jsonl')
    p.add_argument('--timeout', type=float, default=240)
    p.add_argument('--interval', type=float, default=5)
    p.add_argument('--soak', type=int, default=4)
    p = commands.add_parser('collect')
    p.add_argument('board')
    p.add_argument('output')
    p.add_argument('--interval', type=float, default=5)
    p.add_argument('--info-every', type=int, default=12)
    args = parser.parse_args()
    if args.interval <= 0 or (args.command == 'deploy' and (args.soak < 1 or args.timeout <= 0)):
        parser.error('interval, timeout and soak must be positive')
    try:
        deploy(args) if args.command == 'deploy' else collect(args)
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, subprocess.SubprocessError, urllib.error.URLError) as error:
        # Exception URLs can contain the OTA key; report type/status, never URL.
        print('Maintenance failed: ' + (str(error) if isinstance(error, MaintenanceError) else type(error).__name__) +
              (' HTTP ' + str(error.code) if isinstance(error, urllib.error.HTTPError) else ''), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())

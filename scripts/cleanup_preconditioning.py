#!/usr/bin/env python3
"""One-time, manifest-scoped legacy preconditioning cleanup. Never starts/stops charging."""

import argparse
import base64
from email.utils import parsedate_to_datetime
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import ssl
import stat
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, HTTPSHandler, ProxyHandler, Request, build_opener


STATE = Path('/var/tmp/tesla-cron')
MAX_ID = 9007199254740991
INTERVAL = 4.0  # Tesla-bound requests, including reads/wake; local /health is exempt


def private_text(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise RuntimeError('Cleanup files must be private regular files owned by the running user')
    return path.read_text()


def private_json(path):
    return json.loads(private_text(path))


def atomic_text(path, text):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix='.cleanup-', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def schedule(item):
    if not isinstance(item, dict):
        raise RuntimeError('Malformed preconditioning schedule')
    result = {}
    for key, maximum in (('id', MAX_ID), ('precondition_time', 1439), ('days_of_week', 127)):
        value = item.get(key, 0)
        if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum or (key == 'id' and not value):
            raise RuntimeError('Invalid preconditioning schedule ID/time')
        result[key] = value
    for key, maximum in (('latitude', 90), ('longitude', 180)):
        value = item.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or abs(value) > maximum:
            raise RuntimeError('Invalid preconditioning schedule location')
        result[key] = float(value) if value else 0.0
    for key in ('enabled', 'one_time'):
        value = item.get(key, False)
        if not isinstance(value, bool):
            raise RuntimeError('Invalid preconditioning schedule flag')
        result[key] = value
    return result


def schedules(payload, vin):
    response = payload.get('response') if isinstance(payload, dict) else None
    if not isinstance(response, dict) or (response.get('vin', vin) != vin):
        raise RuntimeError('Invalid vehicle response or VIN mismatch')
    section = response.get('preconditioning_schedule_data')
    if not isinstance(section, dict) or not isinstance(section.get('precondition_schedules'), list):
        raise RuntimeError('Preconditioning schedule data missing; refusing cleanup')
    result = {}
    for item in section['precondition_schedules']:
        normalized = schedule(item)
        if normalized['id'] in result:
            raise RuntimeError('Duplicate preconditioning schedule IDs')
        result[normalized['id']] = normalized
    return result


def prepare(cache, destination, expected):
    payload = private_json(cache)
    vin = payload.get('response', {}).get('vin', '')
    if not re.fullmatch(r'[A-Z0-9]{17}', vin):
        raise RuntimeError('Invalid VIN in reviewed cache')
    entries = list(schedules(payload, vin).values())
    if len(entries) != expected or any(not item['enabled'] or item['one_time'] for item in entries):
        raise RuntimeError('Cache no longer matches the approved enabled recurring schedule count')
    manifest = {'version': 1, 'vin': vin, 'snapshot_time': payload.get('tesla_cron_fetched_at'),
                'schedules': sorted(entries, key=lambda item: item['id'])}
    descriptor = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o400)
    with os.fdopen(descriptor, 'w') as stream:
        json.dump(manifest, stream, sort_keys=True)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    print(f'Prepared immutable manifest for {len(entries)} reviewed schedules. No network requests made.', flush=True)


def load_manifest(path, expected):
    manifest = private_json(path)
    if not isinstance(manifest, dict) or manifest.get('version') != 1 or not re.fullmatch(r'[A-Z0-9]{17}', manifest.get('vin', '')):
        raise RuntimeError('Invalid cleanup manifest')
    entries = manifest.get('schedules')
    if not isinstance(entries, list) or len(entries) != expected:
        raise RuntimeError('Manifest count differs from the explicit approved count')
    normalized = [schedule(item) for item in entries]
    approved = {item['id']: item for item in normalized}
    if len(approved) != expected:
        raise RuntimeError('Duplicate IDs in cleanup manifest')
    digest = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    return manifest['vin'], approved, digest


def pending_ids(approved, present):
    def same(reviewed, current):
        # RapidJSON's cached double formatting and Python's decoder can differ
        # by a few ULPs. This tolerance is far below a millimetre, not a geofence.
        return all(math.isclose(reviewed[key], current[key], rel_tol=0, abs_tol=1e-10)
                   if key in ('latitude', 'longitude') else reviewed[key] == current[key]
                   for key in reviewed)

    changed = [identity for identity in approved if identity in present and not same(approved[identity], present[identity])]
    if changed:
        raise RuntimeError('Reviewed schedules changed since approval; refusing to delete any changed entries')
    return sorted(set(approved) & set(present))


class Journal:
    def __init__(self, path, digest):
        self.path = path
        self.data = private_json(path) if path.exists() or path.is_symlink() else {'manifest': digest, 'resolved': [], 'last_request': 0, 'wake_sent': False}
        if (not isinstance(self.data, dict) or self.data.get('manifest') != digest
                or not isinstance(self.data.get('resolved'), list)
                or any(isinstance(identity, bool) or not isinstance(identity, int) or not 0 < identity <= MAX_ID for identity in self.data['resolved'])
                or len(set(self.data['resolved'])) != len(self.data['resolved'])
                or not isinstance(self.data.get('last_request'), (int, float))
                or isinstance(self.data.get('last_request'), bool)
                or not math.isfinite(self.data['last_request']) or self.data['last_request'] < 0
                or not isinstance(self.data.get('wake_sent'), bool)):
            raise RuntimeError('Invalid cleanup journal or manifest changed')
        self.save()

    def save(self):
        atomic_text(self.path, json.dumps(self.data, sort_keys=True) + '\n')

    def reserve_request(self):
        delay = self.data['last_request'] + INTERVAL - time.time()
        if delay > 60:
            raise RuntimeError('Clock moved backwards; cleanup paused without requests')
        if delay > 0:
            time.sleep(delay)
        self.data['last_request'] = time.time()
        self.save()  # reserve durably before every possible request, including failures


class HTTPFailure(RuntimeError):
    def __init__(self, status, source):
        super().__init__(f'{source} HTTP {status}; cleanup stopped without retry')
        self.status = status
        self.source = source


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *_args, **_keywords):
        return None  # never replay a signed operation or forward Authorization to a redirect


class API:
    def __init__(self, state, journal):
        self.state, self.journal = state, journal
        self.audience = os.environ.get('AUDIENCE', 'https://fleet-api.prd.eu.vn.cloud.tesla.com').rstrip('/')
        if self.audience not in ('https://fleet-api.prd.eu.vn.cloud.tesla.com', 'https://fleet-api.prd.na.vn.cloud.tesla.com'):
            raise RuntimeError('Unsupported Fleet API audience for cleanup')
        self.proxy = os.environ.get('TESLA_PROXY_URL', 'https://127.0.0.1:4443').rstrip('/')
        parsed = urlsplit(self.proxy)
        if (parsed.scheme != 'https' or parsed.hostname != '127.0.0.1' or parsed.username is not None or parsed.password is not None
                or parsed.path or parsed.query or parsed.fragment):
            raise RuntimeError('Cleanup proxy must be HTTPS on literal 127.0.0.1 with no URL extras')
        parsed.port  # validate the port before any requests
        local_context = ssl.create_default_context(cafile=os.environ.get('TESLA_PROXY_CA_FILE', '/usr/local/share/tesla-cron/proxy.crt'))
        self.local = build_opener(ProxyHandler({}), NoRedirect(), HTTPSHandler(context=local_context))
        self.fleet = build_opener(ProxyHandler({}), NoRedirect(), HTTPSHandler(context=ssl.create_default_context()))

    def token(self):
        path = self.state / 'access_token.txt'
        token = private_text(path).strip()
        try:
            payload = token.split('.')[1]
            expiry = json.loads(base64.urlsafe_b64decode(payload + '=' * (-len(payload) % 4)))['exp']
            valid = isinstance(expiry, (int, float)) and not isinstance(expiry, bool) and math.isfinite(expiry) and expiry > time.time() + 60
        except (ValueError, KeyError, IndexError, TypeError):
            valid = False
        if not valid:
            raise RuntimeError('Access token expired/unknown; refresh with tesla_cron --refresh-token, then resume this manifest')
        return token

    def health(self):
        # Deliberately unauthenticated and local: never exposes the bearer token.
        try:
            with self.local.open(self.proxy + '/health', timeout=3) as response:
                if response.status != 200 or response.read().strip() != b'OK':
                    raise RuntimeError('Local proxy health failed')
        except (URLError, OSError):
            raise RuntimeError('Local proxy TLS/health check failed; no vehicle commands sent') from None

    def call(self, path, body=None, local=False):
        cooldown = self.state / 'api-not-before'
        if cooldown.exists() or cooldown.is_symlink():
            try:
                not_before = int(private_text(cooldown).strip())
            except (ValueError, OSError):
                raise RuntimeError('Unreadable Tesla cooldown; refusing requests') from None
            if not_before < 0:
                raise RuntimeError('Invalid Tesla cooldown; refusing requests')
            if not_before > time.time():
                raise RuntimeError('Tesla cooldown active; cleanup paused without requests')
        token = self.token()
        self.journal.reserve_request()
        request = Request((self.proxy if local else self.audience) + path,
                          data=json.dumps(body).encode() if body is not None else None,
                          headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})
        try:
            with (self.local if local else self.fleet).open(request, timeout=60) as response:
                return json.load(response)
        except HTTPError as error:
            if error.code == 429:
                until = time.time() + 900
                headers = error.headers or {}
                for header in ('Retry-After', 'RateLimit-Reset'):
                    value = headers.get(header, '').strip()
                    try:
                        if value.isdigit():
                            number = int(value)
                            until = max(until, number if header == 'RateLimit-Reset' and number >= 1000000000 else time.time() + number)
                        elif header == 'Retry-After' and value:
                            until = max(until, parsedate_to_datetime(value).timestamp())
                    except (ValueError, TypeError, OverflowError):
                        pass
                atomic_text(cooldown, str(math.ceil(until)) + '\n')
            raise HTTPFailure(error.code, 'Proxy' if local else 'Fleet API') from None
        except (URLError, OSError, ValueError):
            raise RuntimeError('Ambiguous/invalid API response; cleanup stopped without retry. Resume only after a fresh state check.') from None

    def snapshot(self, vin):
        payload = self.call('/api/1/vehicles/' + vin + '/vehicle_data?endpoints=preconditioning_schedule_data')
        present = schedules(payload, vin)
        raw = payload['response']['preconditioning_schedule_data']['precondition_schedules']
        # Keep a private diagnostic snapshot so a failed comparison never requires
        # repeating an API read just to inspect field differences.
        atomic_text(Path(str(self.journal.path) + '.snapshot'), json.dumps({
            'vin': vin, 'checked_at': time.time(), 'schedules': list(present.values()),
            'field_presence': {key: sum(key in item for item in raw) for key in
                               ('id', 'precondition_time', 'days_of_week', 'latitude', 'longitude', 'enabled', 'one_time')},
        }, sort_keys=True) + '\n')
        return present

    def remove(self, vin, identity):
        result = self.call('/api/1/vehicles/' + vin + '/command/remove_precondition_schedule', {'id': identity}, local=True)
        if not isinstance(result, dict) or not isinstance(result.get('response'), dict) or result['response'].get('result') is not True:
            raise RuntimeError('Preconditioning removal was not confirmed; cleanup stopped without retry')


def execute(api, journal, vin, approved, allow_wake=False):
    journal.data['verified_complete'] = False
    journal.data['removed_this_run'] = 0
    journal.save()
    api.health()
    try:
        present = api.snapshot(vin)
    except HTTPFailure as error:
        if error.status != 408 or error.source != 'Fleet API' or not allow_wake or journal.data.get('wake_sent'):
            raise
        journal.data['wake_sent'] = True
        journal.save()
        print('Vehicle asleep; sending the explicitly permitted single wake command, then one fresh read.', flush=True)
        api.call('/api/1/vehicles/' + vin + '/wake_up', {})
        time.sleep(15)
        present = api.snapshot(vin)  # one safe read after wake; never loops/retries a command
    pending = pending_ids(approved, present)
    journal.data['resolved'] = sorted(set(approved) - set(pending))
    journal.data['already_absent_initially'] = len(approved) - len(pending)
    journal.save()
    print(f'Confirmed {len(pending)} reviewed schedules remain; {len(set(present)-set(approved))} other schedules will be untouched.', flush=True)
    for index, identity in enumerate(pending, 1):
        journal.data['in_flight'] = identity
        journal.save()
        api.remove(vin, identity)
        journal.data['resolved'].append(identity)
        journal.data['removed_this_run'] += 1
        journal.data.pop('in_flight', None)
        journal.save()
        if index % 15 == 0 or index == len(pending):
            print(f'Confirmed removals this run: {index}/{len(pending)} (paced at one request per 4 seconds).', flush=True)
    after = api.snapshot(vin) if pending else present
    remaining = pending_ids(approved, after)
    journal.data['resolved'] = sorted(set(approved) - set(remaining))
    journal.data['verified_complete'] = not remaining
    journal.data['verified_at'] = time.time()
    journal.save()
    print(f'Fresh verification: {len(remaining)} reviewed schedules remain; {len(set(after)-set(approved))} other schedules preserved.', flush=True)
    if remaining:
        raise RuntimeError('Some reviewed schedules still exist; no blind retries were made')


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--expected-count', required=True, type=int)
    parser.add_argument('--prepare-from-cache', type=Path)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--allow-wake', action='store_true', help='Permit one wake only if the first snapshot returns HTTP 408')
    options = parser.parse_args(arguments)
    if options.expected_count <= 0 or (options.prepare_from_cache and options.execute):
        parser.error('Use a positive approved count, and separate preparation from execution')
    if options.prepare_from_cache:
        prepare(options.prepare_from_cache, options.manifest, options.expected_count)
        return 0
    vin, approved, digest = load_manifest(options.manifest, options.expected_count)
    if not options.execute:
        print(f'Manifest contains {len(approved)} reviewed preconditioning schedules; no requests made.')
        return 0
    info = STATE.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o777 != 0o700:
        raise RuntimeError('Cleanup must run as the private state-directory owner')
    descriptor = os.open(STATE / '.run.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        journal = Journal(Path(str(options.manifest) + '.progress'), digest)
        execute(API(STATE, journal), journal, vin, approved, options.allow_wake)
    finally:
        os.close(descriptor)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, OSError) as error:
        print('Legacy cleanup: ' + str(error), file=sys.stderr, flush=True)
        raise SystemExit(1)

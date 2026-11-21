"""Compile the real refresh implementation with dummy tokens and mocked libcurl.

All HTTP transport and proxy forks are replaced by refresh_mock.cpp. Only token
paths are rewritten in a temporary source copy; application sources stay intact.
Requires the same C++/curlpp headers and date submodule as the application build.
"""

import json
import base64
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import time
from urllib.parse import parse_qs


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('g++'), 'C++ compiler required')
class RefreshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary_root = '/tmp/opencode' if Path('/tmp/opencode').is_dir() else None
        cls.temporary = tempfile.TemporaryDirectory(dir=temporary_root)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.tokens = cls.directory / 'tokens'
        source = (ROOT / 'tesla-api.cpp').read_text()
        source = source.replace('/var/tmp/tesla-cron', str(cls.tokens))
        (cls.directory / 'tesla-api.cpp').write_text(source)
        schedules = (ROOT / 'schedules.cpp').read_text().replace('/var/tmp/tesla-cron', str(cls.tokens))
        (cls.directory / 'schedules.cpp').write_text(schedules)
        cls.driver = cls.directory / 'refresh-test'
        result = subprocess.run([
            'g++', '-std=c++17', '-O0', '-ffunction-sections', '-fdata-sections', '-DUSE_OS_TZDB=1',
            '-I' + str(ROOT), '-I' + str(ROOT / 'date/include'),
            str(cls.directory / 'tesla-api.cpp'), str(ROOT / 'tests/refresh_mock.cpp'),
            str(cls.directory / 'schedules.cpp'), str(ROOT / 'date/src/tz.cpp'),
            '-Wl,--gc-sections', '-Wl,--export-dynamic-symbol=curl_easy_setopt',
            '-Wl,--export-dynamic-symbol=curl_easy_perform',
            '-Wl,--export-dynamic-symbol=curl_easy_getinfo',
            '-Wl,--export-dynamic-symbol=fork', '-lcurlpp', '-lcurl', '-pthread',
            '-o', str(cls.driver),
        ], text=True, capture_output=True, timeout=60)
        if result.returncode:
            raise RuntimeError('Offline refresh driver build failed:\n' + result.stderr)

    def setUp(self):
        self.tokens.mkdir(exist_ok=True)
        for name in ('access_token', 'refresh_token'):
            (self.tokens / (name + '.txt')).write_text('previous-' + name)
        self.request_log = self.directory / 'request.log'
        self.proxy_log = self.directory / 'proxy.log'
        self.tls_log = self.directory / 'tls.log'
        self.ca_file = self.directory / 'localhost.crt'
        self.ca_file.write_text('Mock trust certificate; the real libcurl transport is replaced.\n')
        self.request_log.unlink(missing_ok=True)
        self.proxy_log.unlink(missing_ok=True)
        self.tls_log.unlink(missing_ok=True)
        (self.tokens / 'api-not-before').unlink(missing_ok=True)
        for file in self.tokens.glob('schedules-*.json'):
            file.unlink()
        self.environment = {
            **os.environ,
            'TEST_REQUEST_LOG': str(self.request_log),
            'TEST_PROXY_LOG': str(self.proxy_log),
            'TEST_TLS_LOG': str(self.tls_log),
            'TESLA_PROXY_URL': 'https://127.0.0.1:4443',
            'TESLA_PROXY_CA_FILE': str(self.ca_file),
            'TEST_RESPONSE': json.dumps({
                'access_token': 'dummy-access-token',
                'refresh_token': 'dummy-refresh-token',
            }),
            'TEST_STATUS': '200',
        }
        self.environment.pop('TEST_TRANSPORT_ERROR', None)
        self.environment.pop('TEST_OPERATION', None)
        self.environment.pop('TEST_HEADERS', None)
        for key in ('TEST_HEALTH_STATUS', 'TEST_HEALTH_BODY', 'TEST_HEALTH_TRANSPORT_ERROR'):
            self.environment.pop(key, None)

    def refresh(self, **overrides):
        result = subprocess.run([str(self.driver)], text=True, capture_output=True,
                                env={**self.environment, **overrides}, timeout=5)
        self.assertFalse(self.proxy_log.exists(), 'Refresh must not start the proxy')
        for token in ('dummy-access-token', 'dummy-refresh-token', 'previous-refresh_token'):
            self.assertNotIn(token, result.stdout + result.stderr)
        self.assertFalse(list(self.tokens.glob('*.XXXXXX')))
        names = {path.name for path in self.tokens.iterdir()}
        self.assertFalse(names - {'.refresh.lock', 'access_token.txt', 'refresh_token.txt',
                                  'api-not-before', 'schedules-TESTVEHICLE1234567.json'})
        return result

    def assert_unchanged(self):
        for name in ('access_token', 'refresh_token'):
            self.assertEqual((self.tokens / (name + '.txt')).read_text(), 'previous-' + name)

    def request_lines(self, include_health=False):
        lines = self.request_log.read_text().splitlines()
        return [cell for index in range(0, len(lines), 2)
                if include_health or not lines[index].endswith('/health')
                for cell in lines[index:index + 2]]

    def test_success_rotates_tokens_with_one_request(self):
        result = self.refresh()
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.request_log.read_text().splitlines()
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], 'https://fleet-auth.prd.vn.cloud.tesla.com/oauth2/v3/token')
        self.assertEqual(parse_qs(lines[1]), {
            'grant_type': ['refresh_token'],
            'client_id': ['test-client'],
            'refresh_token': ['previous-refresh_token'],
        })
        for name in ('access_token', 'refresh_token'):
            path = self.tokens / (name + '.txt')
            self.assertEqual(path.read_text(), 'dummy-' + name.replace('_', '-'))
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_rate_limit_does_not_retry_or_replace_tokens(self):
        result = self.refresh(TEST_STATUS='429')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('HTTP 429', result.stderr)
        self.assertEqual(len(self.request_log.read_text().splitlines()), 2)
        self.assert_unchanged()

    def test_timeout_does_not_retry(self):
        result = self.refresh(TEST_TRANSPORT_ERROR='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.request_log.read_text().splitlines()), 2)
        self.assert_unchanged()

    def test_http_failure_preserves_tokens(self):
        result = self.refresh(TEST_STATUS='401')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('HTTP 401', result.stderr)
        self.assert_unchanged()

    def test_invalid_response_preserves_tokens(self):
        for response in ('not json', '{}', '[]',
                         '{"access_token":"dummy-access-token","refresh_token":""}'):
            with self.subTest(response=response):
                result = self.refresh(TEST_RESPONSE=response)
                self.assertNotEqual(result.returncode, 0)
                self.assert_unchanged()

    def test_empty_refresh_token_never_makes_request(self):
        (self.tokens / 'refresh_token.txt').write_text('')
        result = self.refresh()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.request_log.exists())
        self.assertEqual((self.tokens / 'access_token.txt').read_text(), 'previous-access_token')

    def test_cooldown_blocks_subsequent_process(self):
        first = self.refresh(TEST_STATUS='429', TEST_HEADERS='Retry-After: 3600\r\n')
        self.assertNotEqual(first.returncode, 0)
        self.assertGreaterEqual(int((self.tokens / 'api-not-before').read_text()), int(time.time()) + 3598)
        second = self.refresh()
        self.assertNotEqual(second.returncode, 0)
        self.assertIn('cooldown active', second.stderr)
        self.assertEqual(len(self.request_log.read_text().splitlines()), 2)

    def test_corrupt_cooldown_fails_closed(self):
        (self.tokens / 'api-not-before').write_text('not a timestamp')
        result = self.refresh()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.request_log.exists())

    def test_rate_limit_reset_header(self):
        reset = int(time.time()) + 7200
        result = self.refresh(TEST_STATUS='429', TEST_HEADERS=f'RateLimit-Reset: {reset}\r\n')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(int((self.tokens / 'api-not-before').read_text()), reset)

    def test_expired_reset_header_is_not_treated_as_multi_year_delay(self):
        now = int(time.time())
        result = self.refresh(TEST_STATUS='429', TEST_HEADERS=f'RateLimit-Reset: {now - 10}\r\n')
        self.assertNotEqual(result.returncode, 0)
        until = int((self.tokens / 'api-not-before').read_text())
        self.assertGreaterEqual(until, now + 900)
        self.assertLessEqual(until, int(time.time()) + 901)

    def test_current_token_needs_no_refresh_request(self):
        payload = base64.urlsafe_b64encode(json.dumps({'exp': int(time.time()) + 3600}).encode()).decode().rstrip('=')
        (self.tokens / 'access_token.txt').write_text('header.' + payload + '.signature')
        result = self.refresh(TEST_OPERATION='ensure')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.request_log.exists())

    def test_per_run_request_budget(self):
        result = self.refresh(TEST_OPERATION='budget', TEST_RESPONSE='{"response":{"state":"online"}}')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('request budget', result.stderr)
        self.assertEqual(len(self.request_log.read_text().splitlines()), 40)

    def test_reconciliation_is_idempotent_and_persists_ids(self):
        result = self.refresh(TEST_OPERATION='reconcile', TEST_RESPONSE='{"response":{"result":true}}')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.request_lines()
        self.assertEqual(len(lines), 4, 'Second reconciliation must not repeat POSTs')
        owned_file = self.tokens / 'schedules-TESTVEHICLE1234567.json'
        owned = json.loads(owned_file.read_text())
        self.assertEqual(json.loads(lines[1])['id'], owned['charge'])
        self.assertEqual(json.loads(lines[3])['id'], owned['precondition'])
        self.assertTrue(lines[0].endswith('/command/add_charge_schedule'))
        self.assertTrue(lines[2].endswith('/command/add_precondition_schedule'))
        self.assertEqual(owned_file.stat().st_mode & 0o777, 0o600)

    def test_ambiguous_schedule_timeout_keeps_owned_ids_without_retry(self):
        result = self.refresh(TEST_OPERATION='reconcile', TEST_TRANSPORT_ERROR='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.request_lines()), 2)
        self.assertTrue((self.tokens / 'schedules-TESTVEHICLE1234567.json').exists())

    def test_same_owned_schedule_ids_follow_vehicle_between_locations(self):
        result = self.refresh(TEST_OPERATION='move', TEST_RESPONSE='{"response":{"result":true}}')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.request_lines()
        self.assertEqual(len(lines), 8)
        first_charge, first_precondition = json.loads(lines[1]), json.loads(lines[3])
        moved_charge, moved_precondition = json.loads(lines[5]), json.loads(lines[7])
        self.assertEqual(first_charge['id'], moved_charge['id'])
        self.assertEqual(first_precondition['id'], moved_precondition['id'])
        self.assertEqual(first_charge['lat'], 55)
        self.assertEqual(moved_charge['lat'], 56)
        self.assertEqual(moved_precondition['lon'], 10)

    def test_proxy_health_verifies_tls_without_authorization(self):
        result = self.refresh(TEST_OPERATION='health')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.request_lines(include_health=True), ['https://127.0.0.1:4443/health', ''])
        fields = self.tls_log.read_text().strip().split('\t')
        self.assertEqual(fields[1:], [str(self.ca_file), '1', '2', '1', '0'])
        self.assert_unchanged()

    def test_proxy_health_before_command_with_pinned_trust(self):
        result = self.refresh(TEST_OPERATION='command', TEST_RESPONSE='{"response":{"result":true}}')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.request_lines(include_health=True)
        self.assertEqual(lines[0], 'https://127.0.0.1:4443/health')
        self.assertEqual(lines[2], 'https://127.0.0.1:4443/api/1/vehicles/TEST/command/set_charge_limit')
        records = [line.split('\t') for line in self.tls_log.read_text().splitlines()]
        self.assertEqual(records[1][1:], [str(self.ca_file), '1', '2', '1', '1'])

    def test_failed_proxy_health_never_posts_command(self):
        for status in ('401', '429', '503'):
            with self.subTest(status=status):
                self.request_log.unlink(missing_ok=True)
                result = self.refresh(TEST_OPERATION='command', TEST_HEALTH_STATUS=status)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('health/TLS', result.stderr)
                self.assertEqual(len(self.request_lines(include_health=True)), 2)
                self.assertEqual(self.request_lines(), [])
                self.assertFalse((self.tokens / 'api-not-before').exists(), 'Local health errors are not Tesla rate limits')

    def test_proxy_tls_failure_never_posts_command(self):
        result = self.refresh(TEST_OPERATION='command', TEST_HEALTH_TRANSPORT_ERROR='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('health/TLS', result.stderr)
        self.assertEqual(self.request_lines(), [])

    def test_unexpected_health_body_never_posts_command(self):
        result = self.refresh(TEST_OPERATION='command', TEST_HEALTH_BODY='<html>wrong server</html>')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.request_lines(), [])

    def test_nonlocal_or_insecure_proxy_urls_are_rejected(self):
        for url in ('http://127.0.0.1:4443', 'https://example.com:4443', 'https://0.0.0.0:4443',
                    'https://user@localhost:4443', 'https://localhost:4443/prefix',
                    'https://localhost:4443?query=1', 'https://localhost:4443#fragment'):
            with self.subTest(url=url):
                result = self.refresh(TEST_OPERATION='command', TESLA_PROXY_URL=url)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.request_log.exists())

    def test_unreadable_proxy_trust_file_fails_before_network(self):
        result = self.refresh(TEST_OPERATION='command', TESLA_PROXY_CA_FILE=str(self.directory / 'missing.crt'))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.request_log.exists())

    def test_local_health_allowed_during_tesla_cooldown(self):
        (self.tokens / 'api-not-before').write_text(str(int(time.time()) + 3600))
        result = self.refresh(TEST_OPERATION='health')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.request_lines(), [])

    def test_single_health_check_for_multiple_schedule_commands(self):
        result = self.refresh(TEST_OPERATION='reconcile', TEST_RESPONSE='{"response":{"result":true}}')
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = self.request_lines(include_health=True)
        self.assertEqual(sum(line.endswith('/health') for line in lines[::2]), 1)


if __name__ == '__main__':
    unittest.main()

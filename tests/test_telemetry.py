"""Telemetry serialization and real HTTP writes to a throwaway loopback server."""

import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('g++'), 'C++ compiler required')
class TelemetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.driver = Path(cls.temporary.name) / 'telemetry-test'
        built = subprocess.run([
            'g++', '-std=c++17', '-I' + str(ROOT), str(ROOT / 'tests/telemetry_driver.cpp'),
            str(ROOT / 'graph.cpp'), str(ROOT / 'influx.cpp'), '-lcurlpp', '-lcurl', '-o', str(cls.driver),
        ], capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise RuntimeError('Telemetry test build failed:\n' + built.stderr)

    def setUp(self):
        self.requests = []
        self.status = 204
        self.reply = b''
        test = self

        class Handler(BaseHTTPRequestHandler):
            def request(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
                test.requests.append((self.command, self.path, dict(self.headers), body))
                self.send_response(test.status)
                self.end_headers()
                self.wfile.write(test.reply)

            do_POST = do_GET = request

            def log_message(self, *_):
                pass

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.stop_server)
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith('INFLUX_')}
        self.environment.update(INFLUX_URL=f'http://127.0.0.1:{self.server.server_port}/',
                                INFLUX_DB='telemetry test', NO_PROXY='127.0.0.1', no_proxy='127.0.0.1')

    def stop_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def run_driver(self, mode='line', site='test-site', **environment):
        return subprocess.run([str(self.driver), mode, site], env={**self.environment, **environment},
                              capture_output=True, text=True, timeout=10)

    def test_line_protocol_schema_and_types(self):
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith('tesla_cron,site=test-site,vin=TESTVIN '))
        for value in ('price_dkk_kwh=-0.125', 'battery_level=55i', 'charge_limit=80i',
                      'charge_window_level=6i', 'charging=true', 'event=true', 'cached=true',
                      'vehicle_data_time=1200i', 'snapshot_age_s=600i', 'price_slot_start=1800i', 'next_event=2100i'):
            self.assertIn(value, result.stdout)
        self.assertTrue(result.stdout.endswith(' 1800\n'))
        self.assertEqual(result.stdout.count('\n'), 1)

    def test_tag_escaping_and_line_injection(self):
        result = self.run_driver(site='a b,c=d\\e')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('site=a\\ b\\,c\\=d\\\\e,vin=', result.stdout)
        for site in ('bad\nsite', 'bad\rsite', ''):
            with self.subTest(site=site):
                self.assertNotEqual(self.run_driver(site=site).returncode, 0)

    def test_locale_does_not_change_float_format(self):
        result = self.run_driver('locale')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('price_dkk_kwh=-0.125', result.stdout)

    def test_invalid_values_rejected(self):
        for mode in ('bad-price', 'bad-window', 'no-state'):
            with self.subTest(mode=mode):
                self.assertNotEqual(self.run_driver(mode).returncode, 0)
        self.assertEqual(self.requests, [])

    def test_fresh_and_unknown_snapshot_metadata(self):
        fresh = self.run_driver('fresh')
        self.assertIn('cached=false', fresh.stdout)
        self.assertIn('snapshot_age_s=20i', fresh.stdout)
        for mode in ('unknown', 'future'):
            result = self.run_driver(mode)
            self.assertIn('cached=true', result.stdout)
            self.assertNotIn('snapshot_age_s=', result.stdout)
            self.assertNotIn('vehicle_data_time=', result.stdout)

    def test_v1_write_endpoint_and_seconds_precision(self):
        result = self.run_driver('write')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.requests), 1)
        method, path, headers, body = self.requests[0]
        self.assertEqual(method, 'POST')
        self.assertEqual(urlsplit(path).path, '/write')
        self.assertEqual(parse_qs(urlsplit(path).query), {'db': ['telemetry test'], 'precision': ['s']})
        self.assertEqual(headers['Content-Type'], 'text/plain; charset=utf-8')
        self.assertTrue(body.startswith(b'tesla_cron,site=test-site,vin=TESTVIN '))

    def test_writer_credentials_are_separate_from_reader(self):
        result = self.run_driver('write', INFLUX_USER='reader', INFLUX_PASS='read-password',
                                 INFLUX_WRITE_USER='writer', INFLUX_WRITE_PASS='write-password')
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = 'Basic ' + base64.b64encode(b'writer:write-password').decode()
        self.assertEqual(self.requests[0][2]['Authorization'], expected)
        self.assertNotIn('write-password', result.stdout + result.stderr)

    def test_query_endpoint_uses_reader_credentials(self):
        self.status, self.reply = 200, b'{"results":[{}]}'
        result = self.run_driver('query', INFLUX_USER='reader', INFLUX_PASS='read-password',
                                 INFLUX_WRITE_USER='writer', INFLUX_WRITE_PASS='write-password')
        self.assertEqual(result.returncode, 0, result.stderr)
        method, path, headers, _ = self.requests[0]
        self.assertEqual(method, 'GET')
        parameters = parse_qs(urlsplit(path).query)
        self.assertEqual(parameters['epoch'], ['s'])
        self.assertIn("'a b'", parameters['q'][0])
        self.assertEqual(headers['Authorization'], 'Basic ' + base64.b64encode(b'reader:read-password').decode())

    def test_write_failure_is_nonfatal_and_not_retried(self):
        self.status, self.reply = 503, b'private server error detail'
        result = self.run_driver('write')
        self.assertEqual(result.returncode, 0)
        self.assertIn('Influx write HTTP 503', result.stderr)
        self.assertNotIn('private server error detail', result.stderr)
        self.assertEqual(len(self.requests), 1)

    def test_empty_and_invalid_points_never_post(self):
        self.assertEqual(self.run_driver('empty').returncode, 0)
        result = self.run_driver('write-invalid')
        self.assertEqual(result.returncode, 0)
        self.assertIn('Invalid telemetry', result.stderr)
        self.assertEqual(self.requests, [])

    def test_invalid_timeout_fails_before_network(self):
        for timeout in ('bad', '0', '60001'):
            with self.subTest(timeout=timeout):
                result = self.run_driver('write', INFLUX_TIMEOUT_MS=timeout)
                self.assertEqual(result.returncode, 0)
                self.assertIn('INFLUX_TIMEOUT_MS', result.stderr)
        self.assertEqual(self.requests, [])


if __name__ == '__main__':
    unittest.main()

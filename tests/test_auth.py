"""Offline OAuth regression checks: python3 -m unittest discover -s tests -v."""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from urllib.parse import parse_qs, urlencode, urlsplit


ROOT = Path(__file__).resolve().parents[1]


class AuthTests(unittest.TestCase):
    def setUp(self):
        temporary_root = '/tmp/opencode' if Path('/tmp/opencode').is_dir() else None
        self.temporary = tempfile.TemporaryDirectory(dir=temporary_root)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.tokens = self.directory / 'tokens'
        self.tokens.mkdir()
        for name in ('access_token', 'refresh_token'):
            (self.tokens / (name + '.txt')).write_text('previous-' + name)
        self.config = self.directory / 'auth.env'
        self.config.write_text('# Test credentials supplied through environment.\n')
        mock = self.directory / 'curl'
        mock.write_text('''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys
from urllib.parse import parse_qs

request = {'arguments': sys.argv[1:], 'body': parse_qs(sys.stdin.read())}
Path(os.environ['REQUEST_LOG']).write_text(json.dumps(request))
print(os.environ['MOCK_RESPONSE'])
print(os.environ['MOCK_STATUS'])
''')
        mock.chmod(0o700)
        self.environment = {
            **os.environ,
            'PATH': str(self.directory) + os.pathsep + os.environ['PATH'],
            'TESLA_AUTH_CONFIG': str(self.config),
            'TESLA_CLIENT_ID': 'test-client',
            'TESLA_CLIENT_SECRET': 'dummy-secret-&+value',
            'REDIRECT_URI': 'https://example.test/auth/callback',
            'AUDIENCE': 'https://fleet-api.prd.eu.vn.cloud.tesla.com',
            'TOKEN_DIR': str(self.tokens),
            'REQUEST_LOG': str(self.directory / 'request.json'),
            'MOCK_STATUS': '200',
            'MOCK_RESPONSE': json.dumps({
                'access_token': 'dummy-access-token',
                'refresh_token': 'dummy-refresh-token',
            }),
        }

    def authorize(self, callback_change=None, environment=None):
        process = subprocess.Popen(
            ['bash', str(ROOT / 'auth.sh')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, env={**self.environment, **(environment or {})},
        )
        self.addCleanup(lambda: process.poll() is None and process.kill())
        output = ''
        while True:
            line = process.stdout.readline()
            output += line
            if line.startswith('https://auth.tesla.com/'):
                break
            if not line:
                stdout, stderr = process.communicate(timeout=10)
                self.fail('Authorization URL not emitted: ' + output + stdout + stderr)
        url = urlsplit(line.strip())
        params = parse_qs(url.query)
        self.assertEqual(params['client_id'], ['test-client'])
        self.assertEqual(params['redirect_uri'], [self.environment['REDIRECT_URI']])
        self.assertEqual(params['prompt_missing_scopes'], ['true'])
        self.assertNotIn('energy_device_data', params['scope'][0])
        self.assertIn('vehicle_location', params['scope'][0].split())
        self.assertGreaterEqual(len(params['state'][0]), 32)
        callback = {'code': 'test-code+&value', 'state': params['state'][0]}
        if callback_change:
            callback_change(callback)
        callback_url = self.environment['REDIRECT_URI'] + '?' + urlencode(callback)
        stdout, stderr = process.communicate(callback_url + '\n', timeout=10)
        output += stdout
        for sensitive in ('dummy-secret', 'dummy-access-token', 'dummy-refresh-token', 'test-code'):
            self.assertNotIn(sensitive, output + stderr)
        return process.returncode, output, stderr

    def assert_old_tokens_preserved(self):
        for name in ('access_token', 'refresh_token'):
            self.assertEqual((self.tokens / (name + '.txt')).read_text(), 'previous-' + name)
        self.assertFalse(list(self.tokens.glob('.auth-*')))

    def test_success_uses_current_endpoint_and_form_encoding(self):
        status, output, stderr = self.authorize()
        self.assertEqual(status, 0, stderr)
        self.assertIn('Saved access and refresh tokens', output)
        request = json.loads((self.directory / 'request.json').read_text())
        self.assertIn('https://fleet-auth.prd.vn.cloud.tesla.com/oauth2/v3/token', request['arguments'])
        self.assertIn('Content-Type: application/x-www-form-urlencoded', request['arguments'])
        self.assertNotIn(self.environment['TESLA_CLIENT_SECRET'], ' '.join(request['arguments']))
        self.assertEqual(request['body']['client_secret'], [self.environment['TESLA_CLIENT_SECRET']])
        self.assertEqual(request['body']['code'], ['test-code+&value'])
        self.assertEqual(request['body']['redirect_uri'], [self.environment['REDIRECT_URI']])
        self.assertEqual(request['body']['audience'], [self.environment['AUDIENCE']])
        for name in ('access_token', 'refresh_token'):
            file = self.tokens / (name + '.txt')
            self.assertEqual(file.read_text().strip(), 'dummy-' + name.replace('_', '-'))
            self.assertEqual(file.stat().st_mode & 0o777, 0o600)

    def test_state_mismatch_never_exchanges_code(self):
        status, _, stderr = self.authorize(lambda callback: callback.update(state='wrong-state'))
        self.assertNotEqual(status, 0)
        self.assertIn('state mismatch', stderr)
        self.assertFalse((self.directory / 'request.json').exists())
        self.assert_old_tokens_preserved()

    def test_denied_consent_never_exchanges_code(self):
        status, _, stderr = self.authorize(lambda callback: callback.update(error='access_denied'))
        self.assertNotEqual(status, 0)
        self.assertIn('declined authorization', stderr)
        self.assertFalse((self.directory / 'request.json').exists())
        self.assert_old_tokens_preserved()

    def test_http_error_preserves_existing_tokens(self):
        status, _, stderr = self.authorize(environment={'MOCK_STATUS': '400'})
        self.assertNotEqual(status, 0)
        self.assertIn('HTTP 400', stderr)
        self.assert_old_tokens_preserved()

    def test_invalid_responses_preserve_existing_tokens(self):
        for response in ('not json', '{}', '[]', '{"access_token":"dummy-access-token"}',
                         '{"access_token":"null","refresh_token":"null"}'):
            with self.subTest(response=response):
                status, _, _ = self.authorize(environment={'MOCK_RESPONSE': response})
                self.assertNotEqual(status, 0)
                self.assert_old_tokens_preserved()


if __name__ == '__main__':
    unittest.main()

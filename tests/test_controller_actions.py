"""Offline action ordering, persisted drive latch and one-shot delayed-stop tests."""

import base64
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
VIN = 'TESTVEHICLE1234567'
EPOCH = 1800000000


@unittest.skipUnless(shutil.which('g++'), 'C++ compiler required')
class ControllerActionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.state = cls.directory / 'state'
        copied = []
        for name in ('tesla-api.cpp', 'schedules.cpp', 'control_state.cpp', 'controller_actions.cpp'):
            path = cls.directory / name
            path.write_text((ROOT / name).read_text().replace('/var/tmp/tesla-cron', str(cls.state)))
            copied.append(str(path))
        cls.driver = cls.directory / 'actions'
        built = subprocess.run([
            'g++', '-std=c++17', '-O0', '-ffunction-sections', '-fdata-sections',
            '-DUSE_OS_TZDB=1', '-DAPI_MOCK_NO_MAIN', '-I' + str(ROOT), '-I' + str(ROOT / 'date/include'),
            *copied, *[str(ROOT / name) for name in ('control.cpp', 'vehicle_data.cpp', 'tests/refresh_mock.cpp',
                                                    'tests/controller_actions_driver.cpp', 'date/src/tz.cpp')],
            '-Wl,--gc-sections', '-Wl,--export-dynamic-symbol=curl_easy_setopt',
            '-Wl,--export-dynamic-symbol=curl_easy_perform', '-Wl,--export-dynamic-symbol=curl_easy_getinfo',
            '-Wl,--export-dynamic-symbol=fork', '-lcurlpp', '-lcurl', '-pthread', '-o', str(cls.driver),
        ], capture_output=True, text=True, timeout=120)
        if built.returncode:
            raise RuntimeError('Controller action test compilation failed:\n' + built.stderr)

    def setUp(self):
        self.state.mkdir(mode=0o700, exist_ok=True)
        for file in self.state.iterdir():
            file.unlink()
        payload = base64.urlsafe_b64encode(json.dumps({'exp': int(time.time()) + 3600}).encode()).decode().rstrip('=')
        (self.state / 'access_token.txt').write_text('header.' + payload + '.signature')
        (self.state / ('schedules-' + VIN + '.json')).write_text('{"charge":11,"precondition":21}')
        self.control_file = self.state / ('control-' + VIN + '.json')
        self.log = self.state / 'requests'
        self.ca = self.state / 'ca.crt'
        self.ca.write_text('dummy CA for replaced libcurl transport')
        expected = datetime.fromtimestamp(EPOCH + 14400, timezone.utc)
        self.schedule = {'id': 11, 'latitude': 55.0, 'longitude': 12.0,
                         'days_of_week': 1 << ((expected.weekday() + 1) % 7),
                         'start_time': expected.hour * 60 + expected.minute, 'end_time': 0,
                         'enabled': True, 'one_time': True, 'start_enabled': True, 'end_enabled': False}
        self.response = {'response': {'vin': VIN,
            'charge_state': {'charge_current_request': 16, 'battery_level': 55, 'charge_limit_soc': 80,
                             'charging_state': 'Charging', 'scheduled_charging_mode': 'ChargeAt'},
            'drive_state': {'latitude': 55.0, 'longitude': 12.0, 'speed': None},
            'charge_schedule_data': {'charge_schedules': [self.schedule]},
            'preconditioning_schedule_data': {'precondition_schedules': []}}}
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith('TEST_')}
        self.environment.update(TEST_REQUEST_LOG=str(self.log), TEST_PROXY_LOG=str(self.state / 'fork'),
            TEST_STATUS='200', TEST_RESPONSE=json.dumps(self.response), TEST_COMMAND_RESPONSE='{"response":{"result":true}}',
            TESLA_PROXY_URL='https://127.0.0.1:4443', TESLA_PROXY_CA_FILE=str(self.ca))

    def run_action(self, mode, **environment):
        result = subprocess.run([str(self.driver)], env={**self.environment, 'TEST_ACTION': mode, **environment},
                                capture_output=True, text=True, timeout=10)
        self.assertFalse((self.state / 'fork').exists())
        return result

    def requests(self):
        if not self.log.exists():
            return []
        lines = self.log.read_text().splitlines()
        return list(zip(lines[::2], lines[1::2]))

    def commands(self):
        return [(url.rsplit('/', 1)[-1], json.loads(body)) for url, body in self.requests() if '/command/' in url]

    def test_drive_reset_once_and_manual_later_limit_preserved(self):
        result = self.run_action('drive')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands(), [('set_charge_limit', {'percent': 50})])
        self.assertIn('Drive final limit: 90', result.stdout)
        saved = json.loads(self.control_file.read_text())
        self.assertTrue(saved['was_driving'])
        self.assertEqual(self.control_file.stat().st_mode & 0o777, 0o600)

    def test_ambiguous_drive_reset_is_not_replayed_on_resume(self):
        first = self.run_action('drive', TEST_FAIL_COMMAND='set_charge_limit')
        self.assertNotEqual(first.returncode, 0)
        before = len(self.requests())
        second = self.run_action('drive')
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(len(self.requests()), before)

    def test_unsent_drive_reset_does_not_consume_drive_latch(self):
        result = self.run_action('drive', TEST_NO_BUDGET='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.requests(), [])
        self.assertFalse(self.control_file.exists())
        result = self.run_action('drive')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands(), [('set_charge_limit', {'percent': 50})])

    def test_unavailable_local_proxy_does_not_consume_drive_latch(self):
        result = self.run_action('drive', TEST_HEALTH_STATUS='503')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.commands(), [])
        self.assertFalse(self.control_file.exists())

    def test_guard_persisted_before_schedule_then_limit_write(self):
        result = self.run_action('apply', TEST_EXPECT_GUARD_FILE=str(self.control_file))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([name for name, _ in self.commands()], ['add_charge_schedule', 'set_charge_limit'])
        self.assertIsInstance(json.loads(self.control_file.read_text())['guard'], dict)

    def test_unchanged_plan_does_not_arm_or_spend_api_budget(self):
        result = self.run_action('unchanged', TEST_LOW_BUDGET='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests(), [])
        self.assertIsNone(json.loads(self.control_file.read_text())['guard'])

    def test_preconditioning_only_change_has_no_charge_guard(self):
        result = self.run_action('precondition', TEST_LOW_BUDGET='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([name for name, _ in self.commands()], ['add_precondition_schedule'])
        self.assertIsNone(json.loads(self.control_file.read_text())['guard'])

    def test_future_charge_deferred_when_guard_budget_unavailable(self):
        result = self.run_action('apply', TEST_LOW_BUDGET='1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('API budget', result.stderr)
        self.assertEqual(self.requests(), [])

    def test_delayed_guard_consumed_before_one_fresh_read_and_one_stop(self):
        result = self.run_action('guard', TEST_EXPECT_CONSUMED_FILE=str(self.control_file))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([name for name, _ in self.commands()], ['charge_stop'])
        self.assertEqual(sum('/vehicle_data' in url for url, _ in self.requests()), 1)
        self.assertIn('Fresh state: Stopped', result.stdout)
        self.assertIsNone(json.loads(self.control_file.read_text())['guard'])
        before = len(self.requests())
        self.assertEqual(self.run_action('resume').returncode, 0)
        self.assertEqual(len(self.requests()), before)

    def test_no_stop_for_waiting_changed_limit_unplugged_or_changed_schedule(self):
        for condition in ('waiting', 'manual-limit', 'unplugged', 'schedule-change'):
            with self.subTest(condition=condition):
                self.log.unlink(missing_ok=True)
                response = json.loads(json.dumps(self.response))
                if condition == 'waiting': response['response']['charge_state']['charging_state'] = 'Stopped'
                if condition == 'manual-limit': response['response']['charge_state']['charge_limit_soc'] = 90
                if condition == 'unplugged': response['response']['charge_state']['charging_state'] = 'Disconnected'
                if condition == 'schedule-change': response['response']['charge_schedule_data']['charge_schedules'][0]['enabled'] = False
                result = self.run_action('guard', TEST_RESPONSE=json.dumps(response))
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.commands(), [])

    def test_no_stop_if_vehicle_left_site(self):
        result = self.run_action('guard', TEST_OTHER_SITE='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.commands(), [])

    def test_expired_guard_never_queries_or_stops(self):
        result = self.run_action('guard', TEST_EXPIRED_GUARD='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests(), [])

    def test_failed_guard_read_does_not_stop_or_retry(self):
        result = self.run_action('guard', TEST_STATUS='408')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.requests()), 1)
        self.assertEqual(self.commands(), [])
        self.assertIsNone(json.loads(self.control_file.read_text())['guard'])

    def test_ambiguous_stop_is_not_retried(self):
        result = self.run_action('guard', TEST_FAIL_COMMAND='charge_stop')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([name for name, _ in self.commands()], ['charge_stop'])
        self.assertIsNone(json.loads(self.control_file.read_text())['guard'])


if __name__ == '__main__':
    unittest.main()

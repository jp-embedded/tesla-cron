"""Full normal-run regression tests with copied state paths, fake config and transport."""

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
VIN = 'TESTVIN1234567890'


@unittest.skipUnless(shutil.which('g++'), 'C++ compiler required')
class ControllerRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.state = cls.directory / 'state'
        copied = []
        for name in ('tesla_cron.cpp', 'tesla-api.cpp', 'schedules.cpp', 'control_state.cpp', 'controller_actions.cpp'):
            path = cls.directory / name
            path.write_text((ROOT / name).read_text().replace('/var/tmp/tesla-cron', str(cls.state)))
            copied.append(str(path))
        (cls.directory / 'configuration.h').write_text('''#include <cstdlib>
#include <string>
#include <vector>
struct test_car { std::string vin; std::vector<std::string> calendars; };
struct test_account { std::string tesla_client_id; std::string tesla_audience; std::vector<test_car> cars; };
const test_account account = [] {
    test_car car {"TESTVIN1234567890", {}};
    if (std::getenv("TEST_CALENDAR_FAILURE")) car.calendars.push_back("https://calendar.test/must-not-be-read");
    return test_account {"test-client", "https://fleet-api.prd.eu.vn.cloud.tesla.com", {car}};
}();
''')
        cls.binary = cls.directory / 'controller'
        built = subprocess.run([
            'g++', '-std=c++17', '-O0', '-ffunction-sections', '-fdata-sections',
            '-DUSE_OS_TZDB=1', '-DAPI_MOCK_NO_MAIN', '-I' + str(cls.directory), '-I' + str(ROOT), '-I' + str(ROOT / 'date/include'),
            *copied, *[str(ROOT / name) for name in ('control.cpp', 'vehicle_data.cpp', 'prices.cpp', 'influx.cpp', 'graph.cpp',
                'location.cpp', 'icalendarlib/icalendar.cpp', 'icalendarlib/date.cpp', 'icalendarlib/types.cpp', 'date/src/tz.cpp',
                'tests/refresh_mock.cpp', 'tests/controller_run_clock.cpp')],
            '-Wl,--gc-sections', '-Wl,--export-dynamic-symbol=curl_easy_setopt',
            '-Wl,--export-dynamic-symbol=curl_easy_perform', '-Wl,--export-dynamic-symbol=curl_easy_getinfo',
            '-Wl,--export-dynamic-symbol=fork', '-lcurlpp', '-lcurl', '-pthread', '-o', str(cls.binary),
        ], capture_output=True, text=True, timeout=120)
        if built.returncode:
            raise RuntimeError('Full controller test build failed:\n' + built.stderr)

    def setUp(self):
        self.state.mkdir(mode=0o700, exist_ok=True)
        for file in self.state.iterdir():
            file.unlink()
        now = int(time.time())
        jwt = base64.urlsafe_b64encode(json.dumps({'exp': now + 3600}).encode()).decode().rstrip('=')
        (self.state / 'access_token.txt').write_text('header.' + jwt + '.signature')
        self.payload = {'tesla_cron_fetched_at': now - 60, 'response': {'vin': VIN, 'state': 'online',
            'charge_state': {'charge_current_request': 16, 'battery_level': 60, 'charge_limit_soc': 80,
                             'charging_state': 'Disconnected', 'scheduled_charging_mode': 'Off'},
            'drive_state': {'latitude': 51, 'longitude': 0, 'speed': None},
            'charge_schedule_data': {'charge_schedules': []},
            'preconditioning_schedule_data': {'precondition_schedules': []}}}
        cache = self.state / ('tesla-' + VIN + '.cache')
        cache.write_text(json.dumps(self.payload))
        cache.chmod(0o600)
        ca = self.state / 'ca.crt'
        ca.write_text('dummy trust certificate for mocked transport')
        self.log = self.state / 'requests'
        self.environment = {key: value for key, value in os.environ.items() if not key.startswith(('TEST_', 'INFLUX_'))}
        self.environment.update(TEST_REQUEST_LOG=str(self.log), TEST_PROXY_LOG=str(self.state / 'fork'), TEST_STATUS='200',
            TEST_COMMAND_RESPONSE='{"response":{"result":true}}', TESLA_PROXY_URL='https://127.0.0.1:4443',
            TESLA_PROXY_CA_FILE=str(ca), TESLA_SITES_FILE=str(self.state / 'missing-sites.json'), INFLUX_URL='http://127.0.0.1:1')

    def run_controller(self, payload, **environment):
        result = subprocess.run([str(self.binary)], capture_output=True, text=True, timeout=15,
            env={**self.environment, 'TEST_RESPONSE': json.dumps(payload), **environment})
        self.assertFalse((self.state / 'fork').exists())
        return result

    def requests(self):
        lines = self.log.read_text().splitlines()
        return list(zip(lines[::2], lines[1::2]))

    def test_fresh_drive_breaks_recent_parked_cache_and_resets_before_dependencies(self):
        fresh = json.loads(json.dumps(self.payload))
        fresh['response']['drive_state'].update(speed=10, shift_state='D')
        result = self.run_controller(fresh, TEST_CALENDAR_FAILURE='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [(url.rsplit('/', 1)[-1], json.loads(body)) for url, body in self.requests() if '/command/' in url]
        self.assertEqual(commands, [('set_charge_limit', {'percent': 50})])
        self.assertFalse(any('/query?' in url or 'calendar.test' in url for url, _ in self.requests()))
        saved = json.loads((self.state / ('control-' + VIN + '.json')).read_text())
        self.assertTrue(saved['was_driving'])

    def test_recent_outside_cache_does_not_hide_online_arrival(self):
        sites = self.state / 'sites.json'
        sites.write_text(json.dumps([{'site': 'home', 'latitude': 55, 'longitude': 12, 'radius_m': 250, 'time_zone': 'UTC'}]))
        fresh = json.loads(json.dumps(self.payload))
        fresh['response']['drive_state'].update(latitude=55, longitude=12)
        fresh['response']['charge_state']['charging_state'] = 'Charging'
        slot = int(time.time()) // 900 * 900
        prices = {'results': [{'series': [{'columns': ['time', 'price'], 'values': [[slot + index * 900, 1] for index in range(193)]}]}]}
        result = self.run_controller(fresh, TESLA_SITES_FILE=str(sites), TEST_INFLUX_RESPONSE=json.dumps(prices), TEST_ALLOW_TELEMETRY='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(any('/vehicle_data' in url for url, _ in self.requests()))
        self.assertTrue(any('/write?' in url and 'site=home' in body for url, body in self.requests()))
        self.assertFalse(any('/command/' in url for url, _ in self.requests()), 'Already-running higher-limit charge stays untouched')

    def test_pending_guard_resumes_without_summary_wake_or_replanning(self):
        now = int(time.time())
        planned = now + 3600
        local = datetime.fromtimestamp(planned, timezone.utc)
        expected = {'id': 11, 'latitude': 55.0, 'longitude': 12.0,
                    'start_time': local.hour * 60 + local.minute, 'end_time': 0,
                    'days_of_week': 1 << ((local.weekday() + 1) % 7),
                    'enabled': True, 'one_time': True, 'start_enabled': True, 'end_enabled': False}
        control = {'version': 1, 'vin': VIN, 'was_driving': False, 'baseline_pending': False, 'managed_limit': 80,
                   'guard': {'due': now - 1, 'expires': now + 179, 'planned_start': planned,
                             'expected_limit': 80, 'site': 'home', 'schedule': expected}}
        state_file = self.state / ('control-' + VIN + '.json')
        state_file.write_text(json.dumps(control))
        state_file.chmod(0o600)
        sites = self.state / 'sites.json'
        sites.write_text(json.dumps([{'site': 'home', 'latitude': 55, 'longitude': 12, 'radius_m': 250, 'time_zone': 'UTC'}]))
        fresh = json.loads(json.dumps(self.payload))
        fresh['response']['state'] = 'asleep'  # must not be used to discard the owed fresh check
        fresh['response']['drive_state'].update(latitude=55, longitude=12)
        fresh['response']['charge_state']['charging_state'] = 'Charging'
        fresh['response']['charge_schedule_data']['charge_schedules'] = [expected]
        slot = now // 900 * 900
        prices = {'results': [{'series': [{'columns': ['time', 'price'], 'values': [[slot + index * 900, 1] for index in range(193)]}]}]}
        result = self.run_controller(fresh, TESLA_SITES_FILE=str(sites), TEST_INFLUX_RESPONSE=json.dumps(prices), TEST_ALLOW_TELEMETRY='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = [url.rsplit('/', 1)[-1] for url, _ in self.requests() if '/command/' in url]
        self.assertEqual(commands, ['charge_stop'])
        self.assertEqual(sum('/vehicle_data' in url for url, _ in self.requests()), 1)
        self.assertFalse(any(url.endswith('/wake_up') or url.endswith('/api/1/vehicles/' + VIN) for url, _ in self.requests()))
        self.assertIsNone(json.loads(state_file.read_text())['guard'])


if __name__ == '__main__':
    unittest.main()

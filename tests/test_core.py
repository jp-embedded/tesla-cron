"""Pure quarter-hour pricing, geofencing, schedule ownership and control tests."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('g++'), 'C++ compiler required')
class CoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.driver = Path(cls.temporary.name) / 'core-tests'
        result = subprocess.run([
            'g++', '-std=c++17', '-O0', '-ffunction-sections', '-fdata-sections',
            '-DUSE_OS_TZDB=1', '-I' + str(ROOT), '-I' + str(ROOT / 'date/include'),
            *[str(ROOT / name) for name in ('tests/core.cpp', 'prices.cpp', 'schedules.cpp',
                                          'vehicle_data.cpp', 'control.cpp', 'control_state.cpp', 'location.cpp', 'date/src/tz.cpp')],
            '-Wl,--gc-sections', '-lcurlpp', '-lcurl', '-pthread', '-o', str(cls.driver),
        ], text=True, capture_output=True, timeout=120)
        if result.returncode:
            raise RuntimeError('Core test build failed:\n' + result.stderr)

    def case(self, name):
        result = subprocess.run([str(self.driver), name], text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


CASES = (
    'quarter_hour_window', 'hour_is_four_slots', 'partial_slot_weighting',
    'missing_slots_not_free', 'hourly_data_not_expanded', 'negative_prices',
    'no_invented_future_prices', 'deadline_and_ties', 'influx_columns_and_units',
    'influx_null_is_gap', 'influx_invalid_rows', 'influx_numeric_bounds', 'one_policy_multiple_locations',
    'schedule_payload_and_64bit_ids', 'timezone_and_dst', 'preserve_manual_schedules',
    'unchanged_schedules_no_commands', 'vehicle_cache_round_trip', 'malformed_vehicle_and_schedule',
    'calendar_departure_plan', 'manual_charging_not_interrupted', 'moving_and_disconnected',
    'low_battery_and_manual_limit', 'incomplete_prices_fail_closed', 'duration_and_token_expiry',
    'driving_baseline_once', 'baseline_charge_not_boosted_early', 'baseline_stopped_starts_only_to_fifty',
    'cheap_now_is_not_a_delayed_schedule', 'manual_charge_limit_is_not_raised', 'cancel_owned_idle_boost_only',
    'delayed_guard_eligibility', 'delayed_guard_fresh_state_checks', 'controller_state_round_trip', 'gear_and_zero_speed',
)

for name in CASES:
    setattr(CoreTests, 'test_' + name, lambda self, name=name: self.case(name))


if __name__ == '__main__':
    unittest.main()

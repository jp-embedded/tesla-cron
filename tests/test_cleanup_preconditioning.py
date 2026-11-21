"""Legacy cleanup scope/resumption/rate safety. All API operations are fakes."""

import base64
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
from urllib.error import HTTPError

from scripts import cleanup_preconditioning as cleanup


VIN = 'TESTVIN1234567890'


def entry(identity):
    return {'id': identity, 'precondition_time': 420, 'days_of_week': 127,
            'latitude': 55.0, 'longitude': 12.0, 'enabled': True, 'one_time': False}


class FakeJournal:
    def __init__(self):
        self.data = {'resolved': [], 'last_request': 0, 'wake_sent': False}
        self.saved = []
        self.reserved = 0

    def save(self):
        self.saved.append(json.loads(json.dumps(self.data)))

    def reserve_request(self):
        self.reserved += 1


class FakeAPI:
    def __init__(self, journal, entries):
        self.journal = journal
        self.entries = dict(entries)
        self.removed, self.calls = [], []
        self.reads = 0
        self.fail_after_removal = None
        self.asleep = False

    def health(self):
        self.calls.append('health')

    def snapshot(self, vin):
        self.reads += 1
        if self.asleep:
            raise cleanup.HTTPFailure(408, 'Fleet API')
        return dict(self.entries)

    def call(self, path, body):
        self.calls.append(path)
        self.asleep = False
        return {'response': {}}

    def remove(self, vin, identity):
        if self.journal.data.get('in_flight') != identity or not self.journal.saved:
            raise AssertionError('Must save in-flight state before network')
        self.removed.append(identity)
        del self.entries[identity]
        if identity == self.fail_after_removal:
            raise RuntimeError('Ambiguous command timeout')


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None)
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.approved = {1: entry(1), 2: entry(2)}
        self.journal = FakeJournal()

    def private_json(self, name, payload):
        path = self.directory / name
        path.write_text(json.dumps(payload))
        path.chmod(0o600)
        return path

    def cache(self):
        return self.private_json('vehicle.cache', {'tesla_cron_fetched_at': 1000,
            'response': {'vin': VIN, 'preconditioning_schedule_data': {'precondition_schedules': [entry(1), entry(2)]}}})

    def execute(self, api, allow_wake=False):
        with redirect_stdout(io.StringIO()), patch.object(cleanup.time, 'sleep'):
            cleanup.execute(api, self.journal, VIN, self.approved, allow_wake)

    def test_immutable_review_manifest_prepared_without_network(self):
        destination = self.directory / 'manifest.json'
        with redirect_stdout(io.StringIO()), patch.object(cleanup, 'build_opener') as network:
            cleanup.prepare(self.cache(), destination, 2)
        network.assert_not_called()
        vin, approved, digest = cleanup.load_manifest(destination, 2)
        self.assertEqual(vin, VIN)
        self.assertEqual(set(approved), {1, 2})
        self.assertEqual(len(digest), 64)
        self.assertEqual(destination.stat().st_mode & 0o777, 0o400)

    def test_manifest_count_and_duplicate_guards(self):
        with self.assertRaises(RuntimeError):
            cleanup.prepare(self.cache(), self.directory / 'manifest.json', 3)
        path = self.private_json('duplicates.json', {'version': 1, 'vin': VIN, 'schedules': [entry(1), entry(1)]})
        with self.assertRaises(RuntimeError):
            cleanup.load_manifest(path, 2)

    def test_private_file_guard(self):
        path = self.private_json('public.json', {})
        path.chmod(0o644)
        with self.assertRaises(RuntimeError):
            cleanup.private_json(path)
        link = self.directory / 'link'
        link.symlink_to(path)
        with self.assertRaises(RuntimeError):
            cleanup.private_json(link)

    def test_only_reviewed_intersection_deleted_new_entries_preserved(self):
        api = FakeAPI(self.journal, {**self.approved, 99: entry(99)})
        self.execute(api)
        self.assertEqual(api.removed, [1, 2])
        self.assertEqual(set(api.entries), {99})
        self.assertEqual(api.reads, 2)
        self.assertTrue(self.journal.data['verified_complete'])
        self.assertEqual(self.journal.data['removed_this_run'], 2)
        self.assertFalse(any('charge_start' in path or 'charge_stop' in path or 'remove_charge_schedule' in path for path in api.calls))

    def test_modified_reviewed_entry_stops_before_deletes(self):
        changed = entry(1)
        changed['precondition_time'] += 1
        api = FakeAPI(self.journal, {1: changed, 2: entry(2)})
        with self.assertRaises(RuntimeError):
            self.execute(api)
        self.assertEqual(api.removed, [])

    def test_double_roundoff_is_allowed_but_real_coordinate_edits_are_not(self):
        current = {identity: dict(value) for identity, value in self.approved.items()}
        current[1]['latitude'] += 1e-14
        self.assertEqual(cleanup.pending_ids(self.approved, current), [1, 2])
        current[1]['latitude'] += 1e-6
        with self.assertRaises(RuntimeError):
            cleanup.pending_ids(self.approved, current)

    def test_already_absent_ids_need_no_delete(self):
        api = FakeAPI(self.journal, {99: entry(99)})
        self.execute(api)
        self.assertEqual(api.removed, [])
        self.assertEqual(api.reads, 1)
        self.assertEqual(self.journal.data['already_absent_initially'], 2)

    def test_ambiguous_timeout_is_not_retried_and_resume_checks_state(self):
        api = FakeAPI(self.journal, self.approved)
        api.fail_after_removal = 1
        with self.assertRaises(RuntimeError):
            self.execute(api)
        self.assertEqual(api.removed, [1])
        self.assertEqual(self.journal.data['in_flight'], 1)
        api.fail_after_removal = None
        self.execute(api)
        self.assertEqual(api.removed, [1, 2])
        self.assertTrue(self.journal.data['verified_complete'])

    def test_wake_is_opt_in_and_at_most_once(self):
        api = FakeAPI(self.journal, self.approved)
        api.asleep = True
        with self.assertRaises(cleanup.HTTPFailure):
            self.execute(api)
        self.assertEqual(api.calls, ['health'])
        self.execute(api, allow_wake=True)
        self.assertEqual(sum(path.endswith('/wake_up') for path in api.calls), 1)
        self.assertTrue(self.journal.data['wake_sent'])
        api.asleep = True
        with self.assertRaises(cleanup.HTTPFailure):
            self.execute(api, allow_wake=True)
        self.assertEqual(sum(path.endswith('/wake_up') for path in api.calls), 1)

    def test_request_pacing_persists_across_instances(self):
        now = [1000.0]
        sleeps = []

        def sleep(seconds):
            sleeps.append(seconds)
            now[0] += seconds

        path = self.directory / 'journal.json'
        with patch.object(cleanup.time, 'time', side_effect=lambda: now[0]), patch.object(cleanup.time, 'sleep', side_effect=sleep):
            first = cleanup.Journal(path, 'digest')
            first.reserve_request()
            first.reserve_request()
            second = cleanup.Journal(path, 'digest')
            second.reserve_request()
        self.assertEqual(sleeps, [4.0, 4.0])
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_corrupt_journal_fails_closed(self):
        path = self.private_json('journal.json', {'manifest': 'digest', 'resolved': [], 'wake_sent': False, 'last_request': -1})
        with self.assertRaises(RuntimeError):
            cleanup.Journal(path, 'digest')

    def bare_api(self):
        api = cleanup.API.__new__(cleanup.API)
        api.state, api.journal = self.directory, self.journal
        api.audience, api.proxy = 'https://fleet-api.prd.eu.vn.cloud.tesla.com', 'https://127.0.0.1:4443'
        api.fleet = Mock()
        api.local = Mock()
        api.token = Mock(return_value='dummy-token')
        return api

    def test_rate_limit_saves_cooldown_without_retry(self):
        api = self.bare_api()
        api.fleet.open.side_effect = HTTPError('unused', 429, 'private server detail', {'Retry-After': '3600'}, None)
        with patch.object(cleanup.time, 'time', return_value=1000), self.assertRaises(cleanup.HTTPFailure):
            api.call('/api/1/vehicles/TEST/vehicle_data')
        self.assertEqual(api.fleet.open.call_count, 1)
        self.assertEqual(int((self.directory / 'api-not-before').read_text()), 4600)

    def test_active_or_corrupt_cooldown_prevents_network(self):
        api = self.bare_api()
        path = self.directory / 'api-not-before'
        for text in ('99999999999', 'bad', '-1'):
            with self.subTest(text=text):
                path.write_text(text)
                path.chmod(0o600)
                with self.assertRaises(RuntimeError):
                    api.call('/api/1/vehicles/TEST/vehicle_data')
        api.fleet.open.assert_not_called()

    def test_expired_token_is_not_sent(self):
        api = self.bare_api()
        del api.token
        path = self.directory / 'access_token.txt'
        for expiry in (900, 2000):
            payload = base64.urlsafe_b64encode(json.dumps({'exp': expiry}).encode()).decode().rstrip('=')
            path.write_text('header.' + payload + '.signature')
            path.chmod(0o600)
            with patch.object(cleanup.time, 'time', return_value=1000):
                if expiry == 900:
                    with self.assertRaises(RuntimeError):
                        api.token()
                else:
                    self.assertEqual(api.token(), path.read_text())

    def test_redirects_are_not_followed(self):
        self.assertIsNone(cleanup.NoRedirect().redirect_request(None, None, 302, '', {}, 'https://example.com'))

    def test_dangling_journal_and_cooldown_symlinks_fail_closed(self):
        path = self.directory / 'journal.json'
        path.symlink_to(self.directory / 'absent')
        with self.assertRaises(RuntimeError):
            cleanup.Journal(path, 'digest')
        api = self.bare_api()
        (self.directory / 'api-not-before').symlink_to(self.directory / 'also-absent')
        with self.assertRaises(RuntimeError):
            api.call('/api/1/vehicles/TEST/vehicle_data')
        api.fleet.open.assert_not_called()

    def test_incomplete_snapshot_is_not_mistaken_for_empty_schedule_list(self):
        with self.assertRaises(RuntimeError):
            cleanup.schedules({'response': {'preconditioning_schedule_data': {}}}, VIN)
        self.assertEqual(cleanup.schedules({'response': {'preconditioning_schedule_data': {'precondition_schedules': []}}}, VIN), {})

    def test_snapshot_diagnostics_are_private_and_do_not_require_another_read(self):
        api = self.bare_api()
        api.journal = cleanup.Journal(self.directory / 'progress', 'digest')
        api.call = Mock(return_value={'response': {'vin': VIN, 'preconditioning_schedule_data': {'precondition_schedules': [entry(1)]}}})
        self.assertEqual(set(api.snapshot(VIN)), {1})
        api.call.assert_called_once()
        path = self.directory / 'progress.snapshot'
        saved = cleanup.private_json(path)
        self.assertEqual(len(saved['schedules']), 1)
        self.assertEqual(saved['field_presence']['enabled'], 1)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)


if __name__ == '__main__':
    unittest.main()

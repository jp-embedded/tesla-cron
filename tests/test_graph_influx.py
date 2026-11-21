"""Read-only SVG rendering and output safety; no production Influx requests."""

from contextlib import redirect_stdout
import base64
import importlib.util
import io
import math
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlsplit

ROOT = Path(__file__).resolve().parents[1]
specification = importlib.util.spec_from_file_location('tesla_graph_influx', ROOT / 'scripts/graph_influx.py')
graph = importlib.util.module_from_spec(specification)
specification.loader.exec_module(graph)


class GraphInfluxTests(unittest.TestCase):
    def setUp(self):
        self.base = 1700000000
        self.data = [
            {'time': self.base, 'site': 'home', 'battery_level': 55, 'price_dkk_kwh': -0.125,
             'price_slot_start': self.base // 900 * 900, 'charge_window_level': 6,
             'charging': True, 'cached': False, 'event': True, 'next_event': self.base + 600},
            {'time': self.base + 300, 'site': 'other', 'battery_level': 56, 'price_dkk_kwh': 2.75,
             'price_slot_start': self.base // 900 * 900, 'charge_window_level': 5,
             'charging': True, 'cached': True, 'event': True, 'next_event': self.base + 600},
        ]

    def test_series_tags_and_columns(self):
        payload = {'results': [{'series': [{'tags': {'vin': 'TESTVIN', 'site': 'home'},
                    'columns': ['time', 'battery_level'], 'values': [[self.base, 55]]}]}]}
        self.assertEqual(graph.rows(payload), [{'vin': 'TESTVIN', 'site': 'home', 'time': self.base, 'battery_level': 55}])
        self.assertEqual(graph.rows({'results': [{}]}), [])

    def test_malformed_and_error_responses_rejected(self):
        for payload in ([], {}, {'results': [{'error': 'bad query'}]},
                        {'results': [{'series': None}]},
                        {'results': [{'series': [{'columns': ['time', 'time'], 'values': [[1, 2]]}]}]},
                        {'results': [{'series': [{'columns': ['time'], 'values': [[1, 2]]}]}]}):
            with self.subTest(payload=payload), self.assertRaises(RuntimeError):
                graph.rows(payload)

    def test_svg_units_cached_state_events_and_multiple_sites(self):
        svg = graph.render_svg('TESTVIN', self.data, self.base - 100, self.base + 1000)
        root = ET.fromstring(svg)
        self.assertIn('DKK/kWh', svg)
        self.assertIn('UTC', svg)
        self.assertIn('home, other', svg)
        self.assertIn('stroke-dasharray="4 3"', svg)
        events = [item for item in root.iter() if item.get('stroke') == '#b00020']
        self.assertEqual(len(events), 1, 'Repeated upcoming-event metadata must not duplicate the marker')

    def test_svg_escapes_tag_text(self):
        self.data[0]['site'] = '<script>alert("bad")</script>'
        svg = graph.render_svg('VIN<script>', self.data, self.base - 100, self.base + 1000)
        ET.fromstring(svg)
        self.assertNotIn('<script>', svg)
        self.assertIn('&lt;script&gt;', svg)

    def test_gaps_and_invalid_numbers_are_not_interpolated(self):
        self.data[1]['time'] = self.base + 7200
        self.data[1]['price_dkk_kwh'] = math.nan
        svg = graph.render_svg('TESTVIN', self.data, self.base - 100, self.base + 8000)
        root = ET.fromstring(svg)
        battery_paths = [item for item in root.iter() if item.get('stroke') == '#145ba0']
        self.assertEqual(battery_paths, [])
        self.assertNotIn('nan', svg)
        self.assertIsNone(graph.number({'value': True}, 'value'))

    def test_empty_data_still_renders_valid_svg(self):
        svg = graph.render_svg('TESTVIN', [], self.base - 86400, self.base)
        ET.fromstring(svg)
        self.assertIn('No telemetry in this period', svg)

    def test_missed_quarter_hour_is_not_bridged_at_boundary(self):
        for gap in (1740, 1800):
            with self.subTest(gap=gap):
                self.data[1]['time'] = self.base + gap
                root = ET.fromstring(graph.render_svg('TESTVIN', self.data, self.base - 100, self.base + 2000))
                self.assertEqual([item for item in root.iter() if item.get('stroke') == '#145ba0'], [])

    def test_query_string_escaping(self):
        self.assertEqual(graph.quoted("a'b\\c"), "'a\\'b\\\\c'")
        with self.assertRaises(ValueError):
            graph.quoted('bad\nvalue')

    def test_private_day_and_week_outputs(self):
        with tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None) as directory:
            output = Path(directory) / 'graphs'
            with patch.object(graph, 'query_influx', return_value=self.data) as query, \
                 patch.object(graph.time, 'time', return_value=self.base + 1000), redirect_stdout(io.StringIO()):
                self.assertEqual(graph.main(['--vin', 'TESTVIN', '--output-dir', str(output)]), 0)
            self.assertEqual(query.call_count, 1, 'Both graphs reuse the same week query')
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            for suffix in ('d', 'w'):
                path = output / ('tesla-TESTVIN-' + suffix + '.svg')
                ET.fromstring(path.read_text())
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)

    def test_public_permissions_are_explicit(self):
        with tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None) as directory:
            output = Path(directory) / 'graphs'
            with patch.object(graph, 'query_influx', return_value=self.data), redirect_stdout(io.StringIO()):
                graph.main(['--vin', 'TESTVIN', '--output-dir', str(output), '--public'])
            self.assertEqual((output / 'tesla-TESTVIN-d.svg').stat().st_mode & 0o777, 0o644)

    def test_vin_cannot_escape_output_directory(self):
        with patch.object(graph, 'query_influx') as query, self.assertRaises(ValueError):
            graph.main(['--vin', '../private'])
        query.assert_not_called()

    def test_missing_measurement_is_a_clean_noop(self):
        with patch.object(graph, 'query_influx', return_value=[]), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(graph.main([]), 0)
        self.assertIn('No tesla_cron telemetry', output.getvalue())

    def test_http_query_uses_reader_credentials_and_seconds(self):
        environment = {'INFLUX_URL': 'http://localhost:8086', 'INFLUX_DB': 'iot',
                       'INFLUX_USER': 'reader', 'INFLUX_PASS': 'read-pass',
                       'INFLUX_WRITE_USER': 'writer', 'INFLUX_WRITE_PASS': 'write-pass'}
        with patch.dict(os.environ, environment, clear=True), \
             patch.object(graph, 'urlopen', return_value=io.BytesIO(b'{"results":[{}]}')) as opened:
            self.assertEqual(graph.query_influx('SELECT * FROM "tesla_cron"'), [])
        request = opened.call_args.args[0]
        self.assertEqual(parse_qs(urlsplit(request.full_url).query)['epoch'], ['s'])
        self.assertEqual(request.get_header('Authorization'), 'Basic ' + base64.b64encode(b'reader:read-pass').decode())
        self.assertNotIn('read-pass', request.full_url)

    def test_http_errors_and_timeout_configuration_are_sanitized(self):
        with patch.dict(os.environ, {'INFLUX_TIMEOUT_MS': 'invalid'}, clear=True), \
             patch.object(graph, 'urlopen') as opened, self.assertRaises(RuntimeError):
            graph.query_influx('SELECT * FROM "tesla_cron"')
        opened.assert_not_called()
        with patch.dict(os.environ, {}, clear=True), \
             patch.object(graph, 'urlopen', side_effect=HTTPError('unused', 401, 'private detail', {}, None)), \
             self.assertRaisesRegex(RuntimeError, '^Influx graph query HTTP 401$'):
            graph.query_influx('SELECT * FROM "tesla_cron"')

    def test_default_private_outputs_reject_shared_or_symlinked_directories(self):
        with tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None) as directory:
            output = Path(directory) / 'shared'
            output.mkdir(mode=0o755)
            output.chmod(0o755)
            with patch.object(graph, 'query_influx') as query, self.assertRaises(ValueError), redirect_stdout(io.StringIO()):
                graph.main(['--vin', 'TESTVIN', '--output-dir', str(output)])
            query.assert_not_called()
            output.chmod(0o700)
            link = Path(directory) / 'link'
            link.symlink_to(output)
            with patch.object(graph, 'query_influx') as query, self.assertRaises(ValueError), redirect_stdout(io.StringIO()):
                graph.main(['--vin', 'TESTVIN', '--output-dir', str(link)])
            query.assert_not_called()


if __name__ == '__main__':
    unittest.main()

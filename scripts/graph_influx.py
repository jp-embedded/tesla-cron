#!/usr/bin/env python3
"""Render day/week SVGs from tesla_cron telemetry using only the standard library."""

import argparse
import base64
from datetime import datetime, timezone
from html import escape
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


def query_influx(query):
    base = os.environ.get('INFLUX_URL', 'http://localhost:8086').rstrip('/')
    url = base + '/query?' + urlencode({'db': os.environ.get('INFLUX_DB', 'iot'), 'epoch': 's', 'q': query})
    request = Request(url)
    user = os.environ.get('INFLUX_USER', '')
    if user:
        credentials = (user + ':' + os.environ.get('INFLUX_PASS', '')).encode()
        request.add_header('Authorization', 'Basic ' + base64.b64encode(credentials).decode())
    try:
        timeout = int(os.environ.get('INFLUX_TIMEOUT_MS', '10000'))
        if not 1 <= timeout <= 60000:
            raise ValueError()
    except ValueError:
        raise RuntimeError('INFLUX_TIMEOUT_MS must be between 1 and 60000') from None
    try:
        with urlopen(request, timeout=timeout / 1000) as response:
            payload = json.load(response)
    except HTTPError as error:
        raise RuntimeError('Influx graph query HTTP ' + str(error.code)) from None
    except (URLError, ValueError, OSError):
        raise RuntimeError('Influx graph query failed') from None
    return rows(payload)


def rows(payload):
    if not isinstance(payload, dict) or 'error' in payload or not isinstance(payload.get('results'), list):
        raise RuntimeError('Invalid Influx graph response')
    result = []
    for item in payload['results']:
        if not isinstance(item, dict) or 'error' in item:
            raise RuntimeError('Influx graph query failed')
        series_list = item.get('series', [])
        if not isinstance(series_list, list):
            raise RuntimeError('Invalid Influx graph series')
        for series in series_list:
            if not isinstance(series, dict) or not isinstance(series.get('tags', {}), dict) or not isinstance(series.get('values', []), list):
                raise RuntimeError('Invalid Influx graph series')
            columns = series.get('columns')
            if not isinstance(columns, list) or any(not isinstance(name, str) for name in columns) or len(set(columns)) != len(columns):
                raise RuntimeError('Invalid Influx graph columns')
            for values in series.get('values', []):
                if not isinstance(values, list) or len(values) != len(columns):
                    raise RuntimeError('Invalid Influx graph row')
                result.append({**series.get('tags', {}), **dict(zip(columns, values))})
    return result


def quoted(value):
    if any(character in value for character in '\r\n\0'):
        raise ValueError('Invalid Influx tag value')
    return "'" + value.replace('\\', '\\\\').replace("'", "\\'") + "'"


def number(row, field):
    value = row.get(field)
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    try:
        return value if math.isfinite(value) else None
    except OverflowError:
        return None


def render_svg(vin, data, begin, end):
    data = sorted((row for row in data if number(row, 'time') is not None and begin <= row['time'] <= end),
                  key=lambda row: row['time'])
    width, height, left, top, plot_width, plot_height = 960, 330, 60, 50, 825, 215
    x = lambda timestamp: left + (timestamp - begin) / (end - begin) * plot_width
    y = lambda percent: top + plot_height * (1 - percent / 100)
    prices = [number(row, 'price_dkk_kwh') for row in data if number(row, 'price_dkk_kwh') is not None]
    low, high = min([0] + prices), max([1] + prices)
    if not math.isfinite(high - low):
        raise ValueError('Invalid price range')
    price_y = lambda price: top + plot_height * (high - price) / (high - low)
    sites = ', '.join(sorted({str(row['site']) for row in data if row.get('site')}))
    title = vin + (' — ' + sites if sites else '')
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
           '<rect width="100%" height="100%" fill="white"/>',
           '<g font-family="sans-serif" font-size="11" fill="#333">',
           f'<title>{escape(title)}</title><text x="60" y="22" font-size="15">{escape(title)}</text>',
           '<text x="8" y="42">Battery %</text><text x="888" y="42">DKK/kWh</text>']
    events = set()
    for row in data:
        until = min(end, row['time'] + 900)
        level = number(row, 'charge_window_level')
        if level is not None and 1 <= level <= 6:
            svg.append(f'<rect x="{x(row["time"]):.2f}" y="{top}" width="{x(until)-x(row["time"]):.2f}" height="{plot_height}" fill="#49a34a" opacity="{0.08+level*0.025:.3f}"/>')
        if row.get('charging') is True:
            svg.append(f'<rect x="{x(row["time"]):.2f}" y="{top+plot_height-7}" width="{x(until)-x(row["time"]):.2f}" height="7" fill="#222"/>')
        event = number(row, 'next_event')
        if row.get('event') is True and event is not None and begin <= event <= end:
            events.add(event)
        price = number(row, 'price_dkk_kwh')
        if price is not None:
            slot = number(row, 'price_slot_start')
            slot = slot if slot is not None else int(row['time'] // 900) * 900
            start, finish = max(begin, slot), min(end, slot + 900)
            if start < finish:
                svg.append(f'<path d="M{x(start):.2f},{price_y(price):.2f} H{x(finish):.2f}" fill="none" stroke="#cf3030" stroke-width="1.5"/>')
    for percent in (0, 25, 50, 75, 100):
        line_y = y(percent)
        price_label = low + (high - low) * percent / 100
        svg.extend([f'<path d="M{left},{line_y:.2f} H{left+plot_width}" stroke="#ddd"/>',
                    f'<text x="25" y="{line_y+4:.2f}">{percent}</text>',
                    f'<text x="890" y="{line_y+4:.2f}">{price_label:.2f}</text>'])
    for previous, current in zip(data, data[1:]):
        a, b = number(previous, 'battery_level'), number(current, 'battery_level')
        if (a is None or b is None or not (0 <= a <= 100 and 0 <= b <= 100)
                or int(current['time'] // 900) - int(previous['time'] // 900) > 1):
            continue
        dashed = ' stroke-dasharray="4 3"' if current.get('cached') is not False else ''
        svg.append(f'<path d="M{x(previous["time"]):.2f},{y(a):.2f} H{x(current["time"]):.2f} V{y(b):.2f}" fill="none" stroke="#145ba0" stroke-width="2"{dashed}/>')
    for row in data:
        battery = number(row, 'battery_level')
        if battery is not None and 0 <= battery <= 100:
            svg.append(f'<circle cx="{x(row["time"]):.2f}" cy="{y(battery):.2f}" r="1.5" fill="#145ba0"/>')
    for event in sorted(events):
        svg.append(f'<path d="M{x(event):.2f},{top} V{top+plot_height}" stroke="#b00020" stroke-dasharray="3 3"/>')
    for index in range(5):
        timestamp = begin + (end - begin) * index / 4
        label = datetime.fromtimestamp(timestamp, timezone.utc).strftime('%m-%d %H:%M')
        svg.append(f'<text x="{x(timestamp):.2f}" y="286" text-anchor="middle">{label}</text>')
    svg.extend(['<text x="60" y="310">UTC · blue: battery (dashed=cached/unknown) · red: price · green: cheap window · black: charging · dashed red: event</text>',
                '</g>'])
    if not data:
        svg.append('<text x="480" y="160" text-anchor="middle" font-family="sans-serif">No telemetry in this period</text>')
    svg.append('</svg>')
    return '\n'.join(svg) + '\n'


def write_svg(path, content, public=False):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, prefix='.tesla-graph-', delete=False) as output:
            temporary = Path(output.name)
            output.write(content)
        temporary.chmod(0o644 if public else 0o600)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vin', action='append', help='Vehicle to render; otherwise discover telemetry VIN tags')
    parser.add_argument('--output-dir', type=Path, default=Path('/var/tmp/tesla-cron-graphs'))
    parser.add_argument('--public', action='store_true', help='Make generated SVGs world-readable for intentional static web serving')
    options = parser.parse_args(arguments)
    vins = options.vin
    if not vins:
        vins = sorted({row['value'] for row in query_influx('SHOW TAG VALUES FROM "tesla_cron" WITH KEY = "vin"')
                       if isinstance(row.get('value'), str)})
    if not vins:
        print('No tesla_cron telemetry found yet.')
        return 0
    if any(not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', vin) for vin in vins):
        raise ValueError('Invalid VIN for graph output filename')
    options.output_dir.mkdir(parents=True, exist_ok=True, mode=0o755 if options.public else 0o700)
    if options.output_dir.is_symlink() or options.output_dir.stat().st_uid != os.geteuid():
        raise ValueError('Graph output directory must be owned by the current user, not a symlink')
    if not options.public and options.output_dir.stat().st_mode & 0o077:
        raise ValueError('Use a private graph output directory, or --public for intentional web publishing')
    now = int(time.time())
    for vin in vins:
        query = ('SELECT * FROM "tesla_cron" WHERE "vin"=' + quoted(vin) +
                 f' AND time >= {now-604800}s AND time <= {now}s ORDER BY time ASC')
        data = query_influx(query)
        for suffix, seconds in (('d', 86400), ('w', 604800)):
            path = options.output_dir / ('tesla-' + vin + '-' + suffix + '.svg')
            write_svg(path, render_svg(vin, data, now-seconds, now), options.public)
            print(path)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, OSError) as error:
        print('Influx graphs: ' + str(error), file=sys.stderr)
        raise SystemExit(1)

"""Optional real loopback TLS checks, using throwaway keys and no Tesla endpoints.

Build .proxy/tesla-http-proxy first to enable these tests. Nothing is installed.
The C++ probe is compiled from current source, never an old application binary.
"""

import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROXY = ROOT / '.proxy/tesla-http-proxy'


@unittest.skipUnless(PROXY.is_file() and shutil.which('openssl') and shutil.which('g++'),
                     'Build the official proxy to enable optional loopback TLS tests')
class LocalProxyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        source = cls.directory / 'probe.cpp'
        source.write_text('''#include "tesla-api.h"
#include <iostream>
int main() {
    try {
        tesla_api api;
        api.check_proxy();
        std::cout << "Local health passed\\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
''')
        cls.probe = cls.directory / 'probe'
        result = subprocess.run([
            'g++', '-std=c++17', '-O0', '-ffunction-sections', '-fdata-sections',
            '-I' + str(ROOT), '-I' + str(ROOT / 'date/include'),
            str(ROOT / 'tesla-api.cpp'), str(source), '-Wl,--gc-sections',
            '-lcurlpp', '-lcurl', '-pthread', '-o', str(cls.probe),
        ], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError('Local TLS probe compilation failed:\n' + result.stderr)

        def certificate(name):
            result = subprocess.run([
                'openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes',
                '-keyout', str(cls.directory / (name + '.key')), '-out', str(cls.directory / (name + '.crt')),
                '-days', '1', '-sha256', '-subj', '/CN=tesla-cron-test',
                '-addext', 'subjectAltName=IP:127.0.0.1', '-addext', 'basicConstraints=critical,CA:FALSE',
            ], capture_output=True, text=True, timeout=15)
            if result.returncode:
                raise RuntimeError('Generating throwaway TLS test certificate failed')

        certificate('server')
        certificate('untrusted')
        generated = subprocess.run([
            'openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout',
            '-out', str(cls.directory / 'throwaway-command.key'),
        ], capture_output=True, text=True, timeout=10)
        if generated.returncode:
            raise RuntimeError('Generating throwaway command key failed')

        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            cls.port = reservation.getsockname()[1]
        cls.log = tempfile.TemporaryFile()
        cls.addClassCleanup(cls.log.close)
        environment = {key: value for key, value in os.environ.items() if not key.startswith('TESLA_')}
        environment['HOME'] = str(cls.directory)
        cls.server = subprocess.Popen([
            str(PROXY), '-host', '127.0.0.1', '-port', str(cls.port),
            '-cert', str(cls.directory / 'server.crt'), '-tls-key', str(cls.directory / 'server.key'),
            '-key-file', str(cls.directory / 'throwaway-command.key'), '-timeout', '3s',
        ], env=environment, stdout=cls.log, stderr=cls.log)
        cls.addClassCleanup(cls.stop_server)
        for _ in range(100):
            if cls.server.poll() is not None:
                raise RuntimeError('Local proxy exited before health tests')
            try:
                with socket.create_connection(('127.0.0.1', cls.port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError('Local proxy did not bind to loopback in time')

    @classmethod
    def stop_server(cls):
        if cls.server.poll() is None:
            cls.server.terminate()
            try:
                cls.server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                cls.server.kill()
                cls.server.wait(timeout=5)

    def health(self, **changes):
        environment = {
            **os.environ,
            'TESLA_PROXY_URL': f'https://127.0.0.1:{self.port}',
            'TESLA_PROXY_CA_FILE': str(self.directory / 'server.crt'),
            **changes,
        }
        return subprocess.run([str(self.probe)], env=environment, capture_output=True, text=True, timeout=10)

    def test_real_proxy_health_with_explicit_trust(self):
        result = self.health()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_certificate_is_rejected(self):
        result = self.health(TESLA_PROXY_CA_FILE=str(self.directory / 'untrusted.crt'))
        self.assertNotEqual(result.returncode, 0)

    def test_hostname_mismatch_is_rejected(self):
        # The throwaway server certificate intentionally has only an IP SAN.
        result = self.health(TESLA_PROXY_URL=f'https://localhost:{self.port}')
        self.assertNotEqual(result.returncode, 0)

    def test_environment_proxy_cannot_capture_local_requests(self):
        result = self.health(HTTPS_PROXY='http://127.0.0.1:1', https_proxy='http://127.0.0.1:1',
                             ALL_PROXY='http://127.0.0.1:1', NO_PROXY='', no_proxy='')
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()

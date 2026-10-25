"""LAN development fixture, NOT a real printer. No commands or file writes.
Run: python tools/mock_moonraker.py --port 7125 [--api-key test-key]
Then add your computer's LAN IP in the ESP32 dashboard. Allow the port in its firewall.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import time
from urllib.parse import urlparse

args = argparse.ArgumentParser()
args.add_argument('--port', type=int, default=7125)
args.add_argument('--api-key', default='')
config = args.parse_args()
started = time.monotonic()


class Mock(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def answer(self, value, code=200):
        raw = json.dumps(value).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if config.api_key and self.headers.get('X-Api-Key') != config.api_key:
            return self.answer({'error': {'message': 'Unauthorized'}}, 401)
        path = urlparse(self.path).path
        result = {
            '/server/info': {'moonraker_version': 'mock-0.1', 'api_version': [1, 5, 0], 'klippy_state': 'ready', 'klippy_connected': True},
            '/printer/info': {'hostname': 'MOCK-Ender', 'state': 'ready', 'software_version': 'mock'},
            '/printer/objects/list': {'objects': ['print_stats', 'extruder', 'heater_bed', 'virtual_sdcard', 'toolhead', 'fan', 'temperature_sensor chamber']},
            '/server/files/metadata': {'filename': 'demo.gcode', 'estimated_time': 3600, 'filament_total': 1200, 'layer_height': .2},
            '/machine/proc_stats': {'cpu_temp': 45, 'moonraker_stats': [{'cpu_usage': 5}]},
        }.get(path)
        self.answer({'result': result}, 200) if result is not None else self.answer({'error': 'not found'}, 404)

    def do_POST(self):
        if config.api_key and self.headers.get('X-Api-Key') != config.api_key:
            return self.answer({'error': 'Unauthorized'}, 401)
        if self.path != '/printer/objects/query':
            return self.answer({'error': 'No printer control is implemented'}, 404)
        size = int(self.headers.get('Content-Length', 0))
        if size > 8192:
            return self.answer({'error': 'too large'}, 413)
        requested = json.loads(self.rfile.read(size))['objects']
        elapsed = time.monotonic() - started
        objects = {'print_stats': {'state': 'printing', 'filename': 'demo.gcode', 'print_duration': elapsed, 'filament_used': elapsed/3},
                   'extruder': {'temperature': 205 + 2*math.sin(elapsed/10), 'target': 205, 'power': .5},
                   'heater_bed': {'temperature': 60 + math.sin(elapsed/20), 'target': 60, 'power': .3},
                   'virtual_sdcard': {'progress': (elapsed % 3600)/3600, 'is_active': True},
                   'toolhead': {'position': [10, 20, elapsed/100, 0], 'homed_axes': 'xyz'},
                   'fan': {'speed': .8}, 'temperature_sensor chamber': {'temperature': 35}}
        self.answer({'result': {'eventtime': elapsed, 'status': {k: v for k,v in objects.items() if k in requested}}})


print(f'Mock Moonraker listening on 0.0.0.0:{config.port}; Ctrl+C to stop')
ThreadingHTTPServer(('0.0.0.0', config.port), Mock).serve_forever()

import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import time

import pytest
from fastapi.testclient import TestClient

path = Path(__file__).resolve().parents[1] / 'server/app.py'
spec = importlib.util.spec_from_file_location('printfleet_app', path)
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)
INGEST = {'Authorization': 'Bearer ' + 'i' * 40}
VIEW = {'Authorization': 'Bearer ' + 'v' * 40}


@pytest.fixture
def client(tmp_path, monkeypatch):
    monkeypatch.setenv('INGEST_TOKEN', 'i' * 40)
    monkeypatch.setenv('DASHBOARD_TOKEN', 'v' * 40)
    monkeypatch.setattr(a, 'DB', str(tmp_path / 'fleet.db'))
    with TestClient(a.app) as c:
        yield c


def sample(seq=1, **kwargs):
    return dict(schema_version=1, collector_id='collector1', boot_id='boot1', seq=seq,
                printer_id='printer1', captured_at=int(time.time()), uptime_ms=1000*seq,
                name='Ender 5', model='Ender 5 Max', host='192.168.1.50', online=True,
                data={'objects': {'extruder': {'temperature': 205.5}, 'heater_bed': {'temperature': 60},
                                  'virtual_sdcard': {'progress': .25}, 'temperature_sensor chamber': {'temperature': 35}}}, **kwargs)


def post(c, s):
    return c.post('/api/v1/telemetry', json=s, headers=INGEST)


def test_auth_and_role_separation(client):
    assert client.post('/api/v1/telemetry', json=sample()).status_code == 401
    assert client.post('/api/v1/telemetry', json=sample(), headers=VIEW).status_code == 401
    assert client.get('/api/v1/printers', headers=INGEST).status_code == 401
    assert client.get('/api/v1/printers').status_code == 401
    assert client.get('/healthz').json() == {'status': 'ok'}


def test_ack_idempotency_and_arbitrary_objects(client):
    s = sample()
    result = post(client, s)
    assert result.json() == {'accepted': True, 'duplicate': False, 'boot_id': 'boot1', 'seq': 1}
    assert post(client, s).json()['duplicate'] is True
    with a.connect() as db:
        assert db.execute('SELECT COUNT(*) FROM samples').fetchone()[0] == 1
    row = client.get('/api/v1/printers', headers=VIEW).json()['printers'][0]
    assert row['data']['objects']['temperature_sensor chamber']['temperature'] == 35


def test_late_retry_does_not_regress_live_state(client):
    post(client, sample(2))
    post(client, sample(1))
    assert client.get('/api/v1/printers', headers=VIEW).json()['printers'][0]['seq'] == 2
    s = sample(1)
    s['boot_id'] = 'earlier_boot'
    s['captured_at'] -= 100
    post(client, s)
    assert client.get('/api/v1/printers', headers=VIEW).json()['printers'][0]['boot_id'] == 'boot1'


def test_reboot_sequence_and_collector_isolation(client):
    post(client, sample(100))
    s = sample(1); s['boot_id'] = 'boot2'; post(client, s)
    t = sample(1); t['collector_id'] = 'collector2'; post(client, t)
    rows = client.get('/api/v1/printers', headers=VIEW).json()['printers']
    assert len(rows) == 2
    assert rows[0]['boot_id'] == 'boot2'


def test_validation_and_byte_limits(client):
    s = sample(); s['seq'] = -1
    assert post(client, s).status_code == 422
    s = sample(); s['printer_id'] = '../bad'
    assert post(client, s).status_code == 422
    s = sample(); s['secret'] = 'should never be uploaded'
    assert post(client, s).status_code == 422
    s = sample(); s['data']['huge'] = 'a' * 40000
    assert post(client, s).status_code == 413
    assert client.post('/api/v1/telemetry', content='not json', headers=INGEST).status_code == 422


def test_history_covers_time_range_and_null_offline(client):
    now = int(time.time())
    s = sample(); s['captured_at'] = now-3500; post(client, s)
    s = sample(2); s['online'] = False; s['captured_at'] = now-1800; s['data'] = {}; post(client, s)
    s = sample(3); s['captured_at'] = now; post(client, s)
    r = client.get('/api/v1/history?collector_id=collector1&printer_id=printer1&hours=1&limit=20', headers=VIEW)
    assert r.status_code == 200
    points = r.json()['points']
    assert len(points) == 3
    assert points[0]['nozzle'] == 205.5 and points[1]['nozzle'] is None
    assert points[-1]['progress'] == .25
    assert client.get('/api/v1/history?collector_id=collector1&printer_id=printer1&hours=1000', headers=VIEW).status_code == 422


def test_retention_preserves_latest_and_restart_preserves_database(client):
    post(client, sample())
    with a.connect() as db:
        db.execute('UPDATE samples SET received_at=?', (int(time.time())-31*86400,))
    a.prune()
    a.initialize()
    with a.connect() as db:
        assert db.execute('SELECT COUNT(*) FROM samples').fetchone()[0] == 0
        assert db.execute('SELECT COUNT(*) FROM latest').fetchone()[0] == 1


def test_ui_contains_no_cdn_or_credentials(client):
    r = client.get('/')
    assert r.status_code == 200 and 'PrintFleet' in r.text
    assert 'https://cdn.' not in r.text
    assert 'frame-ancestors' in r.headers['content-security-policy']
    assert 'i' * 40 not in r.text

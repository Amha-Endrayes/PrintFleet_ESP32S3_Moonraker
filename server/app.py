"""PrintFleet collector API. One uvicorn worker; SQLite resides on a persistent volume."""
import asyncio
from contextlib import asynccontextmanager
from datetime import datetime, timezone
import hmac
import json
import os
from pathlib import Path
import re
import sqlite3
import time

from fastapi import FastAPI, Depends, HTTPException, Query, Request
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field, ConfigDict
from starlette.concurrency import run_in_threadpool

DB = os.getenv('DATABASE_PATH', '/data/printfleet.db')
RETENTION = int(os.getenv('RETENTION_DAYS', '30'))
MAX_BODY = 32768
ID = r'^[A-Za-z0-9_-]{1,64}$'


def connect():
    db = sqlite3.connect(DB, timeout=10)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA busy_timeout=10000')
    return db


def initialize():
    Path(DB).parent.mkdir(parents=True, exist_ok=True)
    with connect() as db:
        db.execute('PRAGMA journal_mode=WAL')
        db.executescript('''
        CREATE TABLE IF NOT EXISTS samples (
          collector_id TEXT NOT NULL, boot_id TEXT NOT NULL, seq INTEGER NOT NULL,
          printer_id TEXT NOT NULL, captured_at INTEGER, uptime_ms INTEGER NOT NULL,
          received_at INTEGER NOT NULL, payload TEXT NOT NULL,
          PRIMARY KEY(collector_id, boot_id, seq));
        CREATE INDEX IF NOT EXISTS by_printer ON samples(collector_id, printer_id, received_at DESC);
        CREATE INDEX IF NOT EXISTS by_received ON samples(received_at);
        CREATE TABLE IF NOT EXISTS latest (
          collector_id TEXT NOT NULL, printer_id TEXT NOT NULL, received_at INTEGER NOT NULL,
          captured_at INTEGER, payload TEXT NOT NULL,
          PRIMARY KEY(collector_id, printer_id));
        ''')


def prune():
    # Rows are removed in bounded transactions; latest is intentionally retained.
    cutoff = int(time.time()) - RETENTION * 86400
    while True:
        with connect() as db:
            n = db.execute('DELETE FROM samples WHERE rowid IN (SELECT rowid FROM samples WHERE received_at < ? LIMIT 2000)', (cutoff,)).rowcount
        if n < 2000:
            break


async def maintenance():
    while True:
        await asyncio.sleep(3600)
        await run_in_threadpool(prune)


@asynccontextmanager
async def lifespan(app):
    if len(os.getenv('INGEST_TOKEN', '')) < 32 or len(os.getenv('DASHBOARD_TOKEN', '')) < 32:
        raise RuntimeError('Set distinct INGEST_TOKEN and DASHBOARD_TOKEN, each at least 32 characters')
    if os.environ['INGEST_TOKEN'] == os.environ['DASHBOARD_TOKEN']:
        raise RuntimeError('Use distinct ingest and dashboard tokens')
    if not 1 <= RETENTION <= 3650:
        raise RuntimeError('RETENTION_DAYS must be between 1 and 3650')
    initialize()
    await run_in_threadpool(prune)
    task = asyncio.create_task(maintenance())
    yield
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


app = FastAPI(title='PrintFleet', lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)


@app.middleware('http')
async def bounded_body(request, call_next):
    # Enforce actual bytes, including chunked bodies, before JSON parsing.
    if request.method == 'POST':
        body = bytearray()
        async for chunk in request.stream():
            body.extend(chunk)
            if len(body) > MAX_BODY:
                from starlette.responses import JSONResponse
                return JSONResponse({'detail': 'Payload too large'}, status_code=413)
        request._body = bytes(body)
    response = await call_next(request)
    response.headers['Cache-Control'] = 'no-store'
    response.headers['X-Content-Type-Options'] = 'nosniff'
    response.headers['Referrer-Policy'] = 'no-referrer'
    response.headers['Content-Security-Policy'] = "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'"
    return response


def auth(env):
    async def check(request: Request):
        expected = os.getenv(env, '')
        supplied = request.headers.get('authorization', '')
        if not expected or not hmac.compare_digest(supplied, 'Bearer ' + expected):
            raise HTTPException(401, 'Invalid token')
    return check


class Sample(BaseModel):
    model_config = ConfigDict(extra='forbid')
    schema_version: int = Field(ge=1, le=1)
    collector_id: str = Field(pattern=ID)
    boot_id: str = Field(pattern=ID)
    seq: int = Field(ge=1, le=9223372036854775807)
    printer_id: str = Field(pattern=ID)
    captured_at: int | None = Field(default=None, ge=1700000000, le=4102444800)
    uptime_ms: int = Field(ge=0)
    name: str = Field(max_length=96)
    model: str = Field(default='', max_length=96)
    host: str = Field(max_length=253)
    online: bool
    error: str = Field(default='', max_length=256)
    data: dict
    diagnostics: dict = Field(default_factory=dict)


def save(sample):
    now = int(time.time())
    data = sample.model_dump()
    encoded = json.dumps(data, separators=(',', ':'), allow_nan=False)
    with connect() as db:
        inserted = db.execute('INSERT OR IGNORE INTO samples VALUES (?,?,?,?,?,?,?,?)', (
            sample.collector_id, sample.boot_id, sample.seq, sample.printer_id,
            sample.captured_at, sample.uptime_ms, now, encoded)).rowcount
        if inserted:
            # Arrival time is not observation time: delayed retries must not regress live state.
            old = db.execute('SELECT payload FROM latest WHERE collector_id=? AND printer_id=?', (sample.collector_id, sample.printer_id)).fetchone()
            previous = json.loads(old['payload']) if old else None
            newer = previous is None
            if previous:
                if previous['boot_id'] == sample.boot_id:
                    newer = sample.seq > previous['seq']
                elif sample.captured_at and previous.get('captured_at'):
                    newer = sample.captured_at >= previous['captured_at']
                else:
                    newer = True  # no clock across boots: arrival order is the only available order
            if newer:
                db.execute('INSERT OR REPLACE INTO latest VALUES (?,?,?,?,?)', (
                    sample.collector_id, sample.printer_id, now, sample.captured_at, encoded))
    return {'accepted': True, 'duplicate': not bool(inserted), 'boot_id': sample.boot_id, 'seq': sample.seq}


@app.post('/api/v1/telemetry', dependencies=[Depends(auth('INGEST_TOKEN'))])
def ingest(sample: Sample):
    return save(sample)


@app.get('/healthz')
def health():
    with connect() as db:
        db.execute('SELECT 1').fetchone()
    return {'status': 'ok'}


@app.get('/api/v1/printers', dependencies=[Depends(auth('DASHBOARD_TOKEN'))])
def printers():
    with connect() as db:
        rows = db.execute('SELECT * FROM latest ORDER BY collector_id, printer_id').fetchall()
    now = int(time.time())
    return {'printers': [dict(json.loads(r['payload']), received_at=r['received_at'],
        age_s=now - (r['captured_at'] or r['received_at'])) for r in rows]}


@app.get('/api/v1/history', dependencies=[Depends(auth('DASHBOARD_TOKEN'))])
def history(collector_id: str = Query(pattern=ID), printer_id: str = Query(pattern=ID),
            hours: int = Query(default=24, ge=1, le=168), limit: int = Query(default=600, ge=1, le=2000)):
    # SQL bucket averaging preserves the entire requested period within a bounded result.
    start = int(time.time()) - hours * 3600
    width = max(1, (hours * 3600 + limit - 1) // limit)
    with connect() as db:
        rows = db.execute('''SELECT (COALESCE(captured_at,received_at)/?)*? AS t,
            AVG(CASE WHEN json_extract(payload,'$.online')=1 THEN json_extract(payload,'$.data.objects.extruder.temperature') END) AS nozzle,
            AVG(CASE WHEN json_extract(payload,'$.online')=1 THEN json_extract(payload,'$.data.objects.heater_bed.temperature') END) AS bed,
            AVG(CASE WHEN json_extract(payload,'$.online')=1 THEN json_extract(payload,'$.data.objects.virtual_sdcard.progress') END) AS progress,
            COUNT(*) AS samples
            FROM samples WHERE collector_id=? AND printer_id=?
            AND received_at>=? AND COALESCE(captured_at,received_at)>=?
            GROUP BY t ORDER BY t LIMIT ?''', (width, width, collector_id, printer_id, start, start, limit)).fetchall()
    return {'points': [dict(r) for r in rows], 'bucket_seconds': width}


@app.get('/')
def dashboard():
    return FileResponse(Path(__file__).parent / 'static' / 'index.html')

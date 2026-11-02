"""Persistent ownership and credentials. Device IDs never grant access alone."""
import hashlib
import hmac
import secrets
import sqlite3
import time
from pathlib import Path


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()


def password_hash(password, salt=None):
    salt = salt or secrets.token_hex(16)
    value = hashlib.scrypt(password.encode(), salt=salt.encode(), n=16384, r=8, p=1).hex()
    return salt + ":" + value


def password_matches(password, encoded):
    return hmac.compare_digest(password_hash(password, encoded.split(":")[0]), encoded)


class Store:
    def __init__(self, path):
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(path)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.executescript("""
        CREATE TABLE IF NOT EXISTS tenants(id TEXT PRIMARY KEY, name TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS users(
            email TEXT PRIMARY KEY, password TEXT NOT NULL, tenant_id TEXT NOT NULL REFERENCES tenants(id));
        CREATE TABLE IF NOT EXISTS sessions(
            token_hash TEXT PRIMARY KEY, email TEXT NOT NULL REFERENCES users(email), expires REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS devices(
            id TEXT PRIMARY KEY, secret_hash TEXT NOT NULL, model TEXT NOT NULL,
            tenant_id TEXT REFERENCES tenants(id), claim_code TEXT NOT NULL,
            claim_expires REAL NOT NULL, last_seen REAL NOT NULL DEFAULT 0,
            status TEXT NOT NULL DEFAULT '{}', image BLOB, image_crc INTEGER,
            image_request TEXT, image_state TEXT NOT NULL DEFAULT 'none');
        """)
        self.db.commit()

    def one(self, sql, args=()):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    def write(self, sql, args=()):
        with self.db:
            return self.db.execute(sql, args)

    def signup(self, email, password, tenant_name):
        tenant = secrets.token_hex(12)
        encoded = password_hash(password)
        with self.db:
            self.db.execute("INSERT INTO tenants VALUES(?,?)", (tenant, tenant_name))
            self.db.execute("INSERT INTO users VALUES(?,?,?)", (email, encoded, tenant))
        return tenant

    def user(self, token):
        if not token:
            return None
        return self.one("""SELECT users.email, users.tenant_id, tenants.name AS tenant_name
            FROM sessions JOIN users ON sessions.email=users.email
            JOIN tenants ON users.tenant_id=tenants.id
            WHERE token_hash=? AND expires>?""", (digest(token), time.time()))

    def session(self, email):
        token = secrets.token_urlsafe(32)
        self.write("DELETE FROM sessions WHERE expires<=?", (time.time(),))
        self.write("INSERT INTO sessions VALUES(?,?,?)", (digest(token), email, time.time() + 86400))
        return token

    def device(self, device_id):
        return self.one("SELECT * FROM devices WHERE id=?", (device_id,))

    def device_auth(self, device_id, secret):
        row = self.device(device_id)
        return bool(row and hmac.compare_digest(row["secret_hash"], digest(secret)))

    def register(self, device_id, secret, model):
        existing = self.device(device_id)
        if existing and not self.device_auth(device_id, secret):
            raise PermissionError("设备凭据不匹配")
        if not existing:
            code = secrets.token_hex(6).upper()
            self.write("""INSERT INTO devices(id,secret_hash,model,claim_code,claim_expires)
                VALUES(?,?,?,?,?)""", (device_id, digest(secret), model, code, time.time() + 900))
        elif not existing["tenant_id"] and existing["claim_expires"] <= time.time():
            self.write("UPDATE devices SET claim_code=?,claim_expires=? WHERE id=?",
                       (secrets.token_hex(6).upper(), time.time() + 900, device_id))
        return self.device(device_id)

    def claim(self, device_id, code, tenant_id):
        # Conditional UPDATE makes simultaneous claims atomic.
        cursor = self.write("""UPDATE devices SET tenant_id=?,claim_code='',claim_expires=0
            WHERE id=? AND tenant_id IS NULL AND claim_code=? AND claim_expires>?""",
            (tenant_id, device_id, code, time.time()))
        return cursor.rowcount == 1

    def owned(self, device_id, tenant_id):
        return self.one("SELECT * FROM devices WHERE id=? AND tenant_id=?", (device_id, tenant_id))

    def devices(self, tenant_id):
        return [dict(row) for row in self.db.execute("""SELECT id,model,last_seen,status,image_state,
            image_crc,image_request FROM devices WHERE tenant_id=? ORDER BY id""", (tenant_id,))]

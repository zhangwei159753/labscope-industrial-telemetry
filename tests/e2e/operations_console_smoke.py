"""Compose-backed HTTP acceptance smoke for the operations console.

Run with LABSCOPE_E2E_USERNAME and LABSCOPE_E2E_PASSWORD set, and optionally
LABSCOPE_E2E_BASE_URL / LABSCOPE_E2E_SITE_ID. This test does not provision data.
"""

import http.cookiejar
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request


class ConsoleClient:
    def __init__(self, base_url: str):
        self.base_url = base_url.rstrip("/")
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.cookies))
        self.csrf = ""

    def request(self, path: str, method: str = "GET", payload=None):
        headers = {"Accept": "application/json"}
        data = None
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if method not in ("GET", "HEAD") and self.csrf:
            headers["X-CSRF-Token"] = self.csrf
        request = urllib.request.Request(self.base_url + path, data=data,
                                         headers=headers, method=method)
        try:
            with self.opener.open(request, timeout=5) as response:
                body = response.read()
                content_type = response.headers.get_content_type()
        except urllib.error.HTTPError as error:
            raise AssertionError(f"{method} {path}: HTTP {error.code}: {error.read()[:256]!r}") from error
        if content_type == "application/json":
            return json.loads(body)
        return body.decode("utf-8", errors="replace")

    def login(self, username: str, password: str):
        session = self.request("/api/v1/auth/login", "POST",
                               {"username": username, "password": password})
        self.csrf = session["csrf_token"]
        return session


def main() -> int:
    username = os.environ.get("LABSCOPE_E2E_USERNAME", "")
    password = os.environ.get("LABSCOPE_E2E_PASSWORD", "")
    if not username or not password:
        print("Set LABSCOPE_E2E_USERNAME and LABSCOPE_E2E_PASSWORD first.", file=sys.stderr)
        return 2
    client = ConsoleClient(os.environ.get("LABSCOPE_E2E_BASE_URL", "http://127.0.0.1:8080"))
    session = client.login(username, password)
    assert session["user"]["role"] in ("admin", "operator", "viewer")
    assert client.request("/api/v1/auth/me")["user"]["username"] == username
    sites = client.request("/api/v1/sites")["sites"]
    site_id = os.environ.get("LABSCOPE_E2E_SITE_ID") or (sites[0]["site_id"] if sites else "")
    if site_id:
        escaped_site = urllib.parse.quote_plus(site_id)
        client.request(f"/api/v1/overview?site_id={escaped_site}")
        client.request(f"/api/v1/devices?site_id={escaped_site}&limit=10")
        client.request(f"/api/v1/alarms/events?site_id={escaped_site}&limit=10")
        client.request(f"/api/v1/alarms/rules?site_id={escaped_site}&limit=10")
    health = client.request("/api/v1/system/health")
    assert health["database"] == "healthy"
    assert "mqtt" in health and "ingestion_queue_depth" in health
    metrics = client.request("/metrics")
    for name in ("labscope_telemetry_persisted_total", "labscope_ingestion_queue_depth",
                 "labscope_mqtt_connected", "labscope_alarm_events_active"):
        assert name in metrics, f"missing Prometheus metric {name}"
    page = client.request("/")
    assert "LabScope" in page or "lab scope" in page.lower()
    print(f"operations console smoke passed for user={username}, site={site_id or '(none)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

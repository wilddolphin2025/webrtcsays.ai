"""Load the existing static TURN account for directcall.

The headset reads the bearer token file and GETs the authenticated
/demo/turn endpoint on the public rtc host. Credentials are written to a
mode 0600 turns file. They are not placed in a URL, an argv element, or a log
line. A missing account does not invent one.
"""

from __future__ import annotations

import json
import os
import re
import ssl
import urllib.error
import urllib.request
from pathlib import Path

TURN_ENDPOINT = "https://rtc.wilddolphin.us/demo/turn"
ALLOWED_TURN_HOSTS = frozenset({"rtc.wilddolphin.us"})
TURN_URI_RE = re.compile(
    r"(turns?):([A-Za-z0-9.-]+)(?::(\d{1,5}))?(?:\?transport=(udp|tcp))?\Z"
)
NOTE_CONFIGURED = "TURN configured"
NOTE_MISSING = "TURN not configured"


def valid_turn_uri(uri: object) -> bool:
    if not isinstance(uri, str) or "@" in uri or " " in uri:
        return False
    match = TURN_URI_RE.fullmatch(uri)
    if not match:
        return False
    host = match.group(2).lower()
    if host not in ALLOWED_TURN_HOSTS:
        return False
    port = match.group(3)
    if port is not None and not (1 <= int(port) <= 65535):
        return False
    return True


def _clean_secret(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text or len(text) > 256 or any(ord(ch) < 32 or ord(ch) > 126 for ch in text):
        return None
    if ";" in text or "\n" in text or "\r" in text:
        return None
    return text


def account_from_payload(payload: object) -> dict | None:
    """Return urls/username/credential, or None. Never includes a secret in an error."""
    if not isinstance(payload, dict):
        return None
    if payload.get("mode") != "static" or payload.get("temporary") is not True:
        return None
    if payload.get("minted") is True or payload.get("rest_api") is True:
        return None
    username = _clean_secret(payload.get("username"))
    credential = _clean_secret(payload.get("credential"))
    listed = payload.get("uris")
    if listed is None:
        listed = payload.get("urls")
    if username is None or credential is None or not isinstance(listed, list):
        return None
    urls = [item for item in listed if valid_turn_uri(item)]
    if not urls:
        return None
    return {"urls": urls, "username": username, "credential": credential}


def turn_hosts(account: dict) -> str:
    hosts = []
    for url in account.get("urls") or []:
        match = TURN_URI_RE.fullmatch(url)
        if not match:
            continue
        host = match.group(2).lower()
        if host not in hosts:
            hosts.append(host)
    return ",".join(hosts)


def read_token(path: Path) -> str:
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError:
        return ""
    return text.strip()


def fetch_turn_account(url: str, token: str):
    """GET the authenticated TURN document. Returns a dict or None. Does not log."""
    if not token or any(ch in token for ch in "\r\n"):
        return None
    request = urllib.request.Request(
        url,
        headers={
            "Authorization": "Bearer " + token,
            "Accept": "application/json",
        },
        method="GET",
    )

    class _NoRedirect(urllib.request.HTTPErrorProcessor):
        def http_response(self, request, response):
            return response

        https_response = http_response

    opener = urllib.request.build_opener(
        _NoRedirect,
        urllib.request.HTTPSHandler(context=ssl.create_default_context()),
    )
    try:
        with opener.open(request, timeout=5) as response:
            status = getattr(response, "status", None) or response.getcode()
            if status != 200:
                return None
            raw = response.read(8192)
    except (OSError, urllib.error.URLError, TimeoutError):
        return None
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict):
        return None
    return payload


def write_turns_file(path: Path, account: dict) -> Path:
    path = Path(path)
    if ".config" in path.parts:
        raise ValueError("turns file stays under the babelfish app directory")
    path.parent.mkdir(parents=True, exist_ok=True)
    os.chmod(path.parent, 0o700)
    temporary = path.with_name(path.name + ".tmp")
    blob = json.dumps(account, separators=(",", ":")).encode("utf-8")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, blob)
    finally:
        os.close(fd)
    os.chmod(temporary, 0o600)
    os.replace(temporary, path)
    os.chmod(path, 0o600)
    return path


def prepare_turns_file(token_path, dest, fetch=None, endpoint=TURN_ENDPOINT):
    """Write a turns file when the endpoint returns a usable static account.

    fetch(url, token) is injectable so tests never contact the public host.
    The returned note is safe to display.
    """
    token = read_token(token_path)
    payload = None
    if token and fetch is not None:
        try:
            payload = fetch(endpoint, token)
        except Exception:
            payload = None
    account = account_from_payload(payload)
    if account is None:
        return None, NOTE_MISSING
    path = write_turns_file(Path(dest), account)
    hosts = turn_hosts(account)
    note = NOTE_CONFIGURED if not hosts else NOTE_CONFIGURED + " host=" + hosts + " tls_verify=on"
    return path, note

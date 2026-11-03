#!/usr/bin/env python3
"""Portable SysDVR WebOS log collector for Windows, macOS and Linux (Python 3.9+).

Finds LG TVs via SSDP/hostnames, remembers TVs after successful downloads,
installs Paramiko privately to log_DATA/vendor and fetches the latest log via
webOS Developer Mode SSH. Node.js and the webOS SDK are not required.
"""

from __future__ import annotations

import base64
import datetime as dt
import getpass
import importlib
import ipaddress
import io
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import xml.etree.ElementTree as ET

APP_ID = "io.github.sysdvrwebos.client"
LOG_PATH = f"/media/developer/apps/usr/palm/applications/{APP_ID}/sysdvr-webos.log"
ROOT = Path(__file__).resolve().parent
DATA = ROOT / "log_DATA"
KEYS = DATA / "keys"
PROFILES = DATA / "tvs.json"
VENDOR = DATA / "vendor"
KNOWN_HOSTS = DATA / "known_hosts"
SSH_PORT = 9922
KEY_PORT = 9991
MAX_LOG = 12 * 1024 * 1024


class CollectorError(Exception):
    pass


def ask(prompt: str, default: str = "") -> str:
    try:
        return input(prompt).strip() or default
    except (KeyboardInterrupt, EOFError) as exc:
        raise CollectorError("Operation cancelled.") from exc


def confirm(prompt: str, default: bool = False) -> bool:
    value = ask(f"{prompt} [{'Y/n' if default else 'y/N'}]: ").lower()
    return default if not value else value in ("y", "yes")


def ipv4(value: str) -> str:
    try:
        address = ipaddress.ip_address(value.strip())
        if not isinstance(address, ipaddress.IPv4Address) or address.is_multicast or address.is_unspecified:
            raise ValueError("not a normal IPv4 address")
        return str(address)
    except ValueError as exc:
        raise CollectorError(f"Invalid IPv4 address: {value!r}") from exc


def ask_ip() -> str:
    while True:
        try:
            return ipv4(ask("TV IPv4 address (e.g. 192.168.1.50): "))
        except CollectorError as exc:
            if str(exc) == "Operation cancelled.":
                raise
            print(f"[Error] {exc}")


def load_profiles() -> tuple[list[dict[str, str]], str]:
    if not PROFILES.is_file():
        return [], ""
    try:
        if PROFILES.stat().st_size > 65536:
            raise ValueError("Profile file is too large")
        saved = json.loads(PROFILES.read_text(encoding="utf-8"))
        if not isinstance(saved, dict) or saved.get("version") != 1:
            raise ValueError("Unsupported profile format")
        entries = []
        for item in saved.get("tvs", []):
            if not isinstance(item, dict):
                continue
            try:
                address = ipv4(str(item.get("ip", "")))
            except CollectorError:
                continue
            name = " ".join(str(item.get("name", "LG webOS TV")).split())[:100]
            entries.append({"ip": address, "name": name or "LG webOS TV"})
        return entries, str(saved.get("last_used", ""))
    except (OSError, ValueError, TypeError, UnicodeError) as exc:
        print(f"[Profiles] Ignoring unreadable saved profiles: {exc}")
        return [], ""


def save_profile(ip: str, name: str) -> None:
    """Persist only after a successful download; never store the SSH passphrase."""
    DATA.mkdir(parents=True, exist_ok=True)
    profiles, _ = load_profiles()
    old = next((entry for entry in profiles if entry["ip"] == ip), None)
    profiles = [entry for entry in profiles if entry["ip"] != ip]
    if old and name == "Manual IP entry":
        name = old["name"]
    profiles.insert(0, {"ip": ip, "name": name or "LG webOS TV"})
    payload = {"version": 1, "last_used": ip, "tvs": profiles[:20]}
    temp = DATA / "tvs.json.tmp"
    try:
        fd = os.open(str(temp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(payload, output, indent=2, ensure_ascii=True)
            output.write("\n")
        temp.replace(PROFILES)
    finally:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass


def description_name(ip: str, location: str) -> str:
    """Fetch SSDP XML only from the address that replied; ignore remote URL host."""
    try:
        parsed = urlsplit(location)
        if parsed.scheme != "http" or not (1 <= (parsed.port or 80) <= 65535):
            return ""
        path = parsed.path or "/"
        if parsed.query:
            path += "?" + parsed.query
        address = f"http://{ip}:{parsed.port or 80}{path}"
        with urlopen(Request(address, headers={"User-Agent": "SysDVR-webOS-log-collector"}), timeout=1.5) as response:
            root = ET.fromstring(response.read(100000))

        def field(tag: str) -> str:
            for element in root.iter():
                if element.tag.rsplit("}", 1)[-1] == tag:
                    return (element.text or "").strip()
            return ""

        maker, model, friendly = field("manufacturer"), field("modelName"), field("friendlyName")
        if "lg" in maker.lower() or "webos" in model.lower() or "[lg]" in friendly.lower():
            return friendly or model or "LG webOS TV"
    except (OSError, ValueError, ET.ParseError, TimeoutError):
        pass
    return ""


def discover(timeout: float = 5.0) -> dict[str, str]:
    print("\n[Discovery] Searching the LAN for LG webOS TVs (SSDP and hostnames)...")
    devices: dict[str, str] = {}
    locations: dict[str, list[str]] = {}
    probe = (b"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
             b'MAN: "ssdp:discover"\r\nMX: 2\r\nST: ssdp:all\r\n\r\n')
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as udp:
            udp.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
            udp.bind(("0.0.0.0", 0))
            udp.settimeout(0.35)
            start, last = time.monotonic(), 0.0
            while time.monotonic() - start < timeout:
                if time.monotonic() - last > 1.5:
                    udp.sendto(probe, ("239.255.255.250", 1900))
                    last = time.monotonic()
                try:
                    packet, peer = udp.recvfrom(8192)
                except socket.timeout:
                    continue
                ip = peer[0]
                headers = {}
                for line in packet.decode("latin1", errors="replace").splitlines()[1:]:
                    if ":" in line:
                        key, value = line.split(":", 1)
                        headers[key.strip().lower()] = value.strip()
                marker = " ".join(headers.get(key, "") for key in
                                  ("server", "usn", "st", "x-user-agent")).lower()
                if any(tag in marker for tag in ("webos", "lge", "lg electronics")):
                    devices[ip] = "LG webOS TV"
                if headers.get("location"):
                    locations.setdefault(ip, []).append(headers["location"])
    except OSError as exc:
        print(f"[Discovery] Multicast not available: {exc}")

    for ip, urls in locations.items():
        for url in urls[:3]:
            name = description_name(ip, url)
            if name:
                devices[ip] = name
                break

    for hostname in ("[LG]webOSTV", "LGwebOSTV", "lgwebostv.local", "webostv.local"):
        try:
            ip = socket.gethostbyname(hostname)
            devices.setdefault(ip, hostname)
        except (OSError, UnicodeError):
            pass
    return dict(sorted(devices.items(), key=lambda pair: tuple(map(int, pair[0].split(".")))))


def auto_or_manual() -> tuple[str, str]:
    results = list(discover().items())
    if not results:
        print("[Discovery] No LG TV found. Enter its IP address manually.")
        return ask_ip(), "Manual IP entry"
    print("\nDiscovered TVs:")
    for n, (ip, name) in enumerate(results, 1):
        print(f"  {n}. {name} ({ip})")
    print("  M. Enter IP manually")
    while True:
        choice = ask("Selection [1]: ", "1").lower()
        if choice == "m":
            return ask_ip(), "Manual IP entry"
        if choice.isdigit() and 1 <= int(choice) <= len(results):
            return results[int(choice) - 1]
        print("Choose a valid option.")


def select_tv() -> tuple[str, str]:
    saved, last = load_profiles()
    if saved:
        saved.sort(key=lambda item: item["ip"] != last)
        print("\nSaved TVs (from successful log downloads):")
        for n, item in enumerate(saved, 1):
            recent = " [last used]" if item["ip"] == last else ""
            print(f"  {n}. {item['name']} ({item['ip']}){recent}")
        print("  S. Search automatically")
        print("  M. Enter a new IP")
        while True:
            choice = ask("Selection [1]: ", "1").lower()
            if choice == "s":
                return auto_or_manual()
            if choice == "m":
                return ask_ip(), "Manual IP entry"
            if choice.isdigit() and 1 <= int(choice) <= len(saved):
                item = saved[int(choice) - 1]
                return item["ip"], item["name"]
            print("Choose a valid option.")

    print("\nHow would you like to find your TV?")
    print("  1. Search automatically")
    print("  2. Enter IP manually")
    while True:
        choice = ask("Option [1]: ", "1")
        if choice == "1":
            return auto_or_manual()
        if choice == "2":
            return ask_ip(), "Manual IP entry"
        print("Choose a valid option.")


def portable_paramiko():
    VENDOR.mkdir(parents=True, exist_ok=True)
    if str(VENDOR) not in sys.path:
        sys.path.insert(0, str(VENDOR))
    try:
        return importlib.import_module("paramiko")
    except ImportError:
        pass

    print("\n[Dependencies] Downloading Paramiko into log_DATA/vendor (not system-wide).")
    command = [sys.executable, "-m", "pip"]
    if subprocess.run(command + ["--version"], capture_output=True).returncode != 0:
        import venv
        bootstrap = DATA / "bootstrap-venv"
        print("[Dependencies] Preparing a local pip environment...")
        try:
            venv.EnvBuilder(with_pip=True).create(str(bootstrap))
        except Exception as exc:
            raise CollectorError(f"Python pip/venv is unavailable: {exc}") from exc
        python = bootstrap / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        command = [str(python), "-m", "pip"]
    command += ["--disable-pip-version-check", "--no-input", "install",
                "--only-binary=:all:", "--target", str(VENDOR), "paramiko>=3.5,<5"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CollectorError(f"Could not install portable SSH dependencies: {exc}") from exc
    if result.returncode:
        raise CollectorError("Portable dependency download failed:\n" +
                             (result.stderr or result.stdout)[-2000:])
    importlib.invalidate_caches()
    try:
        return importlib.import_module("paramiko")
    except ImportError as exc:
        raise CollectorError(f"Paramiko could not be imported: {exc}") from exc


def fetch_encrypted_key(ip: str) -> bytes:
    try:
        with socket.create_connection((ip, KEY_PORT), timeout=7) as sock:
            sock.settimeout(7)
            sock.sendall(b"GET /webos_rsa HTTP/1.0\r\n\r\n")
            received = bytearray()
            while len(received) < 150000:
                chunk = sock.recv(16384)
                if not chunk:
                    break
                received.extend(chunk)
                if b"-----END RSA PRIVATE KEY-----" in received:
                    break
    except OSError as exc:
        raise CollectorError(f"Could not contact Key Server at {ip}:{KEY_PORT}: {exc}. "
                             "Enable Key Server in the TV's Developer Mode app.") from exc
    match = re.search(rb"-----BEGIN RSA PRIVATE KEY-----.*?-----END RSA PRIVATE KEY-----",
                      bytes(received), re.DOTALL)
    if not match:
        raise CollectorError("Key Server did not return an RSA private key.")
    return match.group(0) + b"\n"


def private_key(ip: str, paramiko):
    KEYS.mkdir(parents=True, exist_ok=True)
    path = KEYS / f"webos_rsa_{ip.replace('.', '_')}.pem"
    if not path.exists():
        print("\n[Authentication] Enable Key Server in Developer Mode on the TV.")
        ask("Press Enter when Key Server is enabled...")
        content = fetch_encrypted_key(ip)
        fd = os.open(str(path), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "wb") as handle:
            handle.write(content)
        print(f"[Authentication] Encrypted key saved: {path}")
        print("[Authentication] You may disable Key Server now.")
    content = path.read_bytes()
    if b"PRIVATE KEY" not in content:
        raise CollectorError(f"Invalid private key: {path}")
    password = None
    if b"ENCRYPTED" in content:
        password = getpass.getpass("6-character Developer Mode passphrase: ")
    try:
        return paramiko.RSAKey.from_private_key(io.StringIO(content.decode("ascii")),
                                                password=password)
    except Exception as exc:
        raise CollectorError(f"Failed to open TV SSH key (wrong passphrase?): {exc}") from exc


def connect_ssh(ip: str, key, paramiko):
    DATA.mkdir(parents=True, exist_ok=True)

    class TrustFirstUse(paramiko.MissingHostKeyPolicy):
        def missing_host_key(self, client, hostname, hostkey):
            import hashlib
            digest = hashlib.sha256(hostkey.asbytes()).digest()
            fingerprint = base64.b64encode(digest).decode("ascii").rstrip("=")
            print(f"\n[Security] New SSH host identity: {hostname}")
            print(f"  {hostkey.get_name()} SHA256:{fingerprint}")
            if not confirm("Verified this is your TV; trust its SSH host key?", False):
                raise CollectorError("Host key not approved.")
            client.get_host_keys().add(hostname, hostkey.get_name(), hostkey)
            client.save_host_keys(str(KNOWN_HOSTS))

    def connect(legacy=False):
        client = paramiko.SSHClient()
        if KNOWN_HOSTS.exists():
            client.load_host_keys(str(KNOWN_HOSTS))
        client.set_missing_host_key_policy(TrustFirstUse())
        args = dict(hostname=ip, port=SSH_PORT, username="prisoner", pkey=key,
                    timeout=8, banner_timeout=8, auth_timeout=8,
                    allow_agent=False, look_for_keys=False)
        if legacy:
            args["disabled_algorithms"] = {"pubkeys": ["rsa-sha2-512", "rsa-sha2-256"]}
        try:
            client.connect(**args)
            return client
        except Exception:
            client.close()
            raise

    try:
        return connect()
    except paramiko.BadHostKeyException as exc:
        print("\n[Security] Saved SSH host identity changed. The IP may belong to another device.")
        print(f"  New key type: {exc.key.get_name()}")
        if not confirm("Verified TV identity; replace its saved host key?", False):
            raise CollectorError("Changed host key rejected.") from exc
        saved = paramiko.HostKeys()
        saved.load(str(KNOWN_HOSTS))
        saved.pop(f"[{ip}]:{SSH_PORT}", None)
        saved.save(str(KNOWN_HOSTS))
        return connect()
    except paramiko.AuthenticationException:
        print("[SSH] Trying compatibility with legacy RSA...")
        try:
            return connect(legacy=True)
        except paramiko.AuthenticationException as exc:
            raise CollectorError("TV rejected SSH authentication. Check the Developer Mode passphrase.") from exc
    except (OSError, socket.timeout, paramiko.SSHException) as exc:
        raise CollectorError(f"SSH connection failed for {ip}:{SSH_PORT}: {exc}") from exc


def retrieve_log(ssh, ip: str) -> Path:
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    output = ROOT / f"sysdvr-webos_{stamp}_{ip.replace('.', '-')}.log"
    temp = output.with_name(output.name + ".part")
    command = (f"if [ -r '{LOG_PATH}' ]; then cat '{LOG_PATH}'; "
               "else echo 'LOG_NOT_FOUND' >&2; exit 44; fi")
    print("\n[Download] Retrieving the most recent SysDVR WebOS log...")
    try:
        _, stdout, stderr = ssh.exec_command(command, timeout=15)
        total = 0
        with temp.open("wb") as handle:
            while True:
                chunk = stdout.read(32768)
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_LOG:
                    raise CollectorError("Log exceeds the 12 MiB safety limit.")
                handle.write(chunk)
        code = stdout.channel.recv_exit_status()
        error = stderr.read(4096).decode("utf-8", errors="replace")
        if code != 0:
            if code == 44 or "LOG_NOT_FOUND" in error:
                raise CollectorError("No app log found. Launch SysDVR WebOS, reproduce "
                                     "the issue, then run the collector again.")
            raise CollectorError(f"Remote command failed (exit {code}): {error.strip()}")
        if total == 0:
            raise CollectorError("The TV returned an empty log.")
        temp.replace(output)
        return output
    finally:
        try:
            temp.unlink(missing_ok=True)
        except OSError:
            pass


def main() -> int:
    print("=" * 54)
    print("  SysDVR WebOS - Portable Log Collector")
    print("  Windows / macOS / Linux | Python 3.9+")
    print("=" * 54)
    print("Fetches the latest app-launch log (up to 4 MiB / 60 minutes).")
    if sys.version_info < (3, 9):
        raise CollectorError("Python 3.9 or newer is required.")
    ip, name = select_tv()
    print(f"\n[TV] {name} ({ip})")
    try:
        with socket.create_connection((ip, SSH_PORT), timeout=0.7):
            pass
    except OSError:
        print(f"[Warning] Developer Mode SSH is not reachable at {ip}:{SSH_PORT}.")
        if not confirm("Try connecting anyway?", False):
            return 1

    paramiko = portable_paramiko()
    key = private_key(ip, paramiko)
    ssh = connect_ssh(ip, key, paramiko)
    try:
        output = retrieve_log(ssh, ip)
    finally:
        ssh.close()
    try:
        save_profile(ip, name)
        print("[Profiles] TV saved for future runs.")
    except OSError as exc:
        print(f"[Profiles] Could not save TV information: {exc}")
    print(f"\n[SUCCESS] Log saved: {output}")
    print(f"Size: {output.stat().st_size:,} bytes")
    print("Share only the .log file; never share log_DATA (SSH credentials).")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except CollectorError as exc:
        print(f"\n[Error] {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        sys.exit(130)

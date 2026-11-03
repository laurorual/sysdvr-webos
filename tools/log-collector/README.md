# Portable SysDVR WebOS Log Collector

Retrieve the latest SysDVR WebOS diagnostic log from an LG TV using
**Developer Mode SSH**, with no Node.js, webOS SDK, or `ares-shell` needed.

## Requirements

- Python **3.9+** on Windows, macOS or Linux.
- TV and computer on the same LAN, with Developer Mode enabled.
- Internet on first use only if the SSH dependency (Paramiko) is missing.

Start from the repository root:

```bash
python3 tools/log-collector/get_sysdvr_log.py
```

Or start `run_windows.bat`, `run_linux.sh` or `run_mac.command` in
this directory. On Windows, `py -3 get_sysdvr_log.py` also works.

## First use

1. Choose automatic TV discovery (SSDP + common LG hostnames), or enter IP
   manually. The program falls back to manual IP if discovery finds nothing.
2. If asked, enable **Key Server** in the TV's Developer Mode app.
3. The collector downloads the **encrypted** SSH key from port **9991**.
   Enter the six-character passphrase shown by Developer Mode.
4. Verify the displayed SSH host-key fingerprint before trusting the TV
   (SSH port **9922**). You can then disable Key Server.
5. The latest log is saved next to the Python script, with a timestamp.

## Remembered TVs

After a **successful download**, the TV name and IP are stored in
`log_DATA/tvs.json`. Next time, the last-used TV is offered as choice
**1** (press Enter). Choose **S** to search again or **M** for a manual IP.
Multiple saved TVs are supported.

The encrypted SSH private key is reused automatically, but its **passphrase
is never stored** and may be requested on subsequent runs. If the TV changes
IP, select S to find the new address; Key Server may need to be enabled again.

## Files and security

- `sysdvr-webos_*.log`: downloaded logs, in the script directory
- `log_DATA/tvs.json`: saved TV names and IPs
- `log_DATA/keys`: **sensitive** encrypted SSH private keys
- `log_DATA/known_hosts`: trusted SSH host identities
- `log_DATA/vendor`: locally installed Python dependencies

**Never share `log_DATA`!** Only send the resulting `.log` file to
maintainers. `log_DATA` and downloaded logs are Git-ignored.

The app's log is replaced at each launch and capped at **60 minutes / 4 MiB**;
launch the app and reproduce the issue before fetching the log.

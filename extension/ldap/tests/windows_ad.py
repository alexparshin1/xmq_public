#!/usr/bin/env python3
"""Acceptance against an existing Windows Server domain controller.

samba_ad.py provisions its own domain, which proves the AD schema but not a particular Windows
Server build. This harness runs the same checks against a domain you already have: it starts a
broker with directory_type=ad pointing at that controller over LDAPS and verifies

  - login by sAMAccountName (ldap/alice) and by user principal name (ldap/bob@example.org);
  - a mapped group reached through a nested group, by the in-chain matching rule;
  - a disabled account refused by the directory.

The domain needs the users and groups the checks expect (`--help` lists the options). Everything
the harness creates lives in a temporary directory; the running broker on the machine is untouched.

Usage:
  python3 windows_ad.py build/xmq_server --host dc1.example.org --ca ca.pem \
      --bind-dn 'cn=Administrator,cn=Users,dc=example,dc=org' --bind-password 'Secret#123'
"""
import argparse
import pathlib
import socket
import sys
import tempfile

import directory

GROUPS_SEARCH_BASE = "cn=Users,dc=example,dc=org"

MAPPINGS = (
    "cn=Server Monitoring,cn=Users,dc=example,dc=org => Server Monitoring\n"
    "cn=MQTT Operators,cn=Users,dc=example,dc=org => Operators"
)


def main():
    parser = argparse.ArgumentParser(description="Check XMQ's AD profile against a Windows DC.")
    parser.add_argument("server", help="path to xmq_server")
    parser.add_argument("--host", required=True, help="controller name, as it appears in its certificate")
    parser.add_argument("--port", default="636", help="LDAPS port (default 636)")
    parser.add_argument("--ca", required=True, help="CA certificate that signed the controller's LDAPS certificate")
    parser.add_argument("--bind-dn", required=True, help="DN the extension searches and binds with")
    parser.add_argument("--bind-password", required=True)
    parser.add_argument("--search-base", default=GROUPS_SEARCH_BASE)
    parser.add_argument("--upn", default="ldap/bob@example.org", help="login name checked alongside ldap/alice")
    parser.add_argument("--disabled", default="carol", help="short name of the disabled account")
    parser.add_argument("--host-address", default=None,
                        help="address to put in the temporary /etc/hosts entry (default: resolve --host)")
    parser.add_argument("--no-hosts-entry", action="store_true",
                        help="do not add a temporary /etc/hosts entry for --host")
    arguments = parser.parse_args()

    server = pathlib.Path(arguments.server).resolve()
    library = pathlib.Path(__file__).resolve().parents[3] / "lib/libxmq_ldap.so"
    settings = {
        "host": arguments.host, "port": arguments.port, "encryption": "ldaps",
        "directory_type": "ad", "search_base": arguments.search_base,
        "bind_dn": arguments.bind_dn, "bind_password": arguments.bind_password,
        "ca_file": str(pathlib.Path(arguments.ca).resolve()), "timeout_seconds": "5",
        "group_mappings": MAPPINGS,
    }
    added_host = False
    try:
        if not arguments.no_hosts_entry:
            address = arguments.host_address
            if address is None:
                try:
                    address = socket.gethostbyname(arguments.host)
                except OSError:
                    raise SystemExit(f"cannot resolve {arguments.host}; pass --host-address")
            added_host = directory.add_host_entry(arguments.host, address)
        with tempfile.TemporaryDirectory(prefix="windows-ad-", dir="/tmp/opencode") as workdir:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
            database = pathlib.Path(workdir) / "users.db"
            process, log = directory.start_broker(server, library, port, settings, database,
                                                  f"Windows AD {arguments.host}")
            try:
                directory.verify(port, arguments.upn, arguments.disabled)
                print("Windows AD acceptance passed: sAMAccountName, UPN, nested groups, "
                      "disabled account")
            except Exception:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                process.terminate()
                process.wait(timeout=30)
    finally:
        if added_host:
            directory.remove_host_entry(arguments.host)


if __name__ == "__main__":
    main()

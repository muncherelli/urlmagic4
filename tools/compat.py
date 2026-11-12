#!/usr/bin/env python3
"""httpd.ini conformance: record what ISAPI_Rewrite 2 really does, then hold urlmagic to it.

  python3 tools/compat.py ini              write build/compat/httpd.ini (the cases as rules)
  python3 tools/compat.py capture HOST     send every case's requests to an IIS running the
                                           original filter with that httpd.ini; save the
                                           results to tests/compat/oracle.json
  python3 tools/compat.py show             print the recorded results

HOST is the IIS server's address; requests use the Host header of --site (default
dev.muncherelli.com), whose web root must contain um-echo.cfm (see tests/compat/cases.py).
"""
import argparse
import http.client
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tests", "compat"))
from cases import CASES, httpd_ini  # noqa: E402

ORACLE = os.path.join(os.path.dirname(__file__), "..", "tests", "compat", "oracle.json")


def check_host(host):
    """HOST must be a bare host name or IPv4 address: no scheme, port or path."""
    if not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?", host):
        sys.exit(f"not a host name or address: {host!r}")
    return host


def fetch(host, site, r):
    conn = http.client.HTTPConnection(host, 80, timeout=20)  # NOSONAR: dev tool, the operator's own IIS (check_host)
    headers = {"Host": site, "User-Agent": "urlmagic4-compat"}
    headers.update(r["headers"])
    body = "x=1" if r["method"] == "POST" else None
    if body:
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    conn.request(r["method"], r["url"], body=body, headers=headers)
    resp = conn.getresponse()
    text = resp.read().decode("latin-1", "replace")
    out = {"status": resp.status}
    loc = resp.getheader("Location")
    if loc:
        out["location"] = loc
    first = text.strip().splitlines()[0] if text.strip() else ""
    if first.startswith("ECHO|"):
        _, script, query, method, xum = (first.split("|") + [""] * 5)[:5]
        out["echo"] = {"script": script, "query": query, "method": method, "x_um": xum}
    conn.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["ini", "capture", "show"])
    ap.add_argument("host", nargs="?")
    ap.add_argument("--site", default="dev.muncherelli.com")
    a = ap.parse_args()

    if a.cmd == "ini":
        os.makedirs("build/compat", exist_ok=True)
        with open("build/compat/httpd.ini", "w", newline="") as f:
            f.write(httpd_ini())
        print("wrote build/compat/httpd.ini")
    elif a.cmd == "capture":
        if not a.host:
            sys.exit("capture needs the IIS server's address")
        host = check_host(a.host)
        results = {}
        for cid, title, rules, reqs in CASES:
            results[cid] = {"title": title, "rules": rules, "requests": []}
            for r in reqs:
                got = fetch(host, a.site, r)
                results[cid]["requests"].append({"request": r, "result": got})
                print(f"{cid} {r['method']:4} {r['url']:28} -> {json.dumps(got)}")
        with open(ORACLE, "w") as f:
            json.dump(results, f, indent=1)
        print(f"saved {ORACLE}")
    else:
        with open(ORACLE) as f:
            results = json.load(f)
        for cid, c in results.items():
            print(f"{cid}: {c['title']}")
            for rr in c["requests"]:
                print(f"    {rr['request']['method']:4} {rr['request']['url']:28} -> {json.dumps(rr['result'])}")


if __name__ == "__main__":
    main()

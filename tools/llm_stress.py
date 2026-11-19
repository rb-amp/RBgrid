#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""LLM protocol v1 stress and robustness run against a live hub with an admin token armed.

Usage: RBGRID_ADMIN_TOKEN=... python3 tools/llm_stress.py <hub-ip> [--calls 300] [--reboot]

Pairs its own client (reads the code with the admin token, as the user reads the hub page), then:
 1. N sequential calls over every tool: no 5xx, latency p50/p95, internal heap before/after (leak check);
 2. hostile bodies: bad JSON, oversize, wrong types, deep nesting, odd dates, huge channel lists;
 3. web UI latency (/api/telemetry/latest) alone and while 3 LLM callers hammer the hub;
 4. pairing abuse: bad names/scopes, a third parallel pairing, 5 wrong codes kill a pair;
 5. optional reboot: the token, the on-switch and the manifest hash survive;
and revokes its client at the end. It never trips the 20-wrong-codes pairing block.
"""
import argparse, json, os, statistics, sys, threading, time, urllib.request, urllib.error

FAILS = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)
    return cond


class Hub:
    def __init__(self, host):
        self.base = f"http://{host}"

    def raw(self, path, method="GET", data=None, bearer=None, timeout=20, ctype="application/json"):
        r = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            r.add_header("Content-Type", ctype)
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        t0 = time.time()
        try:
            with urllib.request.urlopen(r, timeout=timeout) as resp:
                body, code = resp.read(), resp.status
        except urllib.error.HTTPError as e:
            body, code = e.read(), e.code
        except Exception as e:  # timeout / reset
            return -1, None, 0, (time.time() - t0) * 1000, repr(e)
        ms = (time.time() - t0) * 1000
        try:
            j = json.loads(body.decode("utf-8"))
        except Exception:
            j = None
        return code, j, len(body), ms, None

    def req(self, path, method="GET", body=None, bearer=None, timeout=20):
        data = json.dumps(body).encode() if body is not None else None
        return self.raw(path, method, data, bearer, timeout)


def pctl(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--calls", type=int, default=300)
    ap.add_argument("--reboot", action="store_true")
    ap.add_argument("--disruptive", action="store_true", help="switch the API off/on and read-only (disturbs other clients)")
    ap.add_argument("--admin-token-file", help="env-style file with RBGRID_*_ADMIN_TOKEN= (read, never printed)")
    a = ap.parse_args()
    if a.admin_token_file:
        for line in open(a.admin_token_file, encoding="utf-8"):
            k, _, v = line.strip().partition("=")
            if k.endswith("_ADMIN_TOKEN") and v:
                os.environ["RBGRID_ADMIN_TOKEN"] = v
    admin = os.environ.get("RBGRID_ADMIN_TOKEN")
    if not admin:
        print("set RBGRID_ADMIN_TOKEN")
        return 2
    h = Hub(a.host)
    code, st, *_ = h.req("/api/v1/llm", bearer=admin)
    if not check(code == 200 and st and st.get("enabled"), "admin view, API on"):
        return 1

    def pair(name, scopes=("read",)):
        c, ps, *_ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": name, "scopes": list(scopes)})
        if c != 200:
            return None, c, ps
        c, st, *_ = h.req("/api/v1/llm", bearer=admin)
        codes = [p["code"] for p in st["pairing"] if p["client_name"] == name]
        c, pj, *_ = h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": ps["pair_id"], "code": codes[0]})
        return (pj or {}).get("token"), c, pj

    tok, c, _ = pair("stress")
    check(bool(tok), "paired client 'stress'")

    def call(tool, args=None, rid="s", timeout=20, retry=True):
        for _ in range(8 if retry else 1):   # another client may hold the two call slots: busy -> retry
            r = h.req("/api/llm/v1/call", "POST", {"tool": tool, "args": args or {}, "request_id": rid}, bearer=tok, timeout=timeout)
            j = r[1]
            if not (j and not j.get("ok") and (j.get("error") or {}).get("code") == "busy"):
                return r
            time.sleep(1.1)
        return r
    call.manifest = lambda: h.req("/api/llm/v1/manifest", bearer=tok)

    code, man, nb, *_ = h.req("/api/llm/v1/manifest", bearer=tok)
    names = [t["name"] for t in man["tools"]]
    print(f"manifest {nb} B, {len(names)} tools: {', '.join(names)}")
    chan = call("list_channels")[1]["result"]["channels"]
    cid = chan[0]["id"] if chan else "0000"

    def heap():
        j = call("get_health")[1]
        return j["result"]["heap"] if j and j.get("ok") else {}

    # ---- 1. sequential calls, latency, leak
    mix = [("get_hub_info", {}), ("list_channels", {}), ("read_live", {}), ("get_health", {}), ("get_events", {"limit": 10}),
           ("get_energy_summary", {"period": {"preset": "today"}}), ("get_energy_summary", {"period": {"preset": "last_7_days"}}),
           ("get_cost_breakdown", {"period": {"preset": "today"}}), ("compare_periods", {"a": {"preset": "today"}, "baseline": "previous_period"}),
           ("get_analysis_playbook", {"id": "installation_check"})]
    if "get_load_profile" in names:
        mix.append(("get_load_profile", {"channel": cid, "period": {"preset": "today"}}))
    if "get_import_export_balance" in names:
        mix.append(("get_import_export_balance", {"period": {"preset": "today"}}))
    for _ in range(10):   # warm-up: lazily allocated buffers, task, queue
        call("get_hub_info")
    h0 = heap()
    lat, bad = [], []
    for i in range(a.calls):
        tool, args = mix[i % len(mix)]
        code, j, nb, ms, err = call(tool, args, rid=f"q{i}")
        lat.append(ms)
        if code != 200 or not j or not j.get("ok") or j.get("request_id") != f"q{i}":
            if not (j and (j.get("error") or {}).get("code") == "busy"):
                bad.append((i, tool, code, err, json.dumps(j)[:160] if j else None))
    h1 = heap()
    check(not bad, f"{a.calls} sequential calls: all ok ({len(bad)} bad) p50 {pctl(lat, .5):.0f} ms p95 {pctl(lat, .95):.0f} ms max {max(lat):.0f} ms")
    for b in bad[:5]:
        print("   ", b)
    if h0 and h1:
        d_int = h0["internal_free_b"] - h1["internal_free_b"]
        d_ps = h0["psram_free_b"] - h1["psram_free_b"]
        print(f"   heap internal {h0['internal_free_b']} -> {h1['internal_free_b']} (min {h1['internal_min_b']}), psram {h0['psram_free_b']} -> {h1['psram_free_b']}")
        check(d_int < 2048 and d_ps < 16384, f"no leak over {a.calls} calls: internal -{d_int} B, psram -{d_ps} B")

    # ---- 2. hostile bodies
    def rawcall(data, ctype="application/json"):
        return h.raw("/api/llm/v1/call", "POST", data, bearer=tok, ctype=ctype)

    cases = [
        ("not JSON", b"{tool:", "invalid_args"),
        ("JSON array", b"[1,2]", "invalid_args"),
        ("tool is a number", b'{"tool":5}', "invalid_args"),
        ("args is an array", b'{"tool":"get_hub_info","args":[]}', "invalid_args"),
        ("args null", b'{"tool":"get_hub_info","args":null}', None),
        ("deep nesting", b'{"tool":"get_hub_info","args":{"x":' + b'[' * 200 + b']' * 200 + b'}}', "invalid_args"),
        ("5 KB body", json.dumps({"tool": "get_hub_info", "args": {}, "pad": "x" * 5000}).encode(), "invalid_args"),
        ("long request_id", json.dumps({"tool": "get_hub_info", "request_id": "r" * 500}).encode(), None),
        ("unicode tool name", json.dumps({"tool": "получить"}).encode(), "unknown_tool"),
        ("invalid UTF-8 tool", b'{"tool":"\xff\xfe"}', None),
        ("feb 30", json.dumps({"tool": "get_energy_summary", "args": {"period": {"from": "2026-02-30", "to": "2026-03-02"}}}).encode(), None),
        ("hour 25", json.dumps({"tool": "get_energy_summary", "args": {"period": {"from": "2026-11-19T25:00", "to": "2026-11-19T26:00"}}}).encode(), "invalid_args"),
        ("offset +14:00", json.dumps({"tool": "get_energy_summary", "args": {"period": {"from": "2026-11-19T00:00+14:00", "to": "2026-11-20T00:00+14:00"}}}).encode(), None),
        ("Z suffix", json.dumps({"tool": "get_energy_summary", "args": {"period": {"from": "2026-11-19T00:00:00Z", "to": "2026-11-19T12:00:00.500Z"}}}).encode(), None),
        ("garbage date", json.dumps({"tool": "get_energy_summary", "args": {"period": {"from": "yesterday", "to": "now"}}}).encode(), "invalid_args"),
        ("future period", json.dumps({"tool": "get_energy_summary", "args": {"period": {"from": "2027-01-01", "to": "2027-02-01"}}}).encode(), "invalid_args"),
        ("bad preset", json.dumps({"tool": "get_cost_breakdown", "args": {"period": {"preset": "tomorrow"}}}).encode(), "invalid_args"),
        ("100 channels", json.dumps({"tool": "read_live", "args": {"channels": [cid] * 100}}).encode(), None),
        ("channels of numbers", json.dumps({"tool": "read_live", "args": {"channels": [1, 2]}}).encode(), "not_found"),
        ("limit -5", json.dumps({"tool": "get_events", "args": {"limit": -5}}).encode(), None),
        ("limit 1e9", json.dumps({"tool": "get_events", "args": {"limit": 1e9}}).encode(), None),
        ("since huge", json.dumps({"tool": "get_events", "args": {"since": 4e9}}).encode(), None),
        ("resolution bogus", json.dumps({"tool": "get_energy_summary", "args": {"period": {"preset": "today"}, "resolution": "minute"}}).encode(), "invalid_args"),
        ("playbook unknown", json.dumps({"tool": "get_analysis_playbook", "args": {"id": "x"}}).encode(), "not_found"),
        ("text/plain", json.dumps({"tool": "get_hub_info"}).encode(), None),
    ]
    for name, data, want in cases:
        code, j, nb, ms, err = rawcall(data, "text/plain" if name == "text/plain" else "application/json")
        okshape = j is not None and isinstance(j, dict) and "ok" in j
        got = None if not okshape else ("ok" if j["ok"] else j["error"]["code"])
        cond = okshape and code in (200, 400) and (want is None or got == want)
        check(cond, f"hostile: {name} -> HTTP {code}, {got}{'' if want is None else ' (want ' + want + ')'}")
        if not cond:
            print("   ", err or (json.dumps(j)[:300] if j else None))
    code, j, *_ = call("get_hub_info")
    check(j and j.get("ok"), "hub still answers after the hostile set")

    # ---- 3. web UI latency, idle vs under LLM load
    def ui_lat(n=15):
        v = []
        for _ in range(n):
            c, _, _, ms, err = h.req("/api/telemetry/latest", timeout=10)
            v.append(ms if c == 200 else 10000)
            time.sleep(0.2)
        return v
    idle = ui_lat()
    stop = threading.Event()
    counts = {"ok": 0, "busy": 0, "other": 0}

    def hammer():
        while not stop.is_set():
            code, j, *_ = call("get_energy_summary", {"period": {"preset": "last_30_days"}, "resolution": "day"}, rid="h", retry=False)
            k = "ok" if j and j.get("ok") else ("busy" if j and j.get("error", {}).get("code") == "busy" else "other")
            counts[k] += 1
    th = [threading.Thread(target=hammer) for _ in range(3)]
    [t.start() for t in th]
    time.sleep(1)
    busy = ui_lat()
    stop.set(); [t.join() for t in th]
    print(f"   web UI /api/telemetry/latest idle p50 {pctl(idle, .5):.0f} ms p95 {pctl(idle, .95):.0f} ms; under LLM load p50 {pctl(busy, .5):.0f} ms p95 {pctl(busy, .95):.0f} ms; LLM {counts}")
    check(pctl(busy, .95) < max(1500, 4 * pctl(idle, .95)) and counts["other"] == 0, "web UI stays responsive under LLM load, LLM only ok/busy")

    # ---- 4. pairing abuse
    for nm, sc, what in [("x" * 33, ["read"], "33-char name"), ("a\x01b", ["read"], "control char"), ("ok", [], "no scope"),
                         ("ok", ["admin"], "admin scope only"), ("", ["read"], "empty name")]:
        c, j, *_ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": nm, "scopes": sc})
        check(c == 400, f"pair/start refuses {what} (HTTP {c})")
    c1, p1, *_ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "abuse1", "scopes": ["read"]})
    c2, p2, *_ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "abuse2", "scopes": ["read"]})
    c3, p3, *_ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "abuse3", "scopes": ["read"]})
    check(c1 == 200 and c2 == 200 and c3 == 429, f"two pairings wait, the third gets 429 ({c1},{c2},{c3})")
    if c1 == 200:
        for k in range(5):
            h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": p1["pair_id"], "code": "%06d" % k if k else "999999"})
        c, st, *_ = h.req("/api/v1/llm", bearer=admin)
        check(not [p for p in st["pairing"] if p["client_name"] == "abuse1"], "5 wrong codes kill the pairing")
    c, j, *_ = h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": "0" * 16, "code": "123456"})
    check(c == 404, "unknown pair_id: 404")
    if not a.disruptive:
        print("   (skipping the off/on and read-only checks: --disruptive not given; pending abuse pairings expire in 10 min)")
    else:
        disruptive(h, admin, call)
    # ---- 5. reboot persistence
    reboot_part(a, h, admin, call, man)
    revoke(h, admin, call)
    print(f"\n{len(FAILS)} FAIL")
    return 1 if FAILS else 0


def disruptive(h, admin, call):
    # clear the remaining pending one: switching off and on drops pairings
    h.req("/api/v1/llm", "POST", {"enabled": False}, bearer=admin)
    c, st, *_ = h.req("/api/v1/llm", bearer=admin)
    check(st["pairing"] == [] and st["enabled"] is False, "switching off drops pending pairings")
    code, j, *_ = call("get_hub_info")
    check(code == 403 and j and j["error"]["code"] == "llm_disabled", "API off: a live token gets llm_disabled")
    h.req("/api/v1/llm", "POST", {"enabled": True}, bearer=admin)
    code, j, *_ = call("get_hub_info")
    check(j and j.get("ok"), "API on again: the same token works")
    h.req("/api/v1/llm", "POST", {"read_only": True}, bearer=admin)
    code, j, *_ = call("get_hub_info")
    check(j and j.get("ok") and j["result"].get("read_only_mode") is True, "read-only mode: read tools still answer, hub reports it")
    h.req("/api/v1/llm", "POST", {"read_only": False}, bearer=admin)


def reboot_part(a, h, admin, call, man):
    tok = None
    if a.reboot:
        mh = man["manifest_hash"]
        c, *_ = h.req("/api/system/reboot", "POST", {}, bearer=admin)
        print(f"   reboot requested: HTTP {c}")
        time.sleep(8)
        t0 = time.time()
        while time.time() - t0 < 90:
            c, j, *_ = h.req("/api/system/version", timeout=3)
            if c == 200:
                break
            time.sleep(3)
        code, man2, *_ = call.manifest()
        check(code == 200 and man2 and man2["manifest_hash"] == mh, "after reboot: same token works, same manifest_hash")
        j = call("get_health")[1]
        check(j and j.get("ok") and j["result"]["uptime_s"] < 120, f"after reboot: uptime {j and j['result']['uptime_s']} s")



def revoke(h, admin, call):
    c, st, *_ = h.req("/api/v1/llm", bearer=admin)
    for t in st.get("tokens", []):
        if t["client_name"] == "stress":
            h.req("/api/v1/llm", "POST", {"revoke": t["id"]}, bearer=admin)
    code, j, *_ = call("get_hub_info")
    check(code == 401, "revoked: 401")


if __name__ == "__main__":
    sys.exit(main())

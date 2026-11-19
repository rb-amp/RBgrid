#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""LLM protocol v1 check against a live hub (documents/api/LLM_PROTOCOL_V1.md).

Usage: python3 tools/llm_test.py <hub-ip> [--admin-token-env VAR] [--keep]

Leaves the hub as found: if it has no admin token, a random one is armed for the run and cleared at the end
(it is never written to disk); the LLM API is switched back off and the test client is revoked.
Prints PASS/FAIL per check, the size and time of every tool answer (the max_response_bytes measurement),
and the "how much did we spend yesterday" figure next to the REST API's own.
"""
import argparse, json, os, secrets, sys, threading, time, urllib.request, urllib.error

FAILS = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)
    return cond


class Hub:
    def __init__(self, host):
        self.base = f"http://{host}"

    def req(self, path, method="GET", body=None, bearer=None, timeout=15):
        data = json.dumps(body).encode() if body is not None else None
        r = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            r.add_header("Content-Type", "application/json")
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        t0 = time.time()
        try:
            with urllib.request.urlopen(r, timeout=timeout) as resp:
                raw = resp.read()
                code = resp.status
        except urllib.error.HTTPError as e:
            raw, code = e.read(), e.code
        ms = (time.time() - t0) * 1000
        try:
            j = json.loads(raw.decode() or "null")
        except Exception:
            j = None
        return code, j, len(raw), ms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--admin-token-env", default="RBGRID_ADMIN_TOKEN")
    ap.add_argument("--keep", action="store_true", help="leave the API on and the client paired")
    ap.add_argument("--admin-token-file", help="env-style file with RBGRID_*_ADMIN_TOKEN= (read, never printed)")
    a = ap.parse_args()
    if a.admin_token_file:
        for line in open(a.admin_token_file, encoding="utf-8"):
            k, _, v = line.strip().partition("=")
            if k.endswith("_ADMIN_TOKEN") and v:
                os.environ["RBGRID_ADMIN_TOKEN"] = v
    h = Hub(a.host)

    # ---- admin token: use the given one, or arm a temporary one
    code, tj, _, _ = h.req("/api/system/token")
    armed_before = bool(tj and tj.get("token_set"))
    admin = os.environ.get(a.admin_token_env)
    temp_admin = None
    if armed_before and not admin:
        print(f"hub has an admin token; set {a.admin_token_env}")
        return 2
    code, st, _, _ = h.req("/api/v1/llm", bearer=admin)
    if not armed_before:
        check(code == 200 and st and st.get("enabled") is False, "onboarding: /api/v1/llm readable, API off")
        code, j, _, _ = h.req("/api/v1/llm", "POST", {"enabled": True})
        check(code == 409, "onboarding: enabling without an admin token is refused (409)")
        code, j, _, _ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "x", "scopes": ["read"]})
        check(code == 403, "onboarding: pairing refused (403)")
        temp_admin = secrets.token_hex(16)
        code, _, _, _ = h.req("/api/system/token", "POST", {"token": temp_admin})
        check(code == 200, "temporary admin token armed")
        admin = temp_admin
    was_on = False
    try:
        code, st, _, _ = h.req("/api/v1/llm", bearer=admin)
        was_on = bool(st and st.get("enabled"))
        # ---- off: every route answers llm_disabled
        if not was_on:
            code, j, _, _ = h.req("/api/llm/v1/manifest", bearer="rbl_" + "0" * 40)
            check(code == 403 and j and j["error"]["code"] == "llm_disabled", "API off: manifest 403 llm_disabled")
        code, j, _, _ = h.req("/api/v1/llm", "POST", {"enabled": True}, bearer=admin)
        check(code == 200 and j and j.get("enabled") is True, "admin: switch the API on")
        code, j, _, _ = h.req("/api/v1/llm", "POST", {"enabled": "yes"}, bearer=admin)
        check(code == 400, "admin: bad field type refused (400)")

        # ---- pairing
        code, ps, _, _ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "llm_test", "scopes": ["read"]})
        check(code == 200 and ps and len(ps.get("pair_id", "")) == 16 and "code" not in ps, "pair/start: pair_id, no code to the client")
        code, st, _, _ = h.req("/api/v1/llm", bearer=admin)
        pend = [p for p in (st or {}).get("pairing", []) if p.get("client_name") == "llm_test"]
        check(len(pend) == 1 and len(pend[0].get("code", "")) == 6, "admin view shows the 6-digit code")
        pcode = pend[0]["code"] if pend else "000000"
        wrong = "%06d" % ((int(pcode) + 1) % 1000000)
        code, j, _, _ = h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": ps["pair_id"], "code": wrong})
        check(code == 401, "pair/complete: wrong code refused (401)")
        code, pj, _, _ = h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": ps["pair_id"], "code": pcode})
        tok = (pj or {}).get("token", "")
        check(code == 200 and tok.startswith("rbl_") and pj.get("scopes") == ["read"], "pair/complete: token, scopes [read]")
        code, j, _, _ = h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": ps["pair_id"], "code": pcode})
        check(code == 404, "pair/complete: a pair_id works once")

        # ---- auth
        code, j, _, _ = h.req("/api/llm/v1/manifest")
        check(code == 401, "manifest without a token: 401")
        code, j, _, _ = h.req("/api/llm/v1/manifest", bearer=admin)
        check(code == 401, "the REST admin token is not an LLM token: 401")

        # ---- manifest
        code, man, nbytes, ms = h.req("/api/llm/v1/manifest", bearer=tok)
        check(code == 200 and man and man.get("proto") == "1.0", f"manifest 200, proto 1.0 ({nbytes} B, {ms:.0f} ms)")
        check(nbytes <= man["limits"]["max_manifest_bytes"], f"manifest within max_manifest_bytes ({nbytes} <= {man['limits']['max_manifest_bytes']})")
        check(man["manifest_hash"].startswith("sha256:") and len(man["manifest_hash"]) == 71, "manifest_hash sha256")
        names = [t["name"] for t in man["tools"]]
        print("tools:", ", ".join(names))
        for t in man["tools"]:
            writes = t["name"] in ("apply_config", "rollback_config")
            ok = isinstance(t.get("input_schema"), dict) and t["input_schema"].get("type") == "object" and t.get("annotations", {}).get("readOnlyHint") is (not writes)
            check(ok, f"tool {t['name']}: object input_schema, readOnlyHint {not writes}")
        code, man2, _, _ = h.req("/api/llm/v1/manifest", bearer=tok)
        check(man2 and man2["manifest_hash"] == man["manifest_hash"], "manifest_hash stable")
        maxb = man["limits"]["max_response_bytes"]

        def call(tool, args=None, rid=None):
            body = {"tool": tool, "args": args or {}, "request_id": rid or f"t-{tool}"}
            return h.req("/api/llm/v1/call", "POST", body, bearer=tok, timeout=20)

        # ---- every tool, sizes
        sizes = []
        calls = [
            ("get_hub_info", {}), ("list_channels", {}), ("read_live", {}), ("get_health", {}), ("get_events", {"limit": 20}),
            ("get_energy_summary", {"period": {"preset": "today"}}),
            ("get_energy_summary", {"period": {"preset": "yesterday"}, "resolution": "hour"}),
            ("get_energy_summary", {"period": {"preset": "last_7_days"}}),
            ("get_energy_summary", {"period": {"preset": "last_30_days"}, "resolution": "day"}),
            ("get_energy_summary", {"period": {"preset": "last_30_days"}, "resolution": "hour"}),
            ("get_energy_summary", {"period": {"preset": "this_year"}, "resolution": "month"}),
            ("get_cost_breakdown", {"period": {"preset": "yesterday"}}),
            ("get_cost_breakdown", {"period": {"preset": "this_month"}}),
            ("compare_periods", {"a": {"preset": "yesterday"}, "baseline": "previous_period"}),
            ("get_analysis_playbook", {"id": "monthly_report"}),
        ]
        names_now = [t["name"] for t in man["tools"]]
        if "get_load_profile" in names_now:
            chans0 = call("list_channels")[1]["result"]["channels"]
            if chans0:
                calls.append(("get_load_profile", {"channel": chans0[0]["id"], "period": {"preset": "today"}}))
                calls.append(("get_load_profile", {"channel": chans0[0]["id"], "period": {"preset": "last_30_days"}, "resolution": "hour"}))
        for extra, ar in (("check_installation", {}), ("detect_anomalies", {}), ("get_regional_context", {}),
                          ("get_regional_context", {"country": "DE"}), ("get_analysis_playbook", {"id": "installation_check"})):
            if extra in names_now:
                calls.append((extra, ar))
        if "get_import_export_balance" in names_now:
            calls.append(("get_import_export_balance", {"period": {"preset": "today"}}))
        res = {}
        for tool, args in calls:
            code, j, nb, ms = call(tool, args)
            ok = code == 200 and j and j.get("ok") is True and j.get("request_id") == f"t-{tool}" and "meta" in j
            m = (j or {}).get("meta", {})
            check(ok and nb <= maxb, f"{tool} {json.dumps(args)}: ok, {nb} B, {ms:.0f} ms, res={m.get('resolution')} "
                                      f"trunc={m.get('truncated')} gaps={m.get('gaps')} as_of={m.get('as_of')}")
            if not ok:
                print("   ", json.dumps(j)[:400])
            sizes.append((nb, ms, tool, json.dumps(args)))
            res.setdefault(tool, j)
            if m.get("notes"):
                print("    notes:", m["notes"])
        print("largest answers:")
        for nb, ms, t, ar in sorted(sizes, reverse=True)[:5]:
            print(f"   {nb:6d} B {ms:6.0f} ms  {t} {ar}")

        # ---- errors
        code, j, _, _ = call("no_such_tool")
        check(j and j.get("ok") is False and j["error"]["code"] == "unknown_tool", "unknown_tool")
        code, j, _, _ = call("get_energy_summary", {"period": {"preset": "today"}, "bogus": 1})
        check(j and j["error"]["code"] == "invalid_args", "invalid_args: unknown argument")
        code, j, _, _ = call("get_energy_summary", {})
        check(j and j["error"]["code"] == "invalid_args", "invalid_args: missing period")
        code, j, _, _ = call("get_energy_summary", {"period": {"from": "2026-11-10", "to": "2026-11-01"}})
        check(j and j["error"]["code"] == "invalid_args", "invalid_args: from after to")
        code, j, _, _ = call("get_energy_summary", {"period": {"from": "2020-01-01", "to": "2020-02-01"}})
        check(j and j["error"]["code"] == "out_of_history", "out_of_history")
        code, j, _, _ = call("read_live", {"channels": ["FFFF"]})
        check(j and j["error"]["code"] == "not_found", "not_found: unknown channel")

        # ---- config scope: a read token is refused, a read+config token reads every section
        if "get_config" in [t["name"] for t in man["tools"]]:
            code, j, _, _ = call("get_config")
            check(j and j.get("ok") is False and j["error"]["code"] == "forbidden_scope", "get_config with a read token: forbidden_scope")
            code, ps2, _, _ = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "llm_test_cfg", "scopes": ["read", "config"]})
            code, st2, _, _ = h.req("/api/v1/llm", bearer=admin)
            c2 = [p["code"] for p in st2["pairing"] if p["client_name"] == "llm_test_cfg"]
            code, pj2, _, _ = h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": ps2["pair_id"], "code": c2[0]})
            tok2 = pj2.get("token", "")
            check(pj2.get("scopes") == ["read", "config"], "pair with read+config")
            for sec in (None, "channels", "tariff", "notifications", "region"):
                body = {"tool": "get_config", "args": {"section": sec} if sec else {}, "request_id": "cfg"}
                code, j, nb, ms = h.req("/api/llm/v1/call", "POST", body, bearer=tok2)
                ok = j and j.get("ok") and len(j["result"].get("version", "")) == 8
                check(ok and nb <= maxb, f"get_config {sec or 'all'}: ok, version, {nb} B, {ms:.0f} ms")
                if not ok:
                    print("   ", json.dumps(j)[:300])
                txt = json.dumps(j)
                check("token\":\"" not in txt and "password" not in txt.lower(), f"get_config {sec or 'all'}: no token or password in the answer")
            code, st2, _, _ = h.req("/api/v1/llm", bearer=admin)
            for t in st2.get("tokens", []):
                if t["client_name"] == "llm_test_cfg":
                    h.req("/api/v1/llm", "POST", {"revoke": t["id"]}, bearer=admin)

        # ---- yesterday's cost vs the REST API
        cb = res.get("get_cost_breakdown") or {}
        chans = (res.get("list_channels") or {}).get("result", {}).get("channels", [])
        mains = [c["id"] for c in chans if c["role"] == "main"]
        if cb.get("ok") and mains:
            p = cb["result"]["period"]
            print(f"yesterday per LLM: total_cost={cb['result'].get('total_cost')} {cb['result'].get('currency')} "
                  f"import={cb['result'].get('import_kwh')} kWh  period {p['from']} .. {p['to']}")
            code, hist, _, _ = h.req(f"/api/v1/history?unit={mains[0]}&period=day&limit=3", bearer=admin)
            print("   REST day records:", json.dumps(hist)[:600])

        # ---- concurrency: 4 parallel calls → all ok, or ok + busy
        out = []
        def worker(i):
            out.append(call("get_energy_summary", {"period": {"preset": "last_7_days"}}, rid=f"c{i}"))
        th = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
        [t.start() for t in th]; [t.join() for t in th]
        kinds = [("ok" if j and j.get("ok") else (j or {}).get("error", {}).get("code", str(code))) for code, j, _, _ in out]
        check(all(k in ("ok", "busy") for k in kinds) and "ok" in kinds, f"concurrency: {kinds}")

        # ---- call log + revoke
        code, st, _, _ = h.req("/api/v1/llm", bearer=admin)
        tk = [t for t in (st or {}).get("tokens", []) if t["client_name"] == "llm_test"]
        check(len(tk) == 1 and st.get("log_total", 0) >= len(calls), f"admin view: token listed, log {st.get('log_total')} entries")
        if not a.keep and tk:
            code, j, _, _ = h.req("/api/v1/llm", "POST", {"revoke": tk[0]["id"]}, bearer=admin)
            check(code == 200, "revoke the test client")
            code, j, _, _ = h.req("/api/llm/v1/manifest", bearer=tok)
            check(code == 401, "revoked token: 401")
    finally:
        if not a.keep and not was_on:
            h.req("/api/v1/llm", "POST", {"enabled": False}, bearer=admin)
        if temp_admin:
            code, _, _, _ = h.req("/api/system/token", "POST", {"token": ""}, bearer=temp_admin)
            print("temporary admin token cleared:", code)
    print(f"\n{len(FAILS)} FAIL")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025-2026 Zhuhai RobotDyn Technology Co., Ltd.
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""LLM protocol v1 stage 2 check: propose / apply / versions / rollback against a live hub with an admin token.

Usage: python3 tools/llm_config_test.py <hub-ip> --admin-token-file <env file>
Restores what it changed (channel name, peaks cycle day, read-only switch) and revokes its client.
"""
import argparse, json, os, sys, time, urllib.request, urllib.error

FAILS = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)
    return cond


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--admin-token-file", required=True)
    a = ap.parse_args()
    admin = [l.split("=", 1)[1].strip() for l in open(a.admin_token_file, encoding="utf-8") if "_ADMIN_TOKEN=" in l][0]
    base = f"http://{a.host}"

    def req(path, method="GET", body=None, bearer=None):
        r = urllib.request.Request(base + path, data=json.dumps(body).encode() if body is not None else None, method=method)
        if body is not None:
            r.add_header("Content-Type", "application/json")
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        try:
            with urllib.request.urlopen(r, timeout=20) as x:
                return x.status, json.loads(x.read() or b"null")
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read() or b"null")

    c, ps = req("/api/llm/v1/pair/start", "POST", {"client_name": "cfg_test", "scopes": ["read", "config"]})
    c, st = req("/api/v1/llm", bearer=admin)
    code = [p["code"] for p in st["pairing"] if p["client_name"] == "cfg_test"][0]
    c, pj = req("/api/llm/v1/pair/complete", "POST", {"pair_id": ps["pair_id"], "code": code})
    tok = pj["token"]

    def call(tool, args=None):
        c, j = req("/api/llm/v1/call", "POST", {"tool": tool, "args": args or {}, "request_id": tool}, bearer=tok)
        return j

    def err(j):
        return (j or {}).get("error", {}).get("code") if j and not j.get("ok") else None

    man = req("/api/llm/v1/manifest", bearer=tok)[1]
    names = [t["name"] for t in man["tools"]]
    check(all(n in names for n in ("propose_config", "apply_config", "list_config_versions", "rollback_config")), "manifest has the stage 2 tools")
    ann = {t["name"]: t["annotations"] for t in man["tools"]}
    check(ann["apply_config"]["readOnlyHint"] is False and ann["propose_config"]["readOnlyHint"] is True, "annotations: apply writes, propose doesn't")
    check("config" in man["capabilities"], "capabilities has config")

    cfg = call("get_config", {"section": "channels"})["result"]
    ch = cfg["channels"][0]
    cid, orig_name = ch["id"], ch["name"]
    print(f"   channel {cid} '{orig_name}', config version {cfg['version']}")
    v0 = call("list_config_versions")
    check(v0.get("ok") and len(v0["result"]["versions"]) >= 1, f"list_config_versions: {len(v0['result']['versions'])} version(s), newest source {v0['result']['versions'][0]['source']}")
    first_ver = v0["result"]["versions"][0]["version"]

    # ---- refusals
    check(err(call("propose_config", {})) == "invalid_args", "propose without patch or rollback_to: invalid_args")
    check(err(call("propose_config", {"patch": {"channels": {cid: {"name": orig_name}}}})) == "invalid_args", "patch that changes nothing: invalid_args")
    check(err(call("propose_config", {"patch": {"channels": {cid: {"ct_code": 3}}}})) == "invalid_args", "non-writable field: invalid_args")
    check(err(call("propose_config", {"patch": {"channels": {cid: {"name": "x" * 40}}}})) == "invalid_args", "name over 31 bytes: invalid_args")
    check(err(call("propose_config", {"patch": {"channels": {cid: {"name": "a\u0001b"}}}})) == "invalid_args", "name with a control char: invalid_args")
    check(err(call("propose_config", {"patch": {"channels": {"FFFF": {"name": "x"}}}})) == "not_found", "unknown channel: not_found")
    if ch["role_detail"] in ("main_supply", "alternative_supply"):
        check(err(call("propose_config", {"patch": {"channels": {cid: {"role": "device_load"}}}})) == "invalid_args", "supply role change refused (topology)")
    check(err(call("propose_config", {"patch": {"peaks_cycle_day": 40}})) == "invalid_args", "peaks_cycle_day 40: invalid_args")
    check(err(call("apply_config", {"confirm_token": "0" * 32})) == "confirm_invalid", "apply with an unknown token: confirm_invalid")
    check(err(call("rollback_config", {"version": first_ver})) == "confirm_required", "rollback without a token: confirm_required")

    # ---- propose → nothing changes yet → apply
    new_name = "LLM test name"
    p = call("propose_config", {"patch": {"channels": {cid: {"name": new_name}}}})
    ok = p.get("ok") and len(p["result"]["confirm_token"]) == 32 and p["result"]["diff"] == [{"path": f"channels.{cid}.name", "from": orig_name, "to": new_name}]
    check(ok, f"propose rename: diff {p.get('result', {}).get('diff')}, warnings {p.get('result', {}).get('warnings')}")
    cur = call("get_config", {"section": "channels"})["result"]["channels"][0]["name"]
    check(cur == orig_name, "a proposal changes nothing")
    ap1 = call("apply_config", {"confirm_token": p["result"]["confirm_token"]})
    check(ap1.get("ok") and ap1["result"]["applied"], f"apply: version {ap1.get('result', {}).get('version')}")
    cur = call("get_config", {"section": "channels"})["result"]["channels"][0]["name"]
    check(cur == new_name, f"the name is now '{cur}'")
    c, units = req("/api/v1/sensors")
    check(err(call("apply_config", {"confirm_token": p["result"]["confirm_token"]})) == "confirm_invalid", "a token works once")
    vl = call("list_config_versions")["result"]["versions"]
    check(vl[0]["source"] == "llm:cfg_test" and vl[0]["current"], f"newest version from llm:cfg_test: {vl[0]}")
    c, st = req("/api/v1/llm", bearer=admin)
    logc = [l for l in st["log"] if l["tool"] == "apply_config" and l["result"] == "ok"]
    check(bool(logc) and logc[0]["config_changed"] is True, "call log marks apply as config_changed")

    # ---- conflict: the user changes the name on the hub page between propose and apply
    p2 = call("propose_config", {"patch": {"channels": {cid: {"name": "conflict test"}}}})
    c, r = req(f"/api/v1/units/{cid}", "POST", {"name": "changed on page"}, bearer=admin)
    check(c == 200, f"REST rename between propose and apply (HTTP {c})")
    check(err(call("apply_config", {"confirm_token": p2["result"]["confirm_token"]})) == "conflict", "apply after an outside change: conflict")
    vl = call("list_config_versions")["result"]["versions"]
    check(vl[0]["source"] == "hub", f"the outside change is recorded as a version from 'hub': {vl[0]['summary']}")

    # ---- read-only mode
    req("/api/v1/llm", "POST", {"read_only": True}, bearer=admin)
    check(err(call("propose_config", {"patch": {"peaks_cycle_day": 5}})) == "read_only_mode", "read-only: propose refused")
    check(call("get_config").get("ok"), "read-only: get_config still answers")
    req("/api/v1/llm", "POST", {"read_only": False}, bearer=admin)

    # ---- peaks cycle day round trip
    pk0 = call("get_config", {"section": "notifications"})["result"]["notifications"]["peaks_cycle_day"]
    nd = 15 if pk0 != 15 else 16
    p3 = call("propose_config", {"patch": {"peaks_cycle_day": nd}})
    a3 = call("apply_config", {"confirm_token": p3["result"]["confirm_token"]}) if p3.get("ok") else p3
    pk1 = call("get_config", {"section": "notifications"})["result"]["notifications"]["peaks_cycle_day"]
    check(a3.get("ok") and pk1 == nd, f"peaks_cycle_day {pk0} -> {pk1}")

    # ---- rollback to the first recorded version: name and cycle day come back
    rb = call("propose_config", {"rollback_to": first_ver})
    check(rb.get("ok") and len(rb["result"]["diff"]) >= 1, f"propose rollback_to {first_ver}: diff {rb.get('result', {}).get('diff')}")
    if rb.get("ok"):
        r2 = call("rollback_config", {"version": first_ver, "confirm_token": rb["result"]["confirm_token"]})
        check(r2.get("ok"), f"rollback applied: version {r2.get('result', {}).get('version')}")
    cfg2 = call("get_config")["result"]
    check(cfg2["channels"][0]["name"] == orig_name and cfg2["notifications"]["peaks_cycle_day"] == pk0,
          f"after rollback: name '{cfg2['channels'][0]['name']}', cycle day {cfg2['notifications']['peaks_cycle_day']}")
    h = call("get_health")["result"]["heap"]
    print(f"   llm worker stack free (high-water): {h.get('llm_stack_free_b')} B; internal free {h['internal_free_b']} min {h['internal_min_b']}")
    check(h.get("llm_stack_free_b", 0) > 2048, "worker stack keeps more than 2 KB after apply/rollback")

    # cleanup
    c, st = req("/api/v1/llm", bearer=admin)
    for t in st["tokens"]:
        if t["client_name"] == "cfg_test":
            req("/api/v1/llm", "POST", {"revoke": t["id"]}, bearer=admin)
    print(f"\n{len(FAILS)} FAIL")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

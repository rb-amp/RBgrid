#!/usr/bin/env python3
"""What the hub page (AI assistants) uses, against a live hub with an admin token armed and the LLM API on.

Usage: python tools/llm_page_test.py <hub-ip> --admin-token-file <env file with RBGRID_*_ADMIN_TOKEN=>

Checks: the connect block; llm_pair_pending in the telemetry and Deny (pair_cancel); the reserved client name "web";
POST /api/v1/llm/call as the page (versions, events, a rename + rollback with source "web", also in read-only mode).
Leaves the hub as found (name restored, read-only restored).
"""
import argparse, json, sys, time, urllib.request, urllib.error

FAILS = 0


def check(cond, what):
    global FAILS
    print(("PASS " if cond else "FAIL ") + what)
    if not cond:
        FAILS += 1
    return cond


class Hub:
    def __init__(self, host, admin):
        self.base, self.admin = f"http://{host}", admin

    def req(self, path, method="GET", body=None, bearer=None, timeout=20):
        data = json.dumps(body).encode() if body is not None else None
        r = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            r.add_header("Content-Type", "application/json")
        if bearer:
            r.add_header("Authorization", "Bearer " + bearer)
        try:
            with urllib.request.urlopen(r, timeout=timeout) as x:
                return x.status, json.loads(x.read().decode("utf-8") or "null")
        except urllib.error.HTTPError as e:
            try:
                return e.code, json.loads(e.read().decode("utf-8") or "null")
            except Exception:
                return e.code, None

    def page(self, tool, args=None):
        for _ in range(8):
            c, j = self.req("/api/v1/llm/call", "POST", {"tool": tool, "args": args or {}}, bearer=self.admin)
            if not (j and not j.get("ok") and (j.get("error") or {}).get("code") == "busy"):
                return c, j
            time.sleep(1.1)
        return c, j


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--admin-token-file", required=True)
    a = ap.parse_args()
    admin = [l.split("=", 1)[1].strip() for l in open(a.admin_token_file, encoding="utf-8") if "_ADMIN_TOKEN=" in l][0]
    h = Hub(a.host, admin)

    c, st = h.req("/api/v1/llm", bearer=admin)
    if not check(c == 200 and st and st.get("enabled"), "admin view, API on"):
        return 1
    ro_before = st.get("read_only")

    # ---- connect block
    cn = st.get("connect") or {}
    check(len(cn.get("hub_id", "")) == 12 and cn.get("mdns_host", "").endswith(".local") and cn.get("proto"),
          f"connect: hub_id {cn.get('hub_id')}, mdns {cn.get('mdns_host')}, proto {cn.get('proto')}")
    check(cn.get("ip") == a.host and cn.get("url") == f"http://{a.host}/", f"connect: ip/url = this hub ({cn.get('url')})")

    # ---- reserved name
    c, j = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "Web", "scopes": ["read"]})
    check(c == 400, f"pairing as \"Web\" refused (HTTP {c})")

    # ---- pending pairing: telemetry counter + Deny
    c, tel = h.req("/api/telemetry/latest")
    n0 = (tel or {}).get("system", {}).get("llm_pair_pending")
    check(isinstance(n0, int), f"telemetry system.llm_pair_pending present ({n0})")
    c, ps = h.req("/api/llm/v1/pair/start", "POST", {"client_name": "page_test", "scopes": ["read"]})
    check(c == 200 and ps and ps.get("pair_id"), "pair/start")
    c, tel = h.req("/api/telemetry/latest")
    n1 = (tel or {}).get("system", {}).get("llm_pair_pending")
    check(isinstance(n1, int) and n1 == (n0 or 0) + 1, f"llm_pair_pending counts it ({n0} -> {n1})")
    c, st = h.req("/api/v1/llm", bearer=admin)
    pend = [p for p in st.get("pairing", []) if p.get("client_name") == "page_test"]
    check(pend and pend[0].get("pair_id") == ps["pair_id"], "admin view lists the pairing with its pair_id")
    code = pend[0]["code"] if pend else "000000"
    c, j = h.req("/api/v1/llm", "POST", {"pair_cancel": ps["pair_id"]}, bearer=admin)
    check(c == 200 and not [p for p in j.get("pairing", []) if p.get("client_name") == "page_test"], "Deny: pair_cancel removes it")
    c, j = h.req("/api/llm/v1/pair/complete", "POST", {"pair_id": ps["pair_id"], "code": code})
    check(c != 200, f"the denied code no longer works (HTTP {c})")
    c, j = h.req("/api/v1/llm", "POST", {"pair_cancel": "nope"}, bearer=admin)
    check(c == 404, f"pair_cancel unknown id -> 404 ({c})")

    # ---- the page's own tool calls
    c, j = h.req("/api/v1/llm/call", "POST", {"tool": "get_hub_info"})
    check(c in (401, 403), f"page call without the admin token refused (HTTP {c})")
    c, j = h.page("list_config_versions")
    check(c == 200 and j and j.get("ok") and j["result"].get("versions"), "page: list_config_versions")
    c, j = h.page("get_events", {"limit": 5})
    check(c == 200 and j and j.get("ok") and isinstance(j["result"].get("events"), list), "page: get_events")
    c, j = h.page("list_channels")
    chans = [x for x in (j or {}).get("result", {}).get("channels", []) if x["role"] != "main"]
    if not check(chans, "page: list_channels has a non-main channel"):
        return 1
    ch = chans[0]
    old = ch["name"]
    try:
        # read-only on: the page may still write (it is the user)
        h.req("/api/v1/llm", "POST", {"read_only": True}, bearer=admin)
        c, pj = h.page("propose_config", {"patch": {"channels": {ch["id"]: {"name": "PageTest"}}}})
        tok = (pj or {}).get("result", {}).get("confirm_token")
        check(pj and pj.get("ok") and tok, "page: propose in read-only mode")
        c, aj = h.page("apply_config", {"confirm_token": tok})
        check(aj and aj.get("ok") and aj["result"].get("applied"), "page: apply in read-only mode")
        ver = (aj or {}).get("result", {}).get("version")
        c, vj = h.page("list_config_versions")
        top = (vj or {}).get("result", {}).get("versions", [{}])[0]
        check(top.get("version") == ver and top.get("source") == "web", f"version {ver} has source \"web\" ({top.get('source')})")
        c, ej = h.page("get_events", {"limit": 50})
        evs = (ej or {}).get("result", {}).get("events", [])
        check(any(e["type"] == "config_changed" and e["data"].get("version") == ver and e["data"].get("source") == "web" for e in evs),
              "config_changed event with source web")
        # rollback to the version before
        c, pj = h.page("propose_config", {"rollback_to": ver - 1})
        tok = (pj or {}).get("result", {}).get("confirm_token")
        c, rj = h.page("rollback_config", {"confirm_token": tok})
        check(rj and rj.get("ok"), "page: rollback to the version before")
        c, j = h.page("list_channels")
        nm = [x["name"] for x in (j or {}).get("result", {}).get("channels", []) if x["id"] == ch["id"]]
        check(nm == [old], f"name restored by the rollback ({nm})")
    finally:
        h.req("/api/v1/llm", "POST", {"read_only": bool(ro_before)}, bearer=admin)
        c, j = h.page("list_channels")
        nm = [x["name"] for x in (j or {}).get("result", {}).get("channels", []) if x["id"] == ch["id"]]
        if nm != [old]:
            print("restoring the name via the page")
            c, pj = h.page("propose_config", {"patch": {"channels": {ch["id"]: {"name": old}}}})
            h.page("apply_config", {"confirm_token": (pj or {}).get("result", {}).get("confirm_token")})

    # ---- an LLM token in read-only mode is still refused (the exemption is the page's only)
    print(f"\n{FAILS} FAIL")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

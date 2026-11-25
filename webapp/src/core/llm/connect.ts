/**
 * Connecting an assistant to the hub: the hub address a client should use. The bridge itself (package, snippets) is
 * in ./bridge, kept out of the build until it is published.
 */
import { LlmConnect } from './api';

/** The hub address a client should use: the mDNS name first; the IP as the fallback. */
export function hubUrls(c: LlmConnect | null, fallbackOrigin: string): { primary: string; fallback: string | null } {
  const ipUrl = c?.ip ? `http://${c.ip}/` : (c?.url || null);
  if (c?.mdns_host) return { primary: `http://${c.mdns_host}/`, fallback: ipUrl };
  if (ipUrl) return { primary: ipUrl, fallback: null };
  return { primary: `${fallbackOrigin.replace(/\/+$/, '')}/`, fallback: null };
}


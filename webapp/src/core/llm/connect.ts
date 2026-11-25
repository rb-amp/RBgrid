/**
 * Config snippets for connecting an assistant to the hub through the local bridge (CONNECT-UX §2.2,
 * root ai-connect-ux). Bridge facts from the bridge's own docs: npm package and bin `rbgrid-mcp`, env
 * `RBGRID_HUB_URL` (+ optional `RBGRID_READ_ONLY=1`).
 */
import { LlmConnect } from './api';

export const BRIDGE_PACKAGE = 'rbgrid-mcp';
/** The Claude Desktop extension (.mcpb) on the bridge's release page. null until it is published:
 *  the page then says it comes with the release instead of linking somewhere that doesn't exist. */
export const MCPB_URL: string | null = null;

export type ClientTab = 'claude-desktop' | 'claude-code' | 'cursor' | 'vscode' | 'lmstudio' | 'other';
export const CLIENT_TABS: ClientTab[] = ['claude-desktop', 'claude-code', 'cursor', 'vscode', 'lmstudio', 'other'];

/** The hub address a client should use: the mDNS name first; the IP as the fallback. */
export function hubUrls(c: LlmConnect | null, fallbackOrigin: string): { primary: string; fallback: string | null } {
  const ipUrl = c?.ip ? `http://${c.ip}/` : (c?.url || null);
  if (c?.mdns_host) return { primary: `http://${c.mdns_host}/`, fallback: ipUrl };
  if (ipUrl) return { primary: ipUrl, fallback: null };
  return { primary: `${fallbackOrigin.replace(/\/+$/, '')}/`, fallback: null };
}

const env = (hub: string, readOnly: boolean): Record<string, string> => ({ RBGRID_HUB_URL: hub, ...(readOnly ? { RBGRID_READ_ONLY: '1' } : {}) });

/** The text a user copies for the client (null for Claude Desktop: it installs the .mcpb). */
export function snippet(tab: ClientTab, hub: string, readOnly: boolean): string | null {
  const e = env(hub, readOnly);
  switch (tab) {
    case 'claude-desktop':
      return null;
    case 'claude-code':
      return `claude mcp add rbgrid ${Object.entries(e).map(([k, v]) => `-e ${k}=${v}`).join(' ')} -- npx -y ${BRIDGE_PACKAGE}`;
    case 'vscode':
      // VS Code's mcp.json keys servers under "servers" with a "type" (not "mcpServers").
      return JSON.stringify({ servers: { rbgrid: { type: 'stdio', command: 'npx', args: ['-y', BRIDGE_PACKAGE], env: e } } }, null, 2);
    default:
      return JSON.stringify({ mcpServers: { rbgrid: { command: 'npx', args: ['-y', BRIDGE_PACKAGE], env: e } } }, null, 2);
  }
}

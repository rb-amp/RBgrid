/**
 * The local bridge (`rbgrid-mcp`): its package, the .mcpb link and the config snippets per client (CONNECT-UX §2.2,
 * root ai-connect-ux; env `RBGRID_HUB_URL` + optional `RBGRID_READ_ONLY=1`).
 *
 * NOT IMPORTED by the app while the bridge is unpublished (25.11: npm answers 404 for the package; the release check
 * wants no `rbgrid-mcp` anywhere in the kit). The day it is published: import BridgeConnect in ConnectCard instead of
 * the «not published yet» note, and set MCPB_URL.
 */
export const BRIDGE_PACKAGE = 'rbgrid-mcp';
/** The Claude Desktop extension (.mcpb) on the bridge's release page; null until it is published. */
export const MCPB_URL: string | null = null;

export type ClientTab = 'claude-desktop' | 'claude-code' | 'cursor' | 'vscode' | 'lmstudio' | 'other';
export const CLIENT_TABS: ClientTab[] = ['claude-desktop', 'claude-code', 'cursor', 'vscode', 'lmstudio', 'other'];

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

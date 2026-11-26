/**
 * The Claude Desktop extension of the bridge (.mcpb) — published 26.11
 * (downloaded and run by the bridge's maintainers, sha256 below).
 * The VERSIONED address on purpose: `releases/latest/…` follows the newest release of the whole repository, and the
 * next firmware release (no .mcpb among its assets) would turn it into a 404 with nobody editing anything.
 * The npm package is NOT published (no npm account in custody): no `npx` line until it is — see ./bridge.
 */
export const MCPB_VERSION = '0.2.0';
export const MCPB_URL = `https://github.com/rb-amp/RBgrid/releases/download/mcp-v${MCPB_VERSION}/rbgrid-mcp.mcpb`;
export const MCPB_SHA256 = 'c979c255de52baf8e6a15d020de730f16051e92900cb2ea539c3a63e351054e4';

import { hubUrls, snippet } from './connect';

const C = { hub_id: 'd8132a03bfac', name: 'Home', mdns_host: 'rbgrid-bfac.local', ip: '192.168.1.50', url: 'http://192.168.1.50/', proto: '1.0' };

describe('connect snippets', () => {
  it('mDNS name first, the IP as the fallback', () => {
    expect(hubUrls(C, 'http://x')).toEqual({ primary: 'http://rbgrid-bfac.local/', fallback: 'http://192.168.1.50/' });
    expect(hubUrls(null, 'http://192.168.1.9')).toEqual({ primary: 'http://192.168.1.9/', fallback: null });
  });
  it('Claude Code command, with read-only on request', () => {
    expect(snippet('claude-code', 'http://rbgrid-bfac.local/', false)).toBe('claude mcp add rbgrid -e RBGRID_HUB_URL=http://rbgrid-bfac.local/ -- npx -y rbgrid-mcp');
    expect(snippet('claude-code', 'http://h/', true)).toContain('-e RBGRID_READ_ONLY=1 --');
  });
  it('mcpServers JSON for Cursor / LM Studio / other; VS Code uses servers + type', () => {
    expect(JSON.parse(snippet('cursor', 'http://h/', false) as string)).toEqual({ mcpServers: { rbgrid: { command: 'npx', args: ['-y', 'rbgrid-mcp'], env: { RBGRID_HUB_URL: 'http://h/' } } } });
    expect(JSON.parse(snippet('vscode', 'http://h/', false) as string).servers.rbgrid.type).toBe('stdio');
    expect(snippet('claude-desktop', 'http://h/', false)).toBeNull();
  });
});

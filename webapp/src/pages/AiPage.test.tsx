import React from 'react';
import { render, screen, fireEvent, waitFor, act, within } from '@testing-library/react';
import { MemoryRouter } from 'react-router-dom';
import { ThemeProvider } from '../theme/ThemeProvider';
import { DeviceProvider, DeviceTransport, mockSchema, mockConfig } from '../core';
import { llmApi, LlmDoc, LlmError } from '../core/llm/api';
import { AiPage, callsToday } from './AiPage';

const transport = new Proxy({ kind: 'direct', getSchema: async () => mockSchema(), getConfig: async (d: string) => mockConfig(d as never) } as Record<string, unknown>, {
  get: (o, k) => (k in o ? o[k as string] : async () => null),
}) as unknown as DeviceTransport;

const DOC: LlmDoc = {
  enabled: true, read_only: false, proto: '1.0',
  pairing: [{ pair_id: 'p1', client_name: '<img src=x onerror=alert(1)>', scopes: ['read', 'config'], code: '482913', expires_in: 600 }],
  tokens: [{ id: 'aa11bb22', client_name: 'Bridge', scopes: ['read'], created: 1795098991, last_used: null }],
  log: [{ ts: 1795102781, client: 'Bridge', tool: 'propose_config', result: 'forbidden_scope', ms: 4, bytes: 90, config_changed: false },
    { ts: 1795102700, client: 'Bridge', tool: 'apply_config', result: 'ok', ms: 80, bytes: 90, config_changed: true }],
  log_total: 2,
  connect: { hub_id: 'd8132a03bfac', name: 'Home', mdns_host: 'rbgrid-bfac.local', ip: '192.168.1.50', url: 'http://192.168.1.50/', proto: '1.0' },
};

const renderPage = () => render(
  <ThemeProvider forcedTheme="dark"><DeviceProvider transport={transport}><MemoryRouter><AiPage /></MemoryRouter></DeviceProvider></ThemeProvider>,
);

beforeEach(() => {
  jest.spyOn(llmApi, 'versions').mockResolvedValue([
    { version: 5, time: '2026-11-20T10:00:00+01:00', source: 'llm:Claude', summary: '1 change(s)', current: true },
    { version: 4, time: null, source: 'hub', summary: 'first recorded state', current: false },
  ]);
  jest.spyOn(llmApi, 'recentEvents').mockResolvedValue([
    { seq: 9, time: '2026-11-20T10:00:00+01:00', type: 'config_changed', severity: 'info', data: { version: 5, source: 'web' }, message: '' },
    { seq: 8, time: null, type: 'module_lost', severity: 'warning', data: { name: 'Boiler' }, message: '' },
  ]);
});
afterEach(() => jest.restoreAllMocks());

it('shows the pairing code big, the client name as text, the paired assistant and the log', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue(DOC);
  const { container } = renderPage();
  expect(await screen.findByText('482913')).toBeInTheDocument();
  expect(screen.getByText('<img src=x onerror=alert(1)>')).toBeInTheDocument();
  expect(container.querySelector('img')).toBeNull();
  expect(screen.getAllByText('Bridge', { selector: 'td' })).toHaveLength(3);
  expect(screen.getByText('forbidden_scope')).toBeInTheDocument();
  expect(screen.getByText('не разрешено этому ассистенту', { exact: false })).toBeInTheDocument();
  expect(screen.getByText('настройки изменены')).toBeInTheDocument();
  expect(screen.getByText(/Действует ещё (10:00|9:5\d)/)).toBeInTheDocument();
});

it('says «set an admin token first» when switching on answers 409', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue({ ...DOC, enabled: false, pairing: [] });
  jest.spyOn(llmApi, 'set').mockRejectedValue(new LlmError('conflict', 409));
  renderPage();
  const sw = await screen.findByLabelText('Разрешить ИИ-ассистентов');
  await act(async () => { fireEvent.click(sw); });
  expect(await screen.findByText(/Сначала задайте токен администратора/)).toBeInTheDocument();
  expect(screen.getByRole('link', { name: 'Безопасность' })).toHaveAttribute('href', '/config#security');
});

it('revokes by token id', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue(DOC);
  const set = jest.spyOn(llmApi, 'set').mockResolvedValue({ ...DOC, tokens: [] });
  renderPage();
  fireEvent.click(await screen.findByRole('button', { name: 'Отключить' }));
  await waitFor(() => expect(set).toHaveBeenCalledWith({ revoke: 'aa11bb22' }));
  expect(await screen.findByText('Подключённых ассистентов нет.')).toBeInTheDocument();
});

it('on a hub without the API says so', async () => {
  jest.spyOn(llmApi, 'get').mockRejectedValue(new LlmError('HTTP 404', 404));
  renderPage();
  expect(await screen.findByText(/не предоставляет API для ИИ-ассистентов/)).toBeInTheDocument();
});

it('a call stamped before the hub clock synced says so instead of «1 Jan»', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue({ ...DOC, pairing: [], log: [{ ...DOC.log[0], ts: 30 }] });
  renderPage();
  expect(await screen.findByText('до синхронизации часов')).toBeInTheDocument();
});

it('Deny cancels the pairing by its pair_id', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue(DOC);
  const set = jest.spyOn(llmApi, 'set').mockResolvedValue({ ...DOC, pairing: [] });
  renderPage();
  fireEvent.click(await screen.findByRole('button', { name: 'Отклонить' }));
  await waitFor(() => expect(set).toHaveBeenCalledWith({ pair_cancel: 'p1' }));
});

it('the connect block fills the hub address from connect (mDNS first)', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue(DOC);
  renderPage();
  expect(await screen.findByText('http://rbgrid-bfac.local/')).toBeInTheDocument();
  // The .mcpb is published (26.11), the npm package is not: the versioned file link, the restart step, no npx line.
  expect(screen.queryByRole('tab', { name: 'Claude Code' })).toBeNull();
  expect(screen.queryByText(/npx|claude mcp add/)).toBeNull();
  const dl = screen.getByRole('link', { name: /Скачать расширение RBgrid/ });
  expect(dl.getAttribute('href')).toBe('https://github.com/rb-amp/RBgrid/releases/download/mcp-v0.2.0/rbgrid-mcp.mcpb');
  expect(dl.getAttribute('href')).not.toMatch(/latest/);
  expect(screen.getByText(/полностью закройте Claude Desktop/)).toBeInTheDocument();
});

it('Restore shows the diff, then rolls back with the token', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue(DOC);
  const propose = jest.spyOn(llmApi, 'proposeRollback').mockResolvedValue({ diff: [{ path: 'channels.0006.name', from: 'PageTest', to: 'Boiler' }], warnings: [], confirm_token: 'tok', expires_in: 600 });
  const roll = jest.spyOn(llmApi, 'rollback').mockResolvedValue(undefined);
  renderPage();
  fireEvent.click(await screen.findByRole('button', { name: 'Восстановить' }));
  await waitFor(() => expect(propose).toHaveBeenCalledWith(4));
  expect(await screen.findByText('channels.0006.name')).toBeInTheDocument();
  const dlg = screen.getByRole('dialog');
  fireEvent.click(within(dlg).getByRole('button', { name: 'Восстановить' }));
  await waitFor(() => expect(roll).toHaveBeenCalledWith(4, 'tok'));
  expect(await screen.findByText('Версия 4 восстановлена.')).toBeInTheDocument();
});

it('events read as text per type', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue(DOC);
  renderPage();
  expect(await screen.findByText('Сохранена версия настроек 5: это приложение')).toBeInTheDocument();
  expect(screen.getByText('Нет данных от «Boiler»')).toBeInTheDocument();
});

it('calls today: by client name, the log cutting names at 23 bytes', () => {
  const d = { ...DOC, log: [{ ...DOC.log[0], client: 'Claude (user simulation', ts: 200 }, { ...DOC.log[0], client: 'Claude (user simulation', ts: 50 }], log_total: 2 };
  expect(callsToday(d, 'Claude (user simulation)', 100)).toBe('1');
  expect(callsToday({ ...d, log_total: 90 }, 'Claude (user simulation)', 10)).toBe('2+');
});

it('the page\'s own calls (client «web») read as «this app» in the log', async () => {
  jest.spyOn(llmApi, 'get').mockResolvedValue({ ...DOC, pairing: [], log: [{ ...DOC.log[1], client: 'web', tool: 'rollback_config' }] });
  renderPage();
  expect(await screen.findByText('это приложение', { selector: 'td' })).toBeInTheDocument();
});

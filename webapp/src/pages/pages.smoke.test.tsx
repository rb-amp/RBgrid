/**
 * Smoke tests — render the device-management-stack pages within the providers and a
 * mock transport, asserting they mount without crashing. Catches the class of runtime
 * errors (bad hook order, undefined access, missing provider) that tsc + the unit tests
 * don't. Not behavioural — just "does it render".
 */
import React from 'react';
import { render, screen, fireEvent, waitFor, act } from '@testing-library/react';
import { MemoryRouter, Routes, Route, Navigate } from 'react-router-dom';
import { ThemeProvider } from '../theme/ThemeProvider';
import {
  DeviceProvider, DeviceTransport,
  mockSchema, mockProvisioning, mockConfig, mockEconomics, mockSummary, mockEspnow,
} from '../core';
import { connectDemo, disconnect } from '../services/connection';
import { ConfigurationPage } from './ConfigurationPage';
import { EconomicsPage } from './EconomicsPage';
import { DevicesPage } from './DevicesPage';
import { DashboardPage } from './DashboardPage';
import { AppShell } from '../app/AppShell';
import { TariffsPricesPage } from './TariffsPricesPage';
import { KioskPage } from './KioskPage';
import { AppliancePage } from './AppliancePage';
import { setDemoVariant } from '../mock/demoVariant';
import { PointDetailPage } from './PointDetailPage';
import { OnboardingPage } from './OnboardingPage';
import { HistoryPage } from './HistoryPage';
import { EventsPage } from './EventsPage';
import { TariffsPage } from './TariffsPage';
import { ConnectScreen } from './ConnectScreen';

// Demo mode so the data hooks (useSensors/useHistory) return synthetic data and the
// pages reach their loaded state instead of hanging on a (serverless) network call.
beforeAll(() => connectDemo());
afterAll(() => disconnect());

/** A mock transport returning the canned shapes (no network); optional per-domain overrides. */
function fakeTransport(over: Partial<Record<string, Record<string, unknown>>> = {}): DeviceTransport {
  return {
    kind: 'direct',
    getSchema: async () => mockSchema(),
    getConfig: async (d) => ({ ...mockConfig(d), ...(over[d] ?? {}) } as ReturnType<typeof mockConfig>),
    setConfig: async () => undefined,
    getProvisioning: async () => mockProvisioning(),
    getEconomics: async (u) => mockEconomics(u),
    getSummary: async (u, p) => mockSummary(u, p),
    setTelemetryProfile: async () => undefined,
    getTokenStatus: async () => false,
    getTokenRoles: async () => ({ adminSet: false, readSet: false }),
    setToken: async () => undefined,
    getSecurity: async () => null,
    getAp: async () => null,
    setApPassword: async () => ({}),
    setSecurity: async () => undefined,
    getBackupStatus: async () => null,
    createBackup: async () => ({ filesBackedUp: 0 }),
    restoreBackup: async () => ({ status: 'OK', filesRestored: 0, requiresReboot: true }),
    postOta: async () => ({ status: 'ok', message: '', partition: 'ota_0' }),
    getEspnow: async () => mockEspnow(),
    espnowAction: async () => ({ ok: true }),
    exportCsv: async () => null,
  };
}

const renderPage = (ui: React.ReactElement, transport: DeviceTransport = fakeTransport()) =>
  render(
    <ThemeProvider forcedTheme="dark">
      <DeviceProvider transport={transport}>
        <MemoryRouter>{ui}</MemoryRouter>
      </DeviceProvider>
    </ThemeProvider>,
  );

/** ConnectScreen is the pre-app gate — no DeviceProvider/router. */
const renderBare = (ui: React.ReactElement) =>
  render(<ThemeProvider forcedTheme="dark">{ui}</ThemeProvider>);

describe('page smoke tests', () => {

  it('ConfigurationPage renders the settings hub', async () => {
    renderPage(<ConfigurationPage />);
    expect(await screen.findByText('Настройки')).toBeInTheDocument();
  });

  it('EconomicsPage renders', async () => {
    renderPage(<EconomicsPage />);
    expect(await screen.findByText('Экономика')).toBeInTheDocument();
  });

  it('DevicesPage: points first, the hardware on its own tab; the wiring folded under «Подключение»', async () => {
    window.sessionStorage.clear();
    renderPage(<DevicesPage />);
    const conn = await screen.findAllByRole('button', { name: 'Подключение' }, { timeout: 4000 });
    expect(conn.length).toBeGreaterThan(0);
    expect(screen.queryByText(/Модули rbAmp/)).toBeNull();
    fireEvent.click(screen.getByRole('tab', { name: /Оборудование/ }));
    expect(await screen.findByText(/Модули rbAmp/)).toBeInTheDocument();
  }, 20_000);

  it('DevicesPage renders', async () => {
    renderPage(<DevicesPage />);
    expect(await screen.findByText('Точки')).toBeInTheDocument();
  });

  it('HistoryPage: points are picked in a searchable list with ticks, up to 6', async () => {
    renderPage(<HistoryPage />);
    fireEvent.click(await screen.findByRole('button', { name: /^Точки: \d/ }, { timeout: 4000 }));
    expect(await screen.findByText(/до 6 точек/)).toBeInTheDocument();
    const boxes = screen.getAllByRole('checkbox');
    expect(boxes.length).toBeGreaterThan(1);
    const before = boxes.filter((b) => (b as HTMLInputElement).checked).length;
    fireEvent.click(boxes.find((b) => !(b as HTMLInputElement).checked)!);
    await waitFor(() => expect(screen.getAllByRole('checkbox').filter((b) => (b as HTMLInputElement).checked).length).toBeGreaterThan(before));
  }, 20_000);

  it('HistoryPage renders the heading', async () => {
    renderPage(<HistoryPage />);
    expect(await screen.findByText('История')).toBeInTheDocument();
  });

  it('EventsPage renders the heading', async () => {
    renderPage(<EventsPage />);
    expect(await screen.findByRole('heading', { name: 'События' })).toBeInTheDocument();
  });

  it('PointDetailPage mounts for a known unit', () => {
    const { container } = render(
      <ThemeProvider forcedTheme="dark">
        <DeviceProvider transport={fakeTransport()}>
          <MemoryRouter initialEntries={['/point/main']}>
            <Routes>
              <Route path="/point/:unitKey" element={<PointDetailPage />} />
            </Routes>
          </MemoryRouter>
        </DeviceProvider>
      </ThemeProvider>,
    );
    expect(container.firstChild).toBeTruthy();
  });

  // Pages behind a loading gate: assert they mount + produce DOM without crashing.
  it('DashboardPage mounts without crashing', () => {
    const { container } = renderPage(<DashboardPage />);
    expect(container.firstChild).toBeTruthy();
  });

  // The Overview's default layout follows what is wired to the hub (UI redesign 22.11), not a
  // scenario: the demo sites differ in their connections.
  it.each([
    ['heatpump-ev', 'Сегодня по часам'],
    ['business', 'Линии нагрузки'],
    ['ev', 'Баланс нагрузки паркинга'],
  ] as const)('DashboardPage builds the %s site\'s default layout from its connections', async (site, marker) => {
    setDemoVariant(site);
    try {
      renderPage(<DashboardPage />);
      expect(await screen.findByText(marker, {}, { timeout: 4000 })).toBeInTheDocument();
    } finally { setDemoVariant('heatpump-ev'); }
  });

  it('DashboardPage «Настроить»: remove a block, Done — the view stays after a remount', async () => {
    window.localStorage.clear();
    const first = renderPage(<DashboardPage />);
    await screen.findByText('Сегодня по часам', {}, { timeout: 4000 });
    fireEvent.click(screen.getByRole('button', { name: 'Вид' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Настроить' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Убрать «Сегодня по часам»' }));
    fireEvent.click(screen.getByRole('button', { name: 'Готово' }));
    await waitFor(() => expect(screen.queryByRole('button', { name: 'Готово' })).toBeNull());
    expect(screen.queryByText('Сегодня по часам')).toBeNull();
    first.unmount();
    renderPage(<DashboardPage />);
    await screen.findByRole('heading', { name: 'Обзор' });
    await screen.findByText('Сегодня', {}, { timeout: 4000 });
    expect(screen.queryByText('Сегодня по часам')).toBeNull();
    window.localStorage.clear();
  }, 30_000);

  it('DashboardPage: one tile per appliance (the heat pump and the charger, not their phases)', async () => {
    window.localStorage.clear();
    renderPage(<DashboardPage />);
    expect(await screen.findByText('Heat pump', {}, { timeout: 4000 })).toBeInTheDocument();
    expect(screen.getByText('EV charger')).toBeInTheDocument();
    expect(screen.queryByText('Heat pump L1')).toBeNull();
    expect(screen.getAllByText('фаз: 3').length).toBe(2);
  });

  it('DashboardPage: tiles keep their order; in «Настроить» a tile moves with the arrows and hides', async () => {
    window.localStorage.clear();
    renderPage(<DashboardPage />);
    await screen.findAllByText('Kitchen', {}, { timeout: 4000 });
    fireEvent.click(screen.getByRole('button', { name: 'Вид' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Настроить' }));
    expect(await screen.findByRole('button', { name: '«Kitchen» раньше' })).toBeDisabled(); // first in the hub's order
    fireEvent.click(screen.getByRole('button', { name: '«Kitchen» позже' }));
    const hideBtns = screen.getAllByRole('button', { name: 'Показана' });
    fireEvent.click(hideBtns[0]);
    fireEvent.click(screen.getByRole('button', { name: 'Готово' }));
    await waitFor(() => expect(screen.queryByRole('button', { name: 'Готово' })).toBeNull());
    const saved = JSON.parse(Object.keys(window.localStorage).filter((k) => k.startsWith('rbgrid.ui_state.')).map((k) => window.localStorage.getItem(k) as string)[0]);
    expect(saved.doc.order.length).toBeGreaterThan(1);
    expect(saved.doc.order[1]).toBe('9020');
    expect(saved.doc.hide.length).toBe(1);
    window.localStorage.clear();
  }, 60_000);

  it('DashboardPage switches to the «Подробно» board', async () => {
    renderPage(<DashboardPage />);
    await screen.findByRole('heading', { name: 'Обзор' });
    fireEvent.click(screen.getByRole('tab', { name: 'Подробно' }));
    await screen.findByText('Поток энергии');
  }, 20_000);

  it('ConfigurationPage: sections in a side list, one section at a time; an old #anchor opens its section', async () => {
    render(
      <ThemeProvider forcedTheme="dark">
        <DeviceProvider transport={fakeTransport()}>
          <MemoryRouter initialEntries={['/config/device']}>
            <Routes>
              <Route path="/config" element={<ConfigurationPage />} />
              <Route path="/config/:section" element={<ConfigurationPage />} />
            </Routes>
          </MemoryRouter>
        </DeviceProvider>
      </ThemeProvider>,
    );
    expect(await screen.findByRole('navigation', { name: 'Разделы настроек' })).toBeInTheDocument();
    expect(await screen.findByText('Мастер настройки')).toBeInTheDocument();
    expect(screen.queryByText('Группы потребителей')).toBeNull(); // the Site section isn't drawn
    fireEvent.click(screen.getByRole('button', { name: 'Объект' }));
    expect(await screen.findByText('Группы потребителей')).toBeInTheDocument();
  }, 30_000);

  it('ConfigurationPage: every section opens without a crash once its data has loaded (Security once crashed, React #310)', async () => {
    for (const sec of ['site', 'ui', 'network', 'integrations', 'security', 'device']) {
      const tr = { ...fakeTransport(), getSecurity: async () => ({ dev_cors: false, origins: [], origins_max: 8 }) } as unknown as DeviceTransport;
      const { unmount } = render(
        <ThemeProvider forcedTheme="dark">
          <DeviceProvider transport={tr}>
            <MemoryRouter initialEntries={[`/config/${sec}`]}>
              <Routes><Route path="/config/:section" element={<ConfigurationPage />} /></Routes>
            </MemoryRouter>
          </DeviceProvider>
        </ThemeProvider>,
      );
      expect(await screen.findByRole('navigation', { name: 'Разделы настроек' })).toBeInTheDocument();
      await act(async () => { await new Promise((r) => setTimeout(r, 300)); }); // the panels' data lands
      expect(screen.getByRole('navigation', { name: 'Разделы настроек' })).toBeInTheDocument();
      unmount();
    }
  }, 60_000);

  it('ConfigurationPage offers no site scenario (the Overview follows what is wired, UI redesign 22.11)', async () => {
    renderPage(<ConfigurationPage />);
    await screen.findByText('Настройки');
    expect(screen.queryByText('Сценарий объекта')).toBeNull();
  });

  it('TariffsPage mounts without crashing', () => {
    const { container } = renderPage(<TariffsPage />);
    expect(container.firstChild).toBeTruthy();
  });

  it('OnboardingPage renders the wizard steps', async () => {
    renderPage(<OnboardingPage />);
    expect(await screen.findByText('Датчики')).toBeInTheDocument();
    expect(screen.getByText('Безопасность')).toBeInTheDocument();
  });

  it('AppShell: the menu by questions (Смотреть / Следить / Настроить), Tariffs & prices in one item, /prices leads there', async () => {
    render(
      <ThemeProvider forcedTheme="dark">
        <DeviceProvider transport={fakeTransport()}>
          <MemoryRouter initialEntries={['/prices']}>
            <Routes>
              <Route path="/" element={<AppShell />}>
                <Route path="tariffs" element={<TariffsPricesPage />} />
                <Route path="tariffs/:tab" element={<TariffsPricesPage />} />
                <Route path="prices" element={<Navigate to="/tariffs/dynamic" replace />} />
              </Route>
            </Routes>
          </MemoryRouter>
        </DeviceProvider>
      </ThemeProvider>,
    );
    expect(await screen.findByText('Смотреть')).toBeInTheDocument();
    expect(screen.getByText('Следить')).toBeInTheDocument();
    expect(screen.getByText('Настроить')).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Цены' })).toBeNull();
    expect((await screen.findAllByText('Тарифы и цены')).length).toBeGreaterThan(0);
    // Quick search (N9): Ctrl+K, a setting by a word the user would type.
    fireEvent.keyDown(window, { key: 'k', ctrlKey: true });
    // Russian words find it, in any order, and the hit says where it is (role audit 22.11).
    fireEvent.change(await screen.findByLabelText('Страница, точка или настройка'), { target: { value: 'пояс часовой' } });
    expect(await screen.findByText('Настройки · Сеть и время')).toBeInTheDocument();
  }, 20_000);

  it('KioskPage: a board on the whole screen, no menu, «Закрыть» back', async () => {
    render(
      <ThemeProvider forcedTheme="dark">
        <DeviceProvider transport={fakeTransport()}>
          <MemoryRouter initialEntries={['/board/main']}>
            <Routes><Route path="/board/:id" element={<KioskPage />} /></Routes>
          </MemoryRouter>
        </DeviceProvider>
      </ThemeProvider>,
    );
    expect(await screen.findByRole('button', { name: 'Закрыть' }, { timeout: 6000 })).toBeInTheDocument();
    expect(screen.queryByText('Смотреть')).toBeNull(); // no rail
    expect(await screen.findByText('Сегодня по часам', {}, { timeout: 6000 })).toBeInTheDocument();
  }, 30_000);

  it('AppliancePage: a heat pump as one — the sum of its phases, its phases with links (role audit H-02)', async () => {
    render(
      <ThemeProvider forcedTheme="dark">
        <DeviceProvider transport={fakeTransport()}>
          <MemoryRouter initialEntries={['/appliance/2']}>
            <Routes><Route path="/appliance/:groupId" element={<AppliancePage />} /></Routes>
          </MemoryRouter>
        </DeviceProvider>
      </ThemeProvider>,
    );
    expect(await screen.findByRole('heading', { name: 'Heat pump' }, { timeout: 6000 })).toBeInTheDocument();
    expect(screen.getByText('Фазы прибора')).toBeInTheDocument();
    expect(screen.getAllByText(/^L[123]$/).length).toBe(3);
  }, 30_000);

  it('ConnectScreen renders the brand', async () => {
    renderBare(<ConnectScreen />);
    expect(await screen.findByText('RBgrid')).toBeInTheDocument();
  });
});

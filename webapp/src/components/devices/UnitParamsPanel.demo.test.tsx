/**
 * UnitParamsPanel against the REAL DeviceProvider + DirectTransport (demo): edits one point and
 * saves; the units-config write must carry the full array with only that unit changed.
 */
import React from 'react';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { ThemeProvider } from '../../theme/ThemeProvider';
import { DeviceProvider } from '../../core';
import { connectDemo, disconnect } from '../../services/connection';
import { UnitParamsPanel, nestOptionsOf } from './UnitParamsPanel';

// The solar home (its water heater 9010, a 13.8 kW input); the default demo site has neither.
beforeAll(() => { localStorage.setItem('rbgrid.demo.variant', 'home'); connectDemo(); });
afterAll(() => { disconnect(); localStorage.removeItem('rbgrid.demo.config'); localStorage.removeItem('rbgrid.demo.variant'); });

const renderPanel = (apiId: string) => render(
  <ThemeProvider forcedTheme="dark">
    <DeviceProvider><UnitParamsPanel apiId={apiId} /></DeviceProvider>
  </ThemeProvider>,
);

it('saves a line nominal and short name; other units untouched', async () => {
  renderPanel('9010');
  const rated = await screen.findByLabelText('Номинал линии, кВт');
  expect((rated as HTMLInputElement).value).toBe('2.3');
  fireEvent.change(rated, { target: { value: '3,5' } });
  fireEvent.change(screen.getByLabelText('Короткое имя (для схем)'), { target: { value: 'Бойлер-2' } });
  fireEvent.click(screen.getByRole('button', { name: 'Сохранить' }));
  // demo: nothing reaches a hub, so the success reads as a demo save (UX audit W0-9).
  await screen.findByText('Сохранено (демо — только в этом браузере, прибор не затронут).');

  const stored = JSON.parse(localStorage.getItem('rbgrid.demo.config') ?? '{}');
  const units = stored.units.units as { unit_id: number; rated_power_w: number; short_name: string }[];
  expect(units).toHaveLength(6);
  const boiler = units.find((u) => u.unit_id === 0x9010)!;
  expect(boiler.rated_power_w).toBe(3500);
  expect(boiler.short_name).toBe('Бойлер-2');
  expect(units.find((u) => u.unit_id === 0x9001)!.rated_power_w).toBe(13800);
});

it('blocks over-long short names and locks the input role', async () => {
  renderPanel('9001');
  const short = await screen.findByLabelText('Короткое имя (для схем)');
  fireEvent.change(short, { target: { value: 'Бойлер-гаражный' } }); // 29 bytes > schema.limits 24
  await waitFor(() => expect(screen.getByRole('button', { name: 'Сохранить' })).toBeDisabled());
  expect(screen.getByText('роль ввода задаёт хаб, она не меняется')).toBeInTheDocument();
  expect(screen.getByLabelText('Лимит ввода, кВт')).toBeInTheDocument();
});

it('offers no «Measured inside» on a hub without point nesting (fw before 1.1.8)', async () => {
  renderPanel('9010');
  await screen.findByLabelText('Номинал линии, кВт');
  expect(screen.queryByLabelText('Внутри точки')).toBeNull();
});

describe('nestOptionsOf (fw 1.1.8 `inside`)', () => {
  const list = [
    { unit_id: 0x0006, role: 'MAIN_SUPPLY' }, { unit_id: 0x0007, type: 'VOLTAGE_BUS' },
    { unit_id: 0x6B08, role: 'DEVICE_LOAD' }, { unit_id: 0x6B09, role: 'EV_CHARGING', inside: 0x6B08 },
    { unit_id: 0x9020, role: 'DEVICE_LOAD', inside: 0x6B09 }, { unit_id: 0x9021, role: 'DEVICE_LOAD' },
  ];
  const ids = (self: string, cur = '') => nestOptionsOf(list, self, cur).map((u) => (u.unit_id as number).toString(16).toUpperCase());
  it('other lines only: not the input, the bus or itself', () => {
    expect(ids('9021')).toEqual(['6B08', '6B09', '9020']);
  });
  it('not a point already inside this one, at any depth (a loop)', () => {
    expect(ids('6B08')).toEqual(['9021']);
    expect(ids('6B09', '6B08')).toEqual(['6B08', '9021']);
  });
});

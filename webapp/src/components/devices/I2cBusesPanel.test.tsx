import React from 'react';
import { render, screen, waitFor, fireEvent, within, act } from '@testing-library/react';
import { I2cBusesPanel, bus1Pins } from './I2cBusesPanel';
import { ConfirmProvider } from '../common/ConfirmDialog';

const mockApi = { getI2c: jest.fn(), setI2cBusPins: jest.fn(), rebootDevice: jest.fn() };
jest.mock('../../services/api', () => ({ getApiClient: () => mockApi, ApiRequestError: class extends Error {} }));

const state = (bus1: object, extra: object = {}) => ({
  buses: [{ bus: 0, up: true, configured: true, sda: 8, scl: 9 }, { bus: 1, ...bus1 }],
  modules: [{ module: 0, bus: 0, addr: 0x53, present: true }],
  max_modules: 8,
  pins_ok: [4, 5, 8, 9, 17, 18],
  pins_reserved: [{ gpio: 8, use: 'I2C bus 0' }, { gpio: 9, use: 'I2C bus 0' }, { gpio: 0, use: 'boot strap' }],
  ...extra,
});

describe('I2cBusesPanel (hub 1.3.0)', () => {
  beforeEach(() => Object.values(mockApi).forEach((f) => f.mockReset()));

  it('bus 1 may take the allowed pins bus 0 does not hold', () => {
    expect(bus1Pins(state({ up: false, configured: false }) as never)).toEqual([4, 5, 17, 18]);
  });

  it('sets bus-1 pins and says they apply after a restart, with the restart offered', async () => {
    mockApi.getI2c.mockResolvedValue(state({ up: false, configured: false }));
    mockApi.setI2cBusPins.mockResolvedValue({ status: 'ok', ...state({ up: false, configured: true, sda: 17, scl: 18 }, { reboot_pending: true }) });
    render(<ConfirmProvider><I2cBusesPanel /></ConfirmProvider>);
    fireEvent.click(await screen.findByRole('button', { name: /(Set up|Настроить)/ }));
    fireEvent.mouseDown(screen.getByLabelText('SDA'));
    fireEvent.click(within(screen.getByRole('listbox')).getByText('GPIO17'));
    fireEvent.mouseDown(screen.getByLabelText('SCL'));
    fireEvent.click(within(screen.getByRole('listbox')).getByText('GPIO18'));
    fireEvent.click(screen.getByRole('button', { name: /^(Save|Сохранить)$/ }));
    await waitFor(() => expect(mockApi.setI2cBusPins).toHaveBeenCalledWith(1, { sda: 17, scl: 18 }));
    expect(await screen.findByText(/(after the hub restarts|после перезапуска хаба)/)).toBeTruthy();
    expect(screen.getByRole('button', { name: /reconcile\.rebootNow|Restart|Перезагрузить/i })).toBeTruthy();
  });

  it('after «Restart now» re-reads until the hub no longer reports a pending restart (S3 23.11)', async () => {
    jest.useFakeTimers();
    try {
      const pending = state({ up: true, configured: true, sda: 17, scl: 18 }, { reboot_pending: true });
      mockApi.getI2c.mockResolvedValueOnce(pending).mockRejectedValueOnce(new Error('restarting'))
        .mockResolvedValue(state({ up: true, configured: true, sda: 17, scl: 18 }, { reboot_pending: false }));
      mockApi.rebootDevice.mockResolvedValue({});
      render(<ConfirmProvider><I2cBusesPanel /></ConfirmProvider>);
      fireEvent.click(await screen.findByRole('button', { name: /reconcile\.rebootNow|Restart now|Перезагрузить сейчас/i }));
      fireEvent.click(await screen.findByRole('button', { name: /^(admin\.rebootAction|Reboot|Перезагрузить)$/i }));
      await waitFor(() => expect(mockApi.rebootDevice).toHaveBeenCalled());
      await act(async () => { jest.advanceTimersByTime(4100); });
      expect(screen.getByText(/(after the hub restarts|после перезапуска хаба)/)).toBeTruthy(); // a failed read keeps the block
      await act(async () => { jest.advanceTimersByTime(4100); });
      await waitFor(() => expect(screen.queryByText(/(after the hub restarts|после перезапуска хаба)/)).toBeNull());
    } finally { jest.useRealTimers(); }
  });

  it('bus 1 with modules on it cannot be switched off', async () => {
    mockApi.getI2c.mockResolvedValue({ ...state({ up: true, configured: true, sda: 17, scl: 18 }), modules: [{ module: 1, bus: 1, addr: 0x53, present: true }] });
    render(<ConfirmProvider><I2cBusesPanel /></ConfirmProvider>);
    expect((await screen.findByRole('button', { name: /(Switch bus 1 off|Выключить шину 1)/ })).hasAttribute('disabled')).toBe(true);
  });
});

import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { LedPanel, ledWords } from './LedPanel';
import { parseLed } from '../../types/led';

const mockApi = { getLed: jest.fn(), setLed: jest.fn() };
jest.mock('../../services/api', () => ({ getApiClient: () => mockApi }));

/** As measured on S3 .157 (fw 1.3.0) 25.11. */
const MEASURED = {
  mode: 'full', leds: { green_gpio: 4, red_gpio: 5 },
  green: { id: 'G7', state: 'normal operation', pattern: 'heartbeat' }, red: null, process: null, events_pending: false,
};

describe('LedPanel (hub status LEDs, fw 1.3.0)', () => {
  beforeEach(() => { mockApi.getLed.mockReset(); mockApi.setLed.mockReset(); });

  it('parses the measured shape', () => {
    const s = parseLed(MEASURED);
    expect(s.mode).toBe('full');
    expect(s.green?.id).toBe('G7');
    expect(s.red).toBeNull();
    expect(s.events_pending).toBe(false);
    expect(parseLed({}).green).toBeNull();
  });

  it('says a red code in words with its flash count; an unknown id falls back to the hub text', () => {
    expect(ledWords({ id: 'R2', state: 'module absent', pattern: '2x' })).toMatch(/module is missing.*2 flashes|модуль пропал.*2/);
    expect(ledWords({ id: 'G9', state: 'something new', pattern: 'x' })).toBe('something new');
    expect(ledWords(null)).toMatch(/off|не горит/);
  });

  it('shows the state, switches the mode and finds the hub', async () => {
    mockApi.getLed.mockResolvedValue(parseLed(MEASURED));
    mockApi.setLed.mockImplementation(async (p: { mode?: string }) => parseLed({ ...MEASURED, mode: p.mode ?? 'full' }));
    render(<LedPanel />);
    expect(await screen.findByText(/working normally|работает нормально/)).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: /^(Quiet|Тихо)$/ }));
    await waitFor(() => expect(mockApi.setLed).toHaveBeenCalledWith({ mode: 'quiet' }));
    fireEvent.click(await screen.findByRole('button', { name: /Find this hub|Найти хаб/ }));
    await waitFor(() => expect(mockApi.setLed).toHaveBeenCalledWith({ identify: true }));
  });

  it('offers to acknowledge a pending event', async () => {
    const ev = { ...MEASURED, red: { id: 'R7', state: 'unexpected restart', pattern: '7x' }, events_pending: true };
    mockApi.getLed.mockResolvedValue(parseLed(ev));
    mockApi.setLed.mockResolvedValue(parseLed({ ...ev, red: null, events_pending: false }));
    render(<LedPanel />);
    fireEvent.click(await screen.findByRole('button', { name: /Acknowledge|Отметить/ }));
    await waitFor(() => expect(mockApi.setLed).toHaveBeenCalledWith({ ack: true }));
    expect(await screen.findByText(/Acknowledged|Отмечено/)).toBeTruthy();
  });
});

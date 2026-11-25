import React from 'react';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { MqttCaPanel, tlsStateLine } from './MqttCaPanel';
import { ConfirmProvider } from '../common/ConfirmDialog';

const mockTransport = { getMqttCa: jest.fn(), setMqttCa: jest.fn(), deleteMqttCa: jest.fn() };
const mockSession = { transport: mockTransport };
jest.mock('../../core', () => ({ useDeviceSession: () => mockSession }));
jest.mock('../../services/api', () => ({ getApiClient: () => ({ rebootDevice: jest.fn() }) }));

const PEM = '-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n';
const cert = { subject: 'CN=Home CA', issuer: 'CN=Home CA', valid_from: '2026-01-01T00:00:00Z', valid_to: '2036-01-01T00:00:00Z', ca: true };
const view = () => render(<ConfirmProvider><MqttCaPanel /></ConfirmProvider>);
const pick = (text: string, name = 'ca.pem') => {
  const input = document.querySelector('input[type=file]') as HTMLInputElement;
  fireEvent.change(input, { target: { files: [new File([text], name)] } });
};

describe('MqttCaPanel (hub 1.1.8 broker CA, root mqtt-ca-ui)', () => {
  beforeEach(() => {
    Object.values(mockTransport).forEach((f) => f.mockReset());
    mockTransport.getMqttCa.mockResolvedValue({ present: false, certs: [] });
  });

  it('uploads a PEM, lists the certificate and asks for a restart', async () => {
    mockTransport.setMqttCa.mockResolvedValue({ present: true, certs: [cert], requiresReboot: true });
    view();
    expect(await screen.findByText(/None uploaded|Не загружен/)).toBeTruthy();
    pick(PEM);
    await waitFor(() => expect(mockTransport.setMqttCa).toHaveBeenCalledWith(PEM));
    expect(await screen.findByText('CN=Home CA')).toBeTruthy();
    expect(screen.getByText(/after a restart|после перезагрузки/)).toBeTruthy();
  });

  it('never sends a file with a private key, or one that isn\'t PEM', async () => {
    view();
    await screen.findByText(/None uploaded|Не загружен/);
    pick(`${PEM}-----BEGIN PRIVATE KEY-----\nx\n-----END PRIVATE KEY-----\n`);
    expect(await screen.findByText(/private key|закрытый ключ/)).toBeTruthy();
    pick('hello');
    expect(await screen.findByText(/isn't a PEM|не сертификат PEM/)).toBeTruthy();
    expect(mockTransport.setMqttCa).not.toHaveBeenCalled();
  });

  it('a 400 field=pem reads in plain words', async () => {
    mockTransport.setMqttCa.mockRejectedValue(Object.assign(new Error('HTTP 400'), { response: { status: 400, data: { field: 'pem' } } }));
    view();
    await screen.findByText(/None uploaded|Не загружен/);
    pick(PEM);
    expect(await screen.findByText(/refused the file|не принял файл/)).toBeTruthy();
  });

  it('removes after a confirmation', async () => {
    mockTransport.getMqttCa.mockResolvedValue({ present: true, certs: [cert] });
    mockTransport.deleteMqttCa.mockResolvedValue({ present: false, certs: [], requiresReboot: true });
    view();
    fireEvent.click(await screen.findByRole('button', { name: /^(Remove|Удалить)$/ }));
    const dialog = await screen.findByRole('dialog');
    const bs = dialog.querySelectorAll('button');
    fireEvent.click(bs[bs.length - 1]);
    await waitFor(() => expect(mockTransport.deleteMqttCa).toHaveBeenCalled());
    expect(await screen.findByText(/Certificate removed|Сертификат удалён/)).toBeTruthy();
  });

  it('TLS state: off says nothing; refusals are errors in plain words', () => {
    expect(tlsStateLine('off')).toBeNull();
    expect(tlsStateLine(undefined)).toBeNull();
    expect(tlsStateLine('ok')?.sev).toBe('success');
    expect(tlsStateLine('unknown_ca')).toEqual({ sev: 'error', text: expect.stringMatching(/CA|УЦ/) });
    expect(tlsStateLine('name_mismatch')?.sev).toBe('error');
  });
});

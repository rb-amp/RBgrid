import React from 'react';
import { render, screen, waitFor } from '@testing-library/react';
import { StatsMediaAlert } from './StatsMediaAlert';
import { BASELINE_CAPABILITIES } from '../../core/schema/capabilities';

let mockCaps = { ...BASELINE_CAPABILITIES };
jest.mock('../../core', () => ({ useCapabilities: () => mockCaps }));
const mockApi = { getHubVersion: jest.fn() };
jest.mock('../../services/api', () => ({ getApiClient: () => mockApi }));
jest.mock('../../services/connection', () => ({ isDemo: () => false }));

const TEXT = /statistics memory chip is not responding|Чип памяти статистики не отвечает/i;

describe('StatsMediaAlert (fw 1.3.x stats_media)', () => {
  beforeEach(() => { mockApi.getHubVersion.mockReset(); mockCaps = { ...BASELINE_CAPABILITIES }; });

  it('shows the warning when the schema says the chip is missing', () => {
    mockCaps = { ...BASELINE_CAPABILITIES, statsMedia: 'none' };
    mockApi.getHubVersion.mockResolvedValue({ version: '1.3.1' });
    render(<StatsMediaAlert />);
    expect(screen.getByRole('alert')).toHaveTextContent(TEXT);
  });

  it('picks up a chip lost mid-session from /api/system/version', async () => {
    mockCaps = { ...BASELINE_CAPABILITIES, statsMedia: 'external' };
    mockApi.getHubVersion.mockResolvedValue({ stats_media: 'none' });
    render(<StatsMediaAlert />);
    await waitFor(() => expect(screen.getByRole('alert')).toHaveTextContent(TEXT));
  });

  it.each(['internal', 'external', undefined])('nothing for %s', async (m) => {
    mockApi.getHubVersion.mockResolvedValue(m ? { stats_media: m } : { version: '1.2.0' });
    render(<StatsMediaAlert />);
    await waitFor(() => expect(mockApi.getHubVersion).toHaveBeenCalled());
    expect(screen.queryByRole('alert')).toBeNull();
  });
});

import React from 'react';
import { render, screen } from '@testing-library/react';
import { NoHistoryNote } from './NoHistoryNote';
import { buildUnitsJoin } from '../../hooks/useUnitsConfig';

let mockSlots: number | null = null;
jest.mock('../../core', () => ({ useCapabilities: () => ({ historySlots: mockSlots }) }));

describe('live-only points (v1.1.2 history flag)', () => {
  it('counts the points that keep history; null when the hub does not say', () => {
    expect(buildUnitsJoin([{ unit_id: 6, history: true }, { unit_id: 7, history: false }, { unit_id: 8, history: true }]).historyCount).toBe(2);
    expect(buildUnitsJoin([{ unit_id: 6 }]).historyCount).toBeNull();
  });
  it('says why: all 12 mockSlots taken, else a restart', () => {
    const { rerender, container } = render(<NoHistoryNote history={false} historyCount={12} />);
    expect(screen.getByText(/12/)).toBeTruthy();
    rerender(<NoHistoryNote history={false} historyCount={5} />);
    expect(container.textContent).not.toMatch(/12/);
    rerender(<NoHistoryNote history={undefined} historyCount={12} />);
    expect(container.textContent).toBe('');
  });
  it('uses the hub\'s history_slots: 49 on the S3 chip, no «full» reason at 0 (chip missing)', () => {
    mockSlots = 49;
    const { rerender, container } = render(<NoHistoryNote history={false} historyCount={12} />);
    expect(container.textContent).not.toMatch(/12|49/);
    rerender(<NoHistoryNote history={false} historyCount={49} />);
    expect(container.textContent).toMatch(/49/);
    mockSlots = 0;
    rerender(<NoHistoryNote history={false} historyCount={49} />);
    expect(container.textContent).not.toMatch(/49/);
    mockSlots = null;
  });
});

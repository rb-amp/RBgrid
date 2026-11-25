import React, { useState } from 'react';
import { render, screen, fireEvent, act, renderHook } from '@testing-library/react';
import { SectionFormsProvider, useSectionForm, useSectionForms } from './sectionForms';
import { markRebootPending, noteHubBoot, useRebootPending, clearRebootPending } from './rebootPending';

const save = jest.fn(async () => true);

const Panel: React.FC = () => {
  const [v, setV] = useState('a');
  const inSection = useSectionForm({ id: 'p', label: 'Panel', dirty: v !== 'a', invalid: v === '', save, reset: () => setV('a') });
  return (
    <div>
      <input aria-label="field" value={v} onChange={(e) => setV(e.target.value)} />
      {!inSection && <button type="button">own save</button>}
    </div>
  );
};

const Bar: React.FC = () => {
  const forms = useSectionForms();
  const dirty = forms.filter((f) => f.dirty);
  return (
    <div>
      <span>dirty {dirty.length}</span>
      <button type="button" disabled={!dirty.length || dirty.some((f) => f.invalid)} onClick={() => dirty.forEach((f) => void f.save())}>save all</button>
      <button type="button" onClick={() => dirty.forEach((f) => f.reset())}>cancel all</button>
    </div>
  );
};

describe('section forms (one Save per settings section)', () => {
  it('outside a section the panel keeps its own Save', () => {
    render(<Panel />);
    expect(screen.getByText('own save')).toBeInTheDocument();
  });

  it('inside a section: the bar counts the draft, saves and cancels it; an invalid draft blocks Save', async () => {
    save.mockClear();
    render(<SectionFormsProvider><Bar /><Panel /></SectionFormsProvider>);
    expect(screen.queryByText('own save')).toBeNull();
    expect(screen.getByText('dirty 0')).toBeInTheDocument();
    fireEvent.change(screen.getByLabelText('field'), { target: { value: '' } });
    expect(screen.getByText('dirty 1')).toBeInTheDocument();
    expect(screen.getByText('save all')).toBeDisabled();
    fireEvent.change(screen.getByLabelText('field'), { target: { value: 'b' } });
    await act(async () => { fireEvent.click(screen.getByText('save all')); });
    expect(save).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByText('cancel all'));
    expect(screen.getByText('dirty 0')).toBeInTheDocument();
  });
});

describe('waits for a restart', () => {
  beforeEach(() => clearRebootPending());
  it('marks collect once each; a new boot_id clears them', () => {
    const { result } = renderHook(() => useRebootPending());
    act(() => { markRebootPending('Time'); markRebootPending('Time'); markRebootPending('MQTT'); });
    expect(result.current).toEqual(['Time', 'MQTT']);
    act(() => { noteHubBoot(11); });
    expect(result.current.length).toBe(2); // the first boot_id seen is not a restart
    act(() => { noteHubBoot(12); });
    expect(result.current).toEqual([]);
  });
});

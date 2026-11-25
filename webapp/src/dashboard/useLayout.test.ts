import { renderHook, act, waitFor } from '@testing-library/react';
import { useLayout } from './useLayout';
import { writeBrowser, readBrowser } from './layoutStore';
import { LayoutDoc } from './layout';
import { SiteTraits } from './siteTraits';

const mockApi = { getUiState: jest.fn(), putUiState: jest.fn(), deleteUiState: jest.fn() };
jest.mock('../services/api', () => ({ ...jest.requireActual('../services/api'), getApiClient: () => mockApi }));
jest.mock('../services/connection', () => ({ ...jest.requireActual('../services/connection'), isDemo: () => false, getBaseUrl: () => 'hub.test' }));

const traits: SiteTraits = {
  phases: 1, hasInput: true, hasGeneration: false, hasStorage: false, evStations: 0, hasInputLimit: false,
  lineCount: 3, hasTariff: true, hasDynamicPrice: false, money: true,
};
const mine: LayoutDoc = { v: 1, boards: [{ id: 'main', name: 'Mine', w: [['now', 'full']] }] };
const theirs: LayoutDoc = { v: 1, boards: [{ id: 'main', name: 'Theirs', w: [['today', 'half']] }] };

describe('useLayout — the hub store (fw 1.3.0)', () => {
  beforeEach(() => { localStorage.clear(); Object.values(mockApi).forEach((f) => f.mockReset()); });

  it('a hub without the store: the browser only, no hub calls', async () => {
    const { result } = renderHook(() => useLayout(traits, false));
    expect(result.current.where).toBe('browser');
    await act(() => result.current.save(mine));
    expect(mockApi.putUiState).not.toHaveBeenCalled();
    expect(result.current.doc.boards[0].name).toBe('Mine');
  });

  it('the hub layout replaces the browser copy and becomes the copy', async () => {
    writeBrowser(mine, 3);
    mockApi.getUiState.mockResolvedValue({ doc: theirs, rev: 7 });
    const { result } = renderHook(() => useLayout(traits, true));
    expect(result.current.doc.boards[0].name).toBe('Mine'); // drawn at once
    await waitFor(() => expect(result.current.doc.boards[0].name).toBe('Theirs'));
    expect(readBrowser()?.rev).toBe(7);
  });

  it('nothing on the hub yet: the browser layout goes to the hub with the first save, from revision 0', async () => {
    writeBrowser(mine, 5);
    mockApi.getUiState.mockResolvedValue(null);
    mockApi.putUiState.mockResolvedValue({ ok: true, rev: 1 });
    const { result } = renderHook(() => useLayout(traits, true));
    await waitFor(() => expect(mockApi.getUiState).toHaveBeenCalled());
    await act(async () => { await Promise.resolve(); }); // the hub's answer lands
    await act(() => result.current.save(mine));
    expect(mockApi.putUiState).toHaveBeenCalledWith(mine, 0);
  });

  it('409: the user picks — «keep mine» saves on top of the hub revision, nothing lost', async () => {
    mockApi.getUiState.mockResolvedValue({ doc: theirs, rev: 4 });
    mockApi.putUiState.mockResolvedValueOnce({ ok: false, rev: 5 }).mockResolvedValueOnce({ ok: true, rev: 6 });
    const { result } = renderHook(() => useLayout(traits, true));
    await waitFor(() => expect(result.current.doc.boards[0].name).toBe('Theirs'));
    await act(() => result.current.save(mine));
    expect(mockApi.putUiState).toHaveBeenLastCalledWith(mine, 4);
    expect(result.current.conflict?.rev).toBe(5);
    await act(() => result.current.keepMine());
    expect(mockApi.putUiState).toHaveBeenLastCalledWith(mine, 5);
    expect(result.current.conflict).toBeNull();
    expect(result.current.doc.boards[0].name).toBe('Mine');
  });

  it('«my own view on this device»: saves stay in the browser, the hub untouched', async () => {
    mockApi.getUiState.mockResolvedValue(null);
    const { result } = renderHook(() => useLayout(traits, true));
    act(() => result.current.setLocalOnly(true));
    expect(result.current.where).toBe('device');
    await act(() => result.current.save(mine));
    expect(mockApi.putUiState).not.toHaveBeenCalled();
    expect(readBrowser(true)?.doc).toEqual(mine);
  });
});

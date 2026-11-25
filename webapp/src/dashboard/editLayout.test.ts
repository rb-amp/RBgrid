import {
  moveBlock, setBlockSize, setBlockOpts, removeBlock, addBlock, addBoard, renameBoard, removeBoard, moveBoard, togglePoint, sameLayout,
} from './editLayout';
import { LayoutDoc } from './layout';

const doc: LayoutDoc = { v: 1, boards: [
  { id: 'main', name: '', w: [['now', 'half'], ['today', 'half'], ['flow', 'full', { view: 'home' }]] },
  { id: 'detail', name: '', w: [['kpi', 'full']] },
] };
const ids = (d: LayoutDoc, b = 'main') => d.boards.find((x) => x.id === b)!.w.map((p) => p[0]);

describe('layout edits', () => {
  it('move, resize, set options, remove, add', () => {
    expect(ids(moveBlock(doc, 'main', 2, 0))).toEqual(['flow', 'now', 'today']);
    expect(ids(moveBlock(doc, 'main', 0, 9))).toEqual(['today', 'flow', 'now']);
    expect(setBlockSize(doc, 'main', 0, 'full').boards[0].w[0]).toEqual(['now', 'full']);
    expect(setBlockSize(doc, 'main', 2, 'full').boards[0].w[2]).toEqual(['flow', 'full', { view: 'home' }]);
    expect(setBlockOpts(doc, 'main', 2, { view: 'tree' }).boards[0].w[2][2]).toEqual({ view: 'tree' });
    expect(ids(removeBlock(doc, 'main', 1))).toEqual(['now', 'flow']);
    expect(ids(addBlock(doc, 'detail', ['alarms', 'third']), 'detail')).toEqual(['kpi', 'alarms']);
    expect(doc.boards[0].w.length).toBe(3); // the original stays
  });
  it('boards: add with a unique id, rename, move, remove — the last board stays', () => {
    const { doc: d2, id } = addBoard(doc, '  Garage  ');
    expect(id).toBe('b3');
    expect(d2.boards[2]).toEqual({ id: 'b3', name: 'Garage', w: [] });
    expect(renameBoard(d2, 'b3', 'Solar').boards[2].name).toBe('Solar');
    expect(moveBoard(d2, 'b3', -1).boards.map((b) => b.id)).toEqual(['main', 'b3', 'detail']);
    expect(moveBoard(d2, 'main', -1)).toBe(d2);
    expect(removeBoard(d2, 'detail').boards.map((b) => b.id)).toEqual(['main', 'b3']);
    const one = { v: 1 as const, boards: [doc.boards[0]] };
    expect(removeBoard(one, 'main')).toBe(one);
  });
  it('a pinned point is not hidden and back', () => {
    const hidden = togglePoint(doc, 'hide', '0012', true);
    expect(hidden.hide).toEqual(['0012']);
    const pinned = togglePoint(hidden, 'pins', '0012', true);
    expect(pinned.pins).toEqual(['0012']);
    expect(pinned.hide).toEqual([]);
    expect(togglePoint(pinned, 'pins', '0012', false).pins).toEqual([]);
  });
  it('same layout by content', () => {
    expect(sameLayout(doc, JSON.parse(JSON.stringify(doc)))).toBe(true);
    expect(sameLayout(doc, moveBlock(doc, 'main', 0, 1))).toBe(false);
  });
});

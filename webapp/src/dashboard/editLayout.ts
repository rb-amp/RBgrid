/**
 * Layout edits (SPEC §2.7) as pure functions on the document — the «Customise» mode keeps a draft
 * and applies these; «Done» saves the draft. Every function returns a new document.
 */
import { Board, LayoutDoc, Placed, Size, WidgetOpts, LIMITS } from './layout';

const mapBoard = (d: LayoutDoc, id: string, f: (b: Board) => Board): LayoutDoc =>
  ({ ...d, boards: d.boards.map((b) => (b.id === id ? f(b) : b)) });

/** Move the block at `from` to index `to` (clamped). */
export function moveBlock(d: LayoutDoc, board: string, from: number, to: number): LayoutDoc {
  return mapBoard(d, board, (b) => {
    if (from < 0 || from >= b.w.length) return b;
    const w = [...b.w];
    const [p] = w.splice(from, 1);
    w.splice(Math.max(0, Math.min(w.length, to)), 0, p);
    return { ...b, w };
  });
}

export const setBlockSize = (d: LayoutDoc, board: string, i: number, size: Size): LayoutDoc =>
  mapBoard(d, board, (b) => ({ ...b, w: b.w.map((p, k) => (k === i ? ([p[0], size, p[2]].filter((x) => x !== undefined) as Placed) : p)) }));

export const setBlockOpts = (d: LayoutDoc, board: string, i: number, opts: WidgetOpts): LayoutDoc =>
  mapBoard(d, board, (b) => ({ ...b, w: b.w.map((p, k) => (k === i ? [p[0], p[1], opts] as Placed : p)) }));

export const removeBlock = (d: LayoutDoc, board: string, i: number): LayoutDoc =>
  mapBoard(d, board, (b) => ({ ...b, w: b.w.filter((_, k) => k !== i) }));

/** Add a block at the end of the board — next to «Add block», in the order added (round 2 business N3:
 *  at the top it reversed that order); the page scrolls to it. The board's limit holds. */
export const addBlock = (d: LayoutDoc, board: string, p: Placed): LayoutDoc =>
  mapBoard(d, board, (b) => (b.w.length >= LIMITS.blocks ? b : { ...b, w: [...b.w, p] }));

/** A new empty board with a unique id; returns the document and the id. */
export function addBoard(d: LayoutDoc, name: string): { doc: LayoutDoc; id: string } {
  if (d.boards.length >= LIMITS.boards) return { doc: d, id: d.boards[d.boards.length - 1].id };
  let n = d.boards.length + 1;
  while (d.boards.some((b) => b.id === `b${n}`)) n += 1;
  const id = `b${n}`;
  return { doc: { ...d, boards: [...d.boards, { id, name: name.trim().slice(0, 40), w: [] }] }, id };
}

export const renameBoard = (d: LayoutDoc, id: string, name: string): LayoutDoc =>
  mapBoard(d, id, (b) => ({ ...b, name: name.trim().slice(0, 40) }));

/** Remove a board; the last one stays (a layout always has a board). */
export const removeBoard = (d: LayoutDoc, id: string): LayoutDoc =>
  (d.boards.length <= 1 ? d : { ...d, boards: d.boards.filter((b) => b.id !== id) });

export function moveBoard(d: LayoutDoc, id: string, dir: -1 | 1): LayoutDoc {
  const i = d.boards.findIndex((b) => b.id === id);
  const j = i + dir;
  if (i < 0 || j < 0 || j >= d.boards.length) return d;
  const boards = [...d.boards];
  [boards[i], boards[j]] = [boards[j], boards[i]];
  return { ...d, boards };
}

/** Pin / unpin and hide / show a point on the Overview (SPEC §2.8). */
export function togglePoint(d: LayoutDoc, list: 'pins' | 'hide', hex: string, on: boolean): LayoutDoc {
  const cur = new Set(d[list] ?? []);
  if (on) cur.add(hex); else cur.delete(hex);
  const other = list === 'pins' ? 'hide' : 'pins';
  const rest = on ? (d[other] ?? []).filter((x) => x !== hex) : d[other];
  return { ...d, [list]: Array.from(cur), [other]: rest };
}

/** Same content (to tell a draft from the saved layout). */
export const sameLayout = (a: LayoutDoc, b: LayoutDoc): boolean => JSON.stringify(a) === JSON.stringify(b);

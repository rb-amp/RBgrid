/**
 * DashboardBoard — draws one board of the layout (SPEC §2.4): a 12-column grid, a block = its size
 * (⅓ · ½ · whole row on wide screens, one column on phones in the same order). Each block sits in a
 * PanelBoundary, so one failing block doesn't take the others down. A block this hub can't show
 * (e.g. no generation yet) is skipped, not removed from the layout.
 * With `edit`, every block gets the «Customise» frame (BoardEditor.BlockFrame).
 */
import React, { useState } from 'react';
import { Box, Button, Grid, Typography } from '@mui/material';
import AddIcon from '@mui/icons-material/Add';
import { PanelBoundary } from '../components/common/PanelBoundary';
import { useDashboardData } from './DashboardData';
import { Board, Size, SIZE_COLS } from './layout';
import { widgetDef, usesEv, EvDataProvider } from './registry';
import { BlockFrame } from './BoardEditor';
import { t } from '../i18n';

/** Blocks dimmed while the link is lost: their figures are the last ones, never read as live (L3P-13). */
const DIM_WHEN_STALE = new Set(['now', 'today', 'kpi']);

export interface BoardEditHandlers {
  move(from: number, to: number): void;
  size(i: number, s: Size): void;
  remove(i: number): void;
  settings(i: number): void;
  add(): void;
}

export const DashboardBoard: React.FC<{ board: Board; edit?: BoardEditHandlers }> = ({ board, edit }) => {
  const m = useDashboardData();
  const [dragFrom, setDragFrom] = useState<number | null>(null);
  const [said, setSaid] = useState('');
  const shown = board.w
    .map((p, i) => ({ p, i, def: widgetDef(p[0]) }))
    .filter((x) => x.def && x.def.available(m.traits));
  const moveTo = (from: number, to: number) => {
    if (!edit || from === to) return;
    edit.move(from, to);
    const name = t(widgetDef(board.w[from][0])?.titleKey ?? '');
    setSaid(t('board.movedTo', { name, n: to + 1, of: board.w.length }));
  };
  const grid = (
    <Grid container spacing={2} alignItems="stretch">
      {shown.map(({ p, i, def }) => {
        const cols = SIZE_COLS[p[1]];
        const R = def!.Render;
        const block = (
          <PanelBoundary name={t(def!.titleKey)}>
            <R opts={p[2] ?? {}} size={p[1]} />
          </PanelBoundary>
        );
        return (
          <Grid item key={`${p[0]}-${i}`} xs={cols.xs} md={cols.md} lg={cols.lg}
            sx={{ display: 'flex', flexDirection: 'column', '& > *': { flex: p[1] === 'full' ? undefined : 1 },
              ...(m.stale && !edit && DIM_WHEN_STALE.has(p[0]) ? { opacity: 0.55, transition: 'opacity .3s' } : {}) }}>
            {edit ? (
              <BlockFrame def={def!} index={i} count={board.w.length} size={p[1]}
                onMove={(to) => moveTo(i, to)} onSize={(s) => edit.size(i, s)} onRemove={() => edit.remove(i)}
                onSettings={def!.settings?.length ? () => edit.settings(i) : undefined}
                onDragStart={() => setDragFrom(i)} onDropHere={() => { if (dragFrom != null) moveTo(dragFrom, i); setDragFrom(null); }}
                dragging={dragFrom === i}>
                {block}
              </BlockFrame>
            ) : block}
          </Grid>
        );
      })}
      {edit && (
        <Grid item xs={12}>
          <Button variant="outlined" startIcon={<AddIcon />} onClick={edit.add} fullWidth sx={{ py: 1.5, borderStyle: 'dashed' }}>{t('board.addBlock')}</Button>
          {shown.length === 0 && <Typography variant="body2" color="text.secondary" sx={{ mt: 1, textAlign: 'center' }}>{t('board.emptyBoard')}</Typography>}
        </Grid>
      )}
    </Grid>
  );
  return (
    <>
      {edit && <Box aria-live="polite" sx={{ position: 'absolute', width: '1px', height: '1px', overflow: 'hidden', clip: 'rect(0 0 0 0)', whiteSpace: 'nowrap' }}>{said}</Box>}
      {usesEv(shown.map((x) => x.p[0])) ? <EvDataProvider>{grid}</EvDataProvider> : grid}
    </>
  );
};

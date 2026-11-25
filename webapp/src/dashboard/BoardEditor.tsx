/**
 * The «Customise» mode pieces (SPEC §2.7): the frame around a block (drag handle, up / down, size,
 * settings, remove), the catalogue to add a block, a block's settings, the board name dialog and
 * the sticky bar «Cancel · Back to standard · Done». Blocks keep showing live data but can't be
 * pressed while editing. Reordering works by dragging the handle (pointer), with the up / down
 * buttons (phones) and with the arrow keys on the focused handle (announced to screen readers).
 */
import React, { useState } from 'react';
import {
  Box, Button, Checkbox, Dialog, DialogActions, DialogContent, DialogTitle, FormControlLabel, IconButton, List,
  ListItemButton, ListItemText, MenuItem, Paper, Radio, RadioGroup, Stack, TextField, ToggleButton, ToggleButtonGroup,
  Tooltip, Typography,
} from '@mui/material';
import DragIndicatorIcon from '@mui/icons-material/DragIndicator';
import ArrowUpwardIcon from '@mui/icons-material/ArrowUpward';
import ArrowDownwardIcon from '@mui/icons-material/ArrowDownward';
import SettingsIcon from '@mui/icons-material/SettingsOutlined';
import CloseIcon from '@mui/icons-material/Close';
import { useDsTokens } from '../theme/useDsTokens';
import { Size, WidgetOpts } from './layout';
import { WidgetDef, WIDGETS } from './registry';
import { SiteTraits } from './siteTraits';
import { t } from '../i18n';

const SIZE_LABEL: Record<Size, string> = { third: '⅓', half: '½', full: '1' };

export interface BlockFrameProps {
  def: WidgetDef;
  index: number;
  count: number;
  size: Size;
  onMove: (to: number) => void;
  onSize: (s: Size) => void;
  onRemove: () => void;
  onSettings?: () => void;
  /** Native drag: the board tracks which block is being dragged. */
  onDragStart: () => void;
  onDropHere: () => void;
  dragging: boolean;
  children: React.ReactNode;
}

export const BlockFrame: React.FC<BlockFrameProps> = ({
  def, index, count, size, onMove, onSize, onRemove, onSettings, onDragStart, onDropHere, dragging, children,
}) => {
  const ds = useDsTokens();
  const [over, setOver] = useState(false);
  const title = t(def.titleKey);
  return (
    <Box
      onDragOver={(e) => { e.preventDefault(); setOver(true); }}
      onDragLeave={() => setOver(false)}
      onDrop={(e) => { e.preventDefault(); setOver(false); onDropHere(); }}
      sx={{
        position: 'relative', borderRadius: 3, outline: `2px dashed ${over ? ds.primary : ds.border}`, outlineOffset: 2,
        opacity: dragging ? 0.45 : 1, height: '100%', display: 'flex', flexDirection: 'column',
      }}
    >
      <Stack direction="row" alignItems="center" spacing={0.5} sx={{ px: 1, py: 0.5, bgcolor: ds.surfaceRaised ?? ds.surface, borderRadius: '12px 12px 0 0', borderBottom: `1px solid ${ds.border}` }}>
        <Tooltip title={t('board.dragHint')}>
          <IconButton size="small" draggable onDragStart={(e) => { e.dataTransfer.effectAllowed = 'move'; e.dataTransfer.setData('text/plain', String(index)); onDragStart(); }}
            aria-label={t('board.moveHandle', { name: title })} aria-roledescription={t('board.handleRole')}
            onKeyDown={(e) => {
              if (e.key === 'ArrowUp' && index > 0) { e.preventDefault(); onMove(index - 1); }
              if (e.key === 'ArrowDown' && index < count - 1) { e.preventDefault(); onMove(index + 1); }
            }}
            sx={{ cursor: 'grab' }}>
            <DragIndicatorIcon fontSize="small" />
          </IconButton>
        </Tooltip>
        <Typography variant="body2" fontWeight={700} noWrap sx={{ flex: 1, minWidth: 0 }}>{title}</Typography>
        {def.sizes.length > 1 && (
          <ToggleButtonGroup size="small" exclusive value={size} onChange={(_, v) => v && onSize(v)} aria-label={t('board.sizeAria', { name: title })}
            sx={{ display: { xs: 'none', md: 'inline-flex' }, '& .MuiToggleButton-root': { py: 0.25, px: 1, minWidth: 32 } }}>
            {def.sizes.map((s) => <ToggleButton key={s} value={s} aria-label={t(`board.size_${s}`)}>{SIZE_LABEL[s]}</ToggleButton>)}
          </ToggleButtonGroup>
        )}
        <IconButton size="small" disabled={index === 0} onClick={() => onMove(index - 1)} aria-label={t('board.up', { name: title })}><ArrowUpwardIcon fontSize="small" /></IconButton>
        <IconButton size="small" disabled={index >= count - 1} onClick={() => onMove(index + 1)} aria-label={t('board.down', { name: title })}><ArrowDownwardIcon fontSize="small" /></IconButton>
        {onSettings && <IconButton size="small" onClick={onSettings} aria-label={t('board.settings', { name: title })}><SettingsIcon fontSize="small" /></IconButton>}
        <IconButton size="small" onClick={onRemove} aria-label={t('board.remove', { name: title })}><CloseIcon fontSize="small" /></IconButton>
      </Stack>
      {/* Live data, not pressable while editing — except a block's own editing controls (pointer-events: auto). */}
      {/* A block with nothing to show now (all set up, no alarms…) says so instead of a bare header
          (walkthrough W4, uiux U18). */}
      <Box data-empty={t('board.emptyNow')} sx={{ pointerEvents: 'none', userSelect: 'none', flex: 1,
        '&:empty::before': { content: 'attr(data-empty)', display: 'block', px: 1.5, py: 1, fontSize: 13, color: 'text.secondary' } }}>{children}</Box>
    </Box>
  );
};

/** «+ Add a block»: the blocks this hub can show; a single-use block already on the board is marked. */
export const CatalogDialog: React.FC<{
  open: boolean; traits: SiteTraits; present: Set<string>; onClose: () => void; onAdd: (def: WidgetDef) => void;
}> = ({ open, traits, present, onClose, onAdd }) => (
  <Dialog open={open} onClose={onClose} maxWidth="sm" fullWidth>
    <DialogTitle>{t('board.catalogTitle')}</DialogTitle>
    <DialogContent dividers sx={{ p: 0 }}>
      <List dense>
        {WIDGETS.filter((w) => w.available(traits)).map((w) => {
          const added = !w.multiple && present.has(w.id);
          return (
            <ListItemButton key={w.id} disabled={added} onClick={() => onAdd(w)}>
              <ListItemText primary={t(w.titleKey)} secondary={added ? t('board.alreadyAdded') : t(w.descKey)}
                primaryTypographyProps={{ fontWeight: 600 }} secondaryTypographyProps={{ fontSize: 13 }} />
            </ListItemButton>
          );
        })}
      </List>
    </DialogContent>
    <DialogActions><Button onClick={onClose}>{t('common.close')}</Button></DialogActions>
  </Dialog>
);

/** A block's settings (SPEC §1.5): choices and the point of a point tile. */
export const SettingsDialog: React.FC<{
  def: WidgetDef | null; value: WidgetOpts; points: { id: string; name: string }[]; onClose: () => void; onSave: (o: WidgetOpts) => void;
}> = ({ def, value, points, onClose, onSave }) => {
  const [draft, setDraft] = useState<WidgetOpts>(value);
  React.useEffect(() => { setDraft(value); }, [value, def]);
  if (!def) return null;
  return (
    <Dialog open onClose={onClose} maxWidth="xs" fullWidth>
      <DialogTitle>{t(def.titleKey)}</DialogTitle>
      <DialogContent>
        <Stack spacing={2} sx={{ pt: 1 }}>
          {(def.settings ?? []).map((s) => (s.kind === 'point' ? (
            <TextField key={s.key} select label={t(s.labelKey)} value={String(draft[s.key] ?? '')} onChange={(e) => setDraft({ ...draft, [s.key]: e.target.value })}>
              {points.map((p) => <MenuItem key={p.id} value={p.id}>{p.name}</MenuItem>)}
            </TextField>
          ) : (
            <Box key={s.key}>
              <Typography variant="body2" fontWeight={600} sx={{ mb: 0.5 }}>{t(s.labelKey)}</Typography>
              <RadioGroup value={String(draft[s.key] ?? def.defaults?.[s.key] ?? s.options[0].value)}
                onChange={(e) => { const o = s.options.find((x) => String(x.value) === e.target.value); if (o) setDraft({ ...draft, [s.key]: o.value }); }}>
                {s.options.map((o) => <FormControlLabel key={String(o.value)} value={String(o.value)} control={<Radio size="small" />} label={t(o.labelKey)} />)}
              </RadioGroup>
            </Box>
          )))}
        </Stack>
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>{t('common.cancel')}</Button>
        <Button variant="contained" onClick={() => onSave(draft)}>{t('board.apply')}</Button>
      </DialogActions>
    </Dialog>
  );
};

/** Name a new board / rename one. */
export const BoardNameDialog: React.FC<{ open: boolean; initial: string; title: string; onClose: () => void; onSave: (name: string) => void }> = ({ open, initial, title, onClose, onSave }) => {
  const [name, setName] = useState(initial);
  React.useEffect(() => { if (open) setName(initial); }, [open, initial]);
  return (
    <Dialog open={open} onClose={onClose} maxWidth="xs" fullWidth>
      <DialogTitle>{title}</DialogTitle>
      <DialogContent>
        <TextField autoFocus fullWidth margin="dense" label={t('board.boardName')} value={name} inputProps={{ maxLength: 40 }}
          onChange={(e) => setName(e.target.value)} onKeyDown={(e) => { if (e.key === 'Enter' && name.trim()) onSave(name); }} />
      </DialogContent>
      <DialogActions>
        <Button onClick={onClose}>{t('common.cancel')}</Button>
        <Button variant="contained" disabled={!name.trim()} onClick={() => onSave(name)}>{t('board.apply')}</Button>
      </DialogActions>
    </Dialog>
  );
};

/** The sticky bar while customising. `where`: the store («on the hub» / «in this browser»). */
export const EditBar: React.FC<{
  where: string; dirty: boolean; busy: boolean; localOnly?: { value: boolean; onChange: (v: boolean) => void } | null;
  onCancel: () => void; onReset: () => void; onDone: () => void;
}> = ({ where, dirty, busy, localOnly, onCancel, onReset, onDone }) => (
  // Compact on a phone (role audit 22.11: with the bottom bar it took a third of the screen): one row
  // of buttons; where the view is saved shows from a tablet up.
  <Paper elevation={6} sx={{ position: 'sticky', bottom: { xs: 68, md: 16 }, zIndex: 5, mt: 2, px: { xs: 1, sm: 2 }, py: { xs: 0.5, sm: 1.25 }, borderRadius: 3 }}>
    <Stack direction="row" alignItems="center" spacing={1} justifyContent="space-between">
      <Stack spacing={0} sx={{ display: { xs: 'none', sm: 'flex' } }}>
        <Typography variant="body2" color="text.secondary">{where}</Typography>
        {localOnly && (
          <FormControlLabel sx={{ m: 0 }} control={<Checkbox size="small" checked={localOnly.value} onChange={(e) => localOnly.onChange(e.target.checked)} />}
            label={<Typography variant="body2">{t('board.localOnly')}</Typography>} />
        )}
      </Stack>
      <Stack direction="row" spacing={1} justifyContent="flex-end" flexWrap="wrap" useFlexGap>
        <Button onClick={onCancel} disabled={busy}>{t('common.cancel')}</Button>
        <Button onClick={onReset} disabled={busy}>{t('board.reset')}</Button>
        <Button variant="contained" onClick={onDone} disabled={busy}>{t('board.done')}</Button>
      </Stack>
    </Stack>
  </Paper>
);

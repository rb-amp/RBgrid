/**
 * GroupTile — one appliance measured per phase (SPEC §2.8): the group's name, the sum of its phases
 * (power or money by the lens), «3 phases»; pressing it opens a row per phase with a link to that
 * point's page. Offline phases are counted, never summed as 0 W.
 */
import React, { useState } from 'react';
import { Box, ButtonBase, Chip, Collapse, IconButton, Stack, Typography } from '@mui/material';
import ExpandMoreIcon from '@mui/icons-material/ExpandMore';
import { useDsTokens } from '../../theme/useDsTokens';
import { UnitEntityGroup } from '../../entities/model';
import { PowerValue } from '../../economics/MoneyLens';
import { fullPower } from '../../entities/siteModel';
import { pwW } from '../common/ruFormat';
import { ROLE_ICONS, roleIcon } from '../../entities/roleVisual';
import { t } from '../../i18n';

export interface GroupTileProps {
  name: string;
  units: UnitEntityGroup[];
  /** A phase's label («L1») and its name. */
  partLabel: (g: UnitEntityGroup) => string;
  cfgOf: (g: UnitEntityGroup) => { phase?: number; phaseGroup?: boolean; role?: string } | undefined;
  stale?: boolean;
  generation?: boolean;
  onOpen: (key: string) => void;
  /** Open the appliance as one (its page); without it a tap unfolds the phases. */
  onOpenAppliance?: () => void;
  /** An action beside the header (the Overview's «⋯» menu). */
  action?: React.ReactNode;
}

export const GroupTile: React.FC<GroupTileProps> = ({ name, units, partLabel, cfgOf, stale, generation, onOpen, onOpenAppliance, action }) => {
  const ds = useDsTokens();
  const [open, setOpen] = useState(false);
  const live = units.filter((g) => g.present);
  const offline = units.length - live.length;
  const sum = live.length ? live.reduce((s, g) => s + (fullPower(g, cfgOf(g)).w ?? 0), 0) : null;
  return (
    <Box sx={{ borderRadius: 2, bgcolor: ds.surface, border: `1px solid ${ds.border}`, opacity: stale ? 0.6 : 1, height: '100%', boxSizing: 'border-box' }}>
      <Stack direction="row" alignItems="flex-start">
      <ButtonBase onClick={() => (onOpenAppliance ? onOpenAppliance() : setOpen((v) => !v))} aria-expanded={onOpenAppliance ? undefined : open}
        aria-label={onOpenAppliance ? t('pointTile.openAppliance', { name }) : undefined}
        sx={{ display: 'block', flex: 1, minWidth: 0, textAlign: 'left', p: 2, pr: 0.5, borderRadius: 2 }}>
        <Stack direction="row" spacing={1} alignItems="center" sx={{ mb: 1 }}>
          {/* The same icon as the machine's points (entities/roleVisual). */}
          <Box sx={{ width: 32, height: 32, borderRadius: 1.5, flexShrink: 0, border: `1px solid ${ds.border}`, bgcolor: ds.surfaceRaised, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke={live.length ? ds.inkMuted : ds.statusOffline} strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
              <path d={ROLE_ICONS[roleIcon(units[0], cfgOf(units[0])?.role)]} />
            </svg>
          </Box>
          <Typography sx={{ fontSize: 14, fontWeight: 600, color: ds.ink, flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{name}</Typography>
          <Chip size="small" variant="outlined" label={t('pointTile.phasesN', { n: units.length })} sx={{ height: 22 }} />
          {!onOpenAppliance && <ExpandMoreIcon fontSize="small" sx={{ color: ds.inkMuted, transform: open ? 'rotate(180deg)' : 'none', transition: 'transform .2s' }} />}
        </Stack>
        {sum == null
          ? <Typography sx={{ fontSize: 24, fontWeight: 600, color: ds.inkMuted }}>—</Typography>
          : <PowerValue w={sum} generation={generation} />}
        <Typography sx={{ fontSize: 13, mt: 0.5, fontWeight: offline ? 600 : 400 }} color={offline ? ds.statusOffline : 'text.secondary'}>
          {offline ? t('pointTile.partsOffline', { n: offline, of: units.length }) : t('pointTile.allPhases')}
        </Typography>
      </ButtonBase>
      <Stack sx={{ pt: 1, pr: 0.5 }} alignItems="center">
        {action}
        {/* The phases unfold here; the tile itself opens the appliance (role audit 22.11 H-02). */}
        {onOpenAppliance && (
          <IconButton size="small" onClick={() => setOpen((v) => !v)} aria-expanded={open} aria-label={t(open ? 'pointTile.hidePhases' : 'pointTile.showPhases')}>
            <ExpandMoreIcon fontSize="small" sx={{ transform: open ? 'rotate(180deg)' : 'none', transition: 'transform .2s' }} />
          </IconButton>
        )}
      </Stack>
      </Stack>
      <Collapse in={open} unmountOnExit>
        <Stack sx={{ px: 2, pb: 1.5 }} spacing={0.25}>
          {units.map((g) => (
            <ButtonBase key={g.unitKey} onClick={() => onOpen(g.unitKey)}
              sx={{ justifyContent: 'space-between', py: 0.75, px: 1, borderRadius: 1, '&:hover': { bgcolor: ds.surfaceRaised } }}>
              <Typography sx={{ fontSize: 13 }}>{partLabel(g)}</Typography>
              <Typography sx={{ fontSize: 13, fontVariantNumeric: 'tabular-nums', color: g.present ? ds.ink : ds.statusOffline }}>
                {g.present ? pwW(Math.abs(fullPower(g, cfgOf(g)).w ?? 0)) : t('pointTile.offline')}
              </Typography>
            </ButtonBase>
          ))}
        </Stack>
      </Collapse>
    </Box>
  );
};

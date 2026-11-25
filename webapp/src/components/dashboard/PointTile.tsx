/**
 * PointTile — a reskinned glance tile for one measurement point (spec §4.1 / §6 PointTile).
 * Replaces the look of the old UnitCard: a framed role icon, the point name, the live power
 * figure, a U·I secondary line, and a status word (never colour-only — spec §9).
 *
 * States (review 04 P0-2 / §3.4): a status word appears ONLY on deviation — offline → «—» +
 * «○ нет связи» (never «0 Вт»), alarm → «тревога»; a normal point shows nothing (no sea of
 * green «норма»). `stale` (frame lost) greys the last values without changing them.
 * ru-RU figures, tabular, «—» for null. Clickable via `onClick`.
 */
import { ROLE_ICONS, roleIcon } from '../../entities/roleVisual';
import React from 'react';
import { Box, Stack, Typography } from '@mui/material';
import { useDsTokens } from '../../theme/useDsTokens';
import { UnitEntityGroup, unitMetricValue } from '../../entities/model';
import { useBasicReversed } from '../../hooks/useHardware';
import { useCapabilities } from '../../core';
import { PowerValue } from '../../economics/MoneyLens';
import { fullPower } from '../../entities/siteModel';
import { intlLocale, t } from '../../i18n';


const nf = (v: number, d = 1) => v.toLocaleString(intlLocale(), { minimumFractionDigits: d, maximumFractionDigits: d });
function fmtPower(w: number | null): string {
  if (w == null) return '—';
  const a = Math.abs(w);
  return a >= 1000 ? `${nf(w / 1000, 2)} ${t('units.kW')}` : `${nf(w, 0)} ${t('units.W')}`;
}

export interface PointTileProps {
  group: UnitEntityGroup;
  /** Display name (configured short_name/name); defaults to the entity group name. */
  name?: string;
  roleName?: string;
  /** No fresh frame (connection lost) → last values greyed. */
  stale?: boolean;
  onClick?: () => void;
  /** A generation point: money = what it saves + earns, not export × sell (GEN-05). */
  generation?: boolean;
  /** Configured phase: a single-CT two-pole / 3P point reads one conductor → full power ×N
   *  with «≈», the same as the line table and the schema (UX audit INP-08/GEN-11). */
  cfg?: { phase?: number; phaseGroup?: boolean; role?: string };
  /** An action at the end of the name row (the Overview's «⋯» menu). */
  action?: React.ReactNode;
}

export const PointTile: React.FC<PointTileProps> = ({ group, name, roleName, stale, onClick, generation, cfg, action }) => {
  const ds = useDsTokens();
  const offline = !group.present;
  // An absent unit reports p=0 on the wire — that's not a measurement → «—».
  const power = offline ? null : unitMetricValue(group, 'power');
  // BASIC counts forward flow only: a backwards CT reads negative and isn't metered (root 18.10).
  const caps = useCapabilities();
  const reversed = useBasicReversed(group.unitKey, power, unitMetricValue(group, 'voltage'), caps.ctModels, group.attributes?.reversed);
  const alarm = group.attributes?.['alarm'] === true;
  const clip = !offline && group.attributes?.clip === true;
  const ctWarn = !offline && !clip && group.attributes?.ct_warn === true;
  const voltage = unitMetricValue(group, 'voltage');
  const current = unitMetricValue(group, 'current');
  const icon = roleIcon(group, (cfg as { role?: string } | undefined)?.role);
  const full = fullPower(group, cfg);

  const secondary = offline
    ? (roleName ?? '')
    : [voltage != null ? `${nf(voltage, 0)} ${t('units.V')}` : null, current != null ? `${nf(current, 1)} ${t('units.A')}` : null]
        .filter(Boolean).join(' · ') || (roleName ?? '');
  const secondaryText = reversed ? t('pointTile.basicReversed') : secondary;

  return (
    <Box
      onClick={onClick}
      role={onClick ? 'button' : undefined}
      tabIndex={onClick ? 0 : undefined}
      sx={{
        p: 2, borderRadius: 2, bgcolor: ds.surface, border: `1px solid ${ds.border}`, height: '100%', boxSizing: 'border-box',
        cursor: onClick ? 'pointer' : 'default', opacity: stale ? 0.6 : 1,
        transition: 'border-color .2s, background-color .2s',
        '&:hover': onClick ? { borderColor: ds.borderStrong } : undefined,
      }}
    >
      <Stack direction="row" spacing={1.5} alignItems="center" sx={{ mb: 1 }}>
        <Box sx={{
          width: 40, height: 40, borderRadius: 1.5, flexShrink: 0,
          border: `1px solid ${ds.border}`, bgcolor: ds.surfaceRaised,
          display: 'flex', alignItems: 'center', justifyContent: 'center',
        }}>
          <svg width="22" height="22" viewBox="0 0 24 24" fill="none"
            stroke={offline ? ds.statusOffline : ds.inkMuted} strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round">
            <path d={ROLE_ICONS[icon]} />
          </svg>
        </Box>
        <Typography sx={{ fontSize: 14, fontWeight: 600, color: offline ? ds.inkMuted : ds.ink, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          {name ?? group.name}
        </Typography>
        {action && <Box sx={{ ml: 'auto !important', flexShrink: 0 }} onClick={(e) => e.stopPropagation()}>{action}</Box>}
      </Stack>
      {offline ? (
        <Typography sx={{ fontSize: 24, fontWeight: 600, letterSpacing: '-0.01em', color: ds.inkMuted, fontVariantNumeric: 'tabular-nums' }}>
          {fmtPower(power)}
        </Typography>
      ) : (
        // Under the «Мощность ⇄ Деньги» lens: money mode leads with ≈ ₽/ч of this line.
        <PowerValue w={full.w} generation={generation} prefix={full.estimated && full.w != null ? '≈ ' : ''} />
      )}
      <Stack direction="row" justifyContent="space-between" alignItems="center" sx={{ mt: 0.5 }}>
        <Typography sx={{ fontSize: 13 }} color={reversed ? 'error' : 'text.secondary'}>{secondaryText}</Typography>
        {offline ? (
          <Typography sx={{ fontSize: 12, fontWeight: 600, color: ds.statusOffline, whiteSpace: 'nowrap' }}>{t(group.noChannel ? 'pointTile.noChannel' : 'pointTile.offline')}</Typography>
        ) : alarm ? (
          <Typography sx={{ fontSize: 12, fontWeight: 600, color: ds.sevAlarm, whiteSpace: 'nowrap' }}>{t('pointTile.alarm')}</Typography>
        ) : clip ? (
          // The CT at its limit (hub clip / ct_warn): the metering under-counts — a word, not colour alone.
          <Typography sx={{ fontSize: 12, fontWeight: 600, color: ds.sevAlarm, whiteSpace: 'nowrap' }}>{t('cards.ctClipShort')}</Typography>
        ) : ctWarn ? (
          <Typography sx={{ fontSize: 12, fontWeight: 600, color: ds.sevWarning, whiteSpace: 'nowrap' }}>{t('cards.ctWarnShort')}</Typography>
        ) : null}
      </Stack>
    </Box>
  );
};

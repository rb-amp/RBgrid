/**
 * I2cBusesPanel — the hub's two I2C buses (hub 1.3.0, root i2c-bus1-support 22.11): what runs on
 * each, and the pins of bus 1 (POST /api/v1/i2c/buses/1 {sda,scl} | {off:true}). The hub checks the
 * pins against its board profile (`pins_ok`, `pins_reserved` with reasons) and applies them after a
 * restart — the panel says so and offers the restart. No per-bus rule: the hub takes `max_modules`
 * on either bus (operator 22.11).
 */
import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { Alert, Box, Button, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { Card } from '@rbgrid/design-system';
import { getApiClient } from '../../services/api';
import type { I2cState } from '../../types/i2c';
import { onBus } from './addModule';
import { RebootNowButton } from '../common/RebootNowButton';
import { t } from '../../i18n';

const SETUP_BUS = 1;

/** GPIOs bus 1 may take: the board's allowed pins minus the ones another bus holds. */
export function bus1Pins(s: I2cState): number[] {
  const other = new Set<number>();
  for (const b of s.buses) if (b.bus !== SETUP_BUS && b.up) { if (b.sda != null) other.add(b.sda); if (b.scl != null) other.add(b.scl); }
  for (const r of s.pins_reserved ?? []) if (/I2C bus (\d)/.test(r.use) && !r.use.includes(`bus ${SETUP_BUS}`)) other.add(r.gpio);
  return (s.pins_ok ?? []).filter((g) => !other.has(g));
}

export const I2cBusesPanel: React.FC<{ onChanged?: () => void }> = ({ onChanged }) => {
  const [state, setState] = useState<I2cState | null>(null);
  const [editing, setEditing] = useState(false);
  const [sda, setSda] = useState<number | ''>('');
  const [scl, setScl] = useState<number | ''>('');
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  const load = useCallback(async () => {
    try { setState(await getApiClient().getI2c()); } catch { setState(null); }
  }, []);
  useEffect(() => { void load(); }, [load]);

  // After «Restart now» the hub is away for a few seconds and comes back with the new pins: re-read until
  // it no longer reports a pending restart (S3 check 23.11: the note «Restart sent» stayed until a reload).
  // Failed reads while it restarts keep the last state, so the block doesn't vanish.
  const [waitRestart, setWaitRestart] = useState(false);
  useEffect(() => {
    if (!waitRestart) return undefined;
    let alive = true;
    const t0 = Date.now();
    const id = setInterval(() => {
      if (Date.now() - t0 > 120_000) { setWaitRestart(false); return; }
      Promise.resolve().then(() => getApiClient().getI2c()).then((s2) => {
        if (!alive) return;
        if (!s2.reboot_pending) { setState(s2); setWaitRestart(false); onChanged?.(); }
      }).catch(() => undefined);
    }, 4000);
    return () => { alive = false; clearInterval(id); };
  }, [waitRestart, onChanged]);

  const pins = useMemo(() => (state ? bus1Pins(state) : []), [state]);
  if (!state) return null;
  const b1 = state.buses.find((b) => b.bus === SETUP_BUS);
  const canSetup = pins.length >= 2;
  const onB1 = onBus(state, SETUP_BUS).length;

  const startEdit = () => {
    setErr(null); setEditing(true);
    setSda(b1?.configured && b1.sda != null && pins.includes(b1.sda) ? b1.sda : '');
    setScl(b1?.configured && b1.scl != null && pins.includes(b1.scl) ? b1.scl : '');
  };

  const save = async (what: { sda: number; scl: number } | 'off') => {
    setBusy(true); setErr(null);
    try {
      const r = await getApiClient().setI2cBusPins(SETUP_BUS, what);
      if (r.status !== 'ok') { setErr(t('i2cBus.refused', { why: r.message })); return; }
      setState(r); setEditing(false); onChanged?.();
    } catch (e) {
      setErr(t('i2cBus.failed', { why: e instanceof Error ? e.message : String(e) }));
    } finally { setBusy(false); }
  };

  const line = (bus: number) => {
    const b = state.buses.find((x) => x.bus === bus);
    const n = onBus(state, bus).length;
    const pinsText = b?.configured && b.sda != null && b.scl != null ? t('i2cBus.pins', { sda: b.sda, scl: b.scl }) : null;
    const status = !b?.configured ? t('i2cBus.off') : b.up ? t('i2cBus.up') : t('i2cBus.down');
    return [t('i2cBus.busN', { bus }), status, pinsText, b?.configured ? t('i2cBus.modules', { n }) : null].filter(Boolean).join(' · ');
  };

  const pick = (label: string, value: number | '', set: (v: number) => void, other: number | '') => (
    <TextField select size="small" label={label} value={value} onChange={(e) => set(Number(e.target.value))} sx={{ minWidth: 140 }} disabled={busy}>
      {pins.map((g) => <MenuItem key={g} value={g} disabled={g === other}>GPIO{g}</MenuItem>)}
    </TextField>
  );

  const reserved = (state.pins_reserved ?? []).map((r) => `GPIO${r.gpio} — ${r.use}`).join(', ');

  return (
    <Card>
      <Stack spacing={1.25}>
        <Typography sx={{ fontWeight: 700, fontSize: 15 }}>{t('i2cBus.title')}</Typography>
        {[0, SETUP_BUS].map((bus) => (
          <Typography key={bus} variant="body2" sx={{ fontVariantNumeric: 'tabular-nums' }}>{line(bus)}</Typography>
        ))}
        {state.reboot_pending && (
          <Alert severity="warning" action={<RebootNowButton onDone={(ok) => { if (ok) setWaitRestart(true); else void load(); }} />}>{t('i2cBus.restartNeeded')}</Alert>
        )}
        {editing ? (
          <Stack spacing={1.25}>
            <Stack direction="row" spacing={1.5} flexWrap="wrap" useFlexGap>
              {pick('SDA', sda, setSda, scl)}
              {pick('SCL', scl, setScl, sda)}
            </Stack>
            <Typography variant="caption" color="text.secondary">{t('i2cBus.afterRestart')}</Typography>
            {reserved && <Typography variant="caption" color="text.secondary" sx={{ fontVariantNumeric: 'tabular-nums' }}>{t('i2cBus.reserved', { list: reserved })}</Typography>}
            <Stack direction="row" spacing={1}>
              <Button variant="contained" size="small" disabled={busy || sda === '' || scl === '' || sda === scl}
                onClick={() => void save({ sda: sda as number, scl: scl as number })}>{t('i2cBus.save')}</Button>
              <Button size="small" disabled={busy} onClick={() => setEditing(false)}>{t('i2cBus.cancel')}</Button>
            </Stack>
          </Stack>
        ) : canSetup && (
          <Box>
            <Stack direction="row" spacing={1} flexWrap="wrap" useFlexGap>
              <Button size="small" variant="outlined" onClick={startEdit}>{b1?.configured ? t('i2cBus.change') : t('i2cBus.setup')}</Button>
              {b1?.configured && (
                <Button size="small" disabled={busy || onB1 > 0} onClick={() => void save('off')}>{t('i2cBus.switchOff')}</Button>
              )}
            </Stack>
            {b1?.configured && onB1 > 0 && <Typography variant="caption" color="text.secondary">{t('i2cBus.offNeedsEmpty', { n: onB1 })}</Typography>}
          </Box>
        )}
        {err && <Alert severity="error" onClose={() => setErr(null)}>{err}</Alert>}
      </Stack>
    </Card>
  );
};

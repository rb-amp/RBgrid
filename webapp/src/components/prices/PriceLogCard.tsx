/**
 * PriceLogCard — the price the hub applied to each closed 15-min period (GET /api/v1/price?log=1,
 * fw 1.2.0; kept 40 days): pick a day, see the periods with buy (all-in), sell and where the price
 * came from, download it as CSV — a bill can be checked line by line.
 */
import React, { useEffect, useState } from 'react';
import { Alert, Box, Button, MenuItem, Stack, Table, TableBody, TableCell, TableHead, TableRow, TextField, Typography } from '@mui/material';
import { DeviceClock } from '../../core/deviceTime';
import { PriceLog } from '../../core/dynprice/types';
import { t, intlLocale } from '../../i18n';

const pad = (n: number) => String(n).padStart(2, '0');

/** The log as CSV in the reader's number format («;» + decimal comma where the UI uses one). */
export function buildPriceLogCsv(log: PriceLog, clock: DeviceClock): string {
  const comma = (1.5).toLocaleString(intlLocale()).includes(',');
  const sep = comma ? ';' : ',';
  const nf = (v: number | null) => (v == null ? '' : v.toLocaleString(intlLocale(), { maximumFractionDigits: 6, useGrouping: false }));
  const when = (ts: number) => { const p = clock.parts(ts); return `${p.y}-${pad(p.mo)}-${pad(p.d)} ${pad(p.h)}:${pad(p.mi)}`; };
  const head = [t('priceLog.colStart'), t('priceLog.colBuy', { cur: log.currency }), t('priceLog.colSell', { cur: log.currency }), t('priceLog.colProv')];
  const rows = log.rows.map((r) => [when(r.from), nf(r.buy), nf(r.sell), provText(r.prov)]);
  return `${comma ? '﻿' : ''}${[head, ...rows].map((l) => l.join(sep)).join('\r\n')}\r\n`;
}

export const provText = (prov: string): string => {
  const [base, ...mods] = prov.split('+');
  const name = t(`prices.prov.${base}`);
  return `${name === `prices.prov.${base}` ? base : name}${mods.includes('hourly_mean') ? ` · ${t('prices.prov.hourly_mean')}` : ''}`;
};

export const PriceLogCard: React.FC<{
  clock: DeviceClock;
  now: number;
  fmtRate: (v: number) => string;
  load: (from: number, until: number) => Promise<PriceLog>;
}> = ({ clock, now, fmtRate, load }) => {
  const today = clock.dayStart(now);
  const days = Array.from({ length: 40 }, (_, i) => clock.dayStart(today - i * 86400 + 3600));
  // The choice is «N days back», so it follows the hub's clock when time-config arrives after mount.
  const [back, setBack] = useState(0);
  const day = days[back];
  const [log, setLog] = useState<PriceLog | null>(null);
  const [err, setErr] = useState<string | null>(null);
  useEffect(() => {
    let on = true;
    setErr(null);
    load(day, clock.dayStart(day + 26 * 3600))
      .then((l) => { if (on) setLog(l); })
      .catch((e) => { if (on) { setLog(null); setErr((e as Error).message); } });
    return () => { on = false; };
  }, [day, load, clock]);
  const label = (ts: number) => { const p = clock.parts(ts); return new Date(Date.UTC(p.y, p.mo - 1, p.d)).toLocaleDateString(intlLocale(), { weekday: 'short', day: 'numeric', month: 'long', timeZone: 'UTC' }); };
  const hm = (ts: number) => { const p = clock.parts(ts); return `${pad(p.h)}:${pad(p.mi)}`; };

  const download = () => {
    if (!log) return;
    const url = URL.createObjectURL(new Blob([buildPriceLogCsv(log, clock)], { type: 'text/csv;charset=utf-8' }));
    const a = document.createElement('a');
    const p = clock.parts(day);
    a.href = url;
    a.download = `rbgrid_${t('priceLog.file')}_${p.y}-${pad(p.mo)}-${pad(p.d)}.csv`.replace(/\s+/g, '_');
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  };

  return (
    <Box>
      <Typography variant="caption" color="text.secondary" component="div" sx={{ mb: 1.5 }}>{t('priceLog.hint')}</Typography>
      <Stack direction="row" spacing={1} alignItems="center" flexWrap="wrap" useFlexGap sx={{ mb: 1.5 }}>
        <TextField select size="small" label={t('priceLog.day')} value={back} onChange={(e) => setBack(Number(e.target.value))} sx={{ minWidth: 220 }}>
          {days.map((d, i) => <MenuItem key={d} value={i}>{label(d)}</MenuItem>)}
        </TextField>
        <Button size="small" variant="outlined" disabled={!log?.rows.length} onClick={download}>{t('priceLog.csv')}</Button>
      </Stack>
      {err && <Alert severity="error">{t('priceLog.failed', { why: err })}</Alert>}
      {log && !log.rows.length && <Typography color="text.secondary">{t('priceLog.empty')}</Typography>}
      {log && log.rows.length > 0 && (
        <Box sx={{ maxHeight: 360, overflow: 'auto' }}>
          <Table size="small" stickyHeader>
            <TableHead>
              <TableRow>
                <TableCell>{t('priceLog.colPeriod')}</TableCell>
                <TableCell align="right">{t('prices.buy')}</TableCell>
                <TableCell align="right">{t('prices.sell')}</TableCell>
                <TableCell>{t('priceLog.colProv')}</TableCell>
              </TableRow>
            </TableHead>
            <TableBody>
              {log.rows.map((r) => (
                <TableRow key={r.from}>
                  <TableCell sx={{ whiteSpace: 'nowrap' }}>{`${hm(r.from)}–${hm(r.from + 900)}`}</TableCell>
                  <TableCell align="right" sx={{ fontVariantNumeric: 'tabular-nums' }}>{fmtRate(r.buy)}</TableCell>
                  <TableCell align="right" sx={{ fontVariantNumeric: 'tabular-nums' }}>{r.sell != null ? fmtRate(r.sell) : '—'}</TableCell>
                  <TableCell>{provText(r.prov)}</TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
          {log.more && <Typography variant="caption" color="text.secondary">{t('priceLog.more')}</Typography>}
        </Box>
      )}
    </Box>
  );
};

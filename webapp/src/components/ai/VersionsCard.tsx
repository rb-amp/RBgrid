/**
 * VersionsCard — «Changes made by assistants»: the config versions the hub keeps (the first, before any assistant change, and the newest 7 — fw 1.3.0 CFGVER_N 8;
 * tool list_config_versions via POST /api/v1/llm/call) and «Restore this version» in two steps:
 * propose_config {rollback_to} shows what changes back, rollback_config applies it.
 */
import React, { useState } from 'react';
import {
  Alert, Box, Button, Chip, Dialog, DialogActions, DialogContent, DialogTitle, Table, TableBody, TableCell, TableHead, TableRow, Typography,
} from '@mui/material';
import { LlmCallError, LlmProposal, LlmVersion, llmApi } from '../../core/llm/api';
import { DeviceClock } from '../../core/deviceTime';
import { t } from '../../i18n';
import { Section, sourceText, whenIso } from './aiShared';

const errText = (e: unknown): string => {
  const code = (e as LlmCallError)?.code;
  if (code) { const k = `ai.err_${code}`; const v = t(k); return v === k ? code : v; }
  return (e as Error)?.message ?? String(e);
};
const val = (v: string | number) => (v === '' ? '—' : String(v));

export const VersionsCard: React.FC<{ versions: LlmVersion[] | null; error: string | null; clock: DeviceClock; onChanged: () => void }> = ({ versions, error, clock, onChanged }) => {
  const [restore, setRestore] = useState<{ v: LlmVersion; p: LlmProposal } | null>(null);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState<{ sev: 'success' | 'error'; text: string } | null>(null);

  const ask = async (v: LlmVersion) => {
    setBusy(true); setMsg(null);
    try { setRestore({ v, p: await llmApi.proposeRollback(v.version) }); } catch (e) { setMsg({ sev: 'error', text: t('ai.restoreFailed', { why: errText(e) }) }); } finally { setBusy(false); }
  };
  const apply = async () => {
    if (!restore) return;
    setBusy(true);
    try {
      await llmApi.rollback(restore.v.version, restore.p.confirm_token);
      setMsg({ sev: 'success', text: t('ai.restored', { n: restore.v.version }) });
      onChanged();
    } catch (e) { setMsg({ sev: 'error', text: t('ai.restoreFailed', { why: errText(e) }) }); } finally { setBusy(false); setRestore(null); }
  };

  return (
    <Section title={t('ai.versionsTitle')}>
      <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>{t('ai.versionsLead')}</Typography>
      {error && <Alert severity="warning" sx={{ mb: 1 }}>{t('ai.versionsFailed', { why: error })}</Alert>}
      {msg && <Alert severity={msg.sev} sx={{ mb: 1 }}>{msg.text}</Alert>}
      {versions && versions.length === 0 && <Typography color="text.secondary">{t('ai.versionsNone')}</Typography>}
      {versions && versions.length > 0 && (
        <Box sx={{ overflowX: 'auto' }}>
          <Table size="small">
            <TableHead><TableRow>
              <TableCell>{t('ai.colVersion')}</TableCell><TableCell>{t('ai.colTime')}</TableCell>
              <TableCell>{t('ai.colBy')}</TableCell><TableCell>{t('ai.colWhat')}</TableCell><TableCell />
            </TableRow></TableHead>
            <TableBody>
              {versions.map((v) => (
                <TableRow key={v.version}>
                  <TableCell>{v.version}</TableCell>
                  <TableCell sx={{ whiteSpace: 'nowrap' }}>{whenIso(clock, v.time)}</TableCell>
                  <TableCell sx={{ wordBreak: 'break-word' }}>{sourceText(v.source)}</TableCell>
                  <TableCell sx={{ wordBreak: 'break-word' }}>{v.summary}</TableCell>
                  <TableCell align="right">
                    {v.current
                      ? <Chip size="small" label={t('ai.current')} />
                      : <Button size="small" disabled={busy} onClick={() => ask(v)}>{t('ai.restore')}</Button>}
                  </TableCell>
                </TableRow>
              ))}
            </TableBody>
          </Table>
        </Box>
      )}

      <Dialog open={!!restore} onClose={() => !busy && setRestore(null)} maxWidth="sm" fullWidth>
        <DialogTitle>{t('ai.restoreTitle', { n: restore?.v.version ?? '' })}</DialogTitle>
        <DialogContent>
          {restore && restore.p.diff.length === 0
            ? <Typography>{t('ai.restoreNothing')}</Typography>
            : (
              <Table size="small">
                <TableHead><TableRow><TableCell>{t('ai.colSetting')}</TableCell><TableCell>{t('ai.colNow')}</TableCell><TableCell>{t('ai.colBecomes')}</TableCell></TableRow></TableHead>
                <TableBody>
                  {restore?.p.diff.map((d) => (
                    <TableRow key={d.path}>
                      <TableCell><Box component="code" sx={{ wordBreak: 'break-all' }}>{d.path}</Box></TableCell>
                      <TableCell sx={{ wordBreak: 'break-word' }}>{val(d.from)}</TableCell>
                      <TableCell sx={{ wordBreak: 'break-word' }}>{val(d.to)}</TableCell>
                    </TableRow>
                  ))}
                </TableBody>
              </Table>
            )}
          {restore?.p.warnings.map((w) => <Alert key={w} severity="warning" sx={{ mt: 1 }}>{w}</Alert>)}
        </DialogContent>
        <DialogActions>
          <Button onClick={() => setRestore(null)} disabled={busy}>{t('common.cancel')}</Button>
          <Button variant="contained" onClick={apply} disabled={busy || restore?.p.diff.length === 0}>{t('ai.restoreDo')}</Button>
        </DialogActions>
      </Dialog>
    </Section>
  );
};

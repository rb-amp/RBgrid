/**
 * AiPage — the hub's «AI assistants» page (fw 1.3.0; root ai-llm-page-contract + ai-connect-ux,
 * protocol v1 §2/§6, CONNECT-UX §2.2 a): pairing codes on top (with Deny), access (on, read-only),
 * how to connect a client (ConnectCard), the paired assistants (calls today, Revoke), the changes
 * assistants made (versions + Restore), the hub's events and the call log.
 * The hub has no push for pairing: the page polls GET /api/v1/llm every 2.5 s while open and visible.
 * Versions and events are tool calls the page makes itself (POST /api/v1/llm/call, caller `web`).
 * The code is the secret the user reads to the assistant: shown big, never logged.
 * Client names are user text — rendered as text only (React escapes them).
 */
import { PageSkeleton } from '../components/common/PageSkeleton';
import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  Alert, Box, Button, Chip, FormControlLabel, Stack, Switch, Table, TableBody, TableCell,
  TableHead, TableRow, Typography,
} from '@mui/material';
import { Link as RouterLink } from 'react-router-dom';
import { useDeviceSession } from '../core';
import { useDeviceClock } from '../core/deviceTime';
import { llmApi, LlmDoc, LlmError, LlmEvent, LlmVersion, LLM_ERROR_CODES } from '../core/llm/api';
import { getBaseUrl } from '../services/connection';
import { useConfirm } from '../components/common/ConfirmDialog';
import { Section, Scopes, when } from '../components/ai/aiShared';
import { PairingCard } from '../components/ai/PairingCard';
import { ConnectCard } from '../components/ai/ConnectCard';
import { VersionsCard } from '../components/ai/VersionsCard';
import { EventsCard } from '../components/ai/EventsCard';
import { t } from '../i18n';

const POLL_MS = 2500;

const knownCode = (r: string): boolean => (LLM_ERROR_CODES as readonly string[]).includes(r);

/** Calls today per client from the log the hub sends (its newest 50). The log cuts client names
 *  (seen: 23 bytes), so a token matches by prefix. «n+» when the log is cut and still within today. */
export function callsToday(doc: LlmDoc, name: string, dayStart: number): string {
  const mine = (c: string) => !!c && (c === name || name.startsWith(c));
  const n = doc.log.filter((l) => l.ts >= dayStart && mine(l.client)).length;
  const oldest = doc.log.length ? Math.min(...doc.log.map((l) => l.ts)) : 0;
  return doc.log_total > doc.log.length && oldest >= dayStart && n > 0 ? `${n}+` : String(n);
}

export const AiPage: React.FC = () => {
  const session = useDeviceSession();
  const clock = useDeviceClock();
  const confirm = useConfirm();
  const direct = session.transport.kind === 'direct';
  const [doc, setDoc] = useState<LlmDoc | null>(null);
  const [err, setErr] = useState<string | null>(null);
  const [unsupported, setUnsupported] = useState(false);
  const [needToken, setNeedToken] = useState(false);
  const [busy, setBusy] = useState(false);
  const [now, setNow] = useState(() => Date.now());
  // code → deadline (ms), fixed when the code first appears, so polling doesn't jitter the countdown.
  const deadlines = useRef(new Map<string, number>());
  const [versions, setVersions] = useState<LlmVersion[] | null>(null);
  const [versionsErr, setVersionsErr] = useState<string | null>(null);
  const [events, setEvents] = useState<LlmEvent[] | null>(null);
  const [eventsErr, setEventsErr] = useState<string | null>(null);

  const apply = useCallback((d: LlmDoc) => {
    const at = Date.now();
    const m = deadlines.current;
    const live = new Set(d.pairing.map((p) => p.code));
    for (const k of Array.from(m.keys())) if (!live.has(k)) m.delete(k);
    for (const p of d.pairing) if (!m.has(p.code)) m.set(p.code, at + p.expires_in * 1000);
    setDoc(d); setErr(null);
  }, []);

  const load = useCallback(async () => {
    try { apply(await llmApi.get()); } catch (e) {
      const s = (e as LlmError).status;
      if (s === 404 || s === 405) setUnsupported(true); else setErr((e as Error).message);
    }
  }, [apply]);

  useEffect(() => {
    if (!direct) return undefined;
    load();
    const id = setInterval(() => { if (typeof document === 'undefined' || document.visibilityState !== 'hidden') load(); }, POLL_MS);
    return () => clearInterval(id);
  }, [direct, load]);

  // Versions and events: on open, whenever the call log moves (an assistant may have changed something) and after a Restore.
  const loadHistory = useCallback(() => {
    llmApi.versions().then((v) => { setVersions(v); setVersionsErr(null); }).catch((e) => setVersionsErr((e as Error).message));
    llmApi.recentEvents(20).then((v) => { setEvents(v); setEventsErr(null); }).catch((e) => setEventsErr((e as Error).message));
  }, []);
  const logTotal = doc?.log_total;
  const loaded = !!doc;
  useEffect(() => { if (direct && loaded) loadHistory(); }, [direct, loaded, logTotal, loadHistory]);

  // The countdown ticks only while a code is pending.
  const pending = (doc?.pairing.length ?? 0) > 0;
  useEffect(() => {
    if (!pending) return undefined;
    const id = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(id);
  }, [pending]);

  const set = async (patch: { enabled?: boolean; read_only?: boolean; revoke?: string; pair_cancel?: string }) => {
    setBusy(true);
    try { apply(await llmApi.set(patch)); setNeedToken(false); } catch (e) {
      if ((e as LlmError).status === 409 && patch.enabled) setNeedToken(true); else setErr((e as Error).message);
    } finally { setBusy(false); }
  };

  const revoke = async (id: string, name: string) => {
    if (await confirm({ title: t('ai.revokeTitle'), body: t('ai.revokeBody', { name: name || t('ai.unnamed') }), confirmLabel: t('ai.revoke'), danger: true })) {
      await set({ revoke: id });
    }
  };

  if (!direct) return <Alert severity="info">{t('ai.directOnly')}</Alert>;
  if (unsupported) return <Alert severity="info">{t('ai.notOnThisHub')}</Alert>;
  if (err && !doc) return <Alert severity="error">{t('ai.loadFailed', { why: err })}</Alert>;
  if (!doc) return <PageSkeleton />;

  return (
    <Box>
      <Typography variant="h4" fontWeight={800} sx={{ mb: 0.5 }}>{t('ai.title')}</Typography>
      <Typography color="text.secondary" sx={{ mb: 2 }}>{t('ai.lead')}</Typography>
      {err && <Alert severity="warning" sx={{ mb: 2 }}>{t('ai.loadFailed', { why: err })}</Alert>}

      {doc.pairing.length > 0 && (
        <Section title={t('ai.pairTitle')} id="pairing">
          <Typography variant="body2" sx={{ mb: 1 }}>{t('ai.pairHow')}</Typography>
          {doc.pairing.map((p) => (
            <PairingCard key={p.code} p={p} deadline={deadlines.current.get(p.code) ?? now} now={now} busy={busy}
              onDeny={() => set({ pair_cancel: p.pair_id })} />
          ))}
        </Section>
      )}

      <Section title={t('ai.accessTitle')}>
        <Stack spacing={1}>
          <FormControlLabel
            control={<Switch checked={doc.enabled} disabled={busy} onChange={(e) => set({ enabled: e.target.checked })} />}
            label={t('ai.enabled')} />
          <Alert severity={doc.enabled ? 'warning' : 'info'}>{t('ai.enabledWarn')}</Alert>
          {needToken && (
            <Alert severity="error" action={<Button color="inherit" size="small" component={RouterLink} to="/config#security">{t('ai.toSecurity')}</Button>}>
              {t('ai.needAdminToken')}
            </Alert>
          )}
          <FormControlLabel
            control={<Switch checked={doc.read_only} disabled={busy} onChange={(e) => set({ read_only: e.target.checked })} />}
            label={t('ai.readOnly')} />
          <Typography variant="body2" color="text.secondary">{t('ai.readOnlyHint')}</Typography>
        </Stack>
      </Section>

      {doc.enabled
        ? <ConnectCard connect={doc.connect} origin={getBaseUrl() || window.location.origin} />
        : <Alert severity="info" sx={{ mb: 2 }}>{t('ai.connectWhenOn')}</Alert>}
      {doc.enabled && doc.pairing.length === 0 && (
        <Typography variant="body2" color="text.secondary" sx={{ mb: 2 }}>{t('ai.pairNone')}</Typography>
      )}

      <Section title={t('ai.tokensTitle')}>
        {doc.tokens.length === 0 ? <Typography color="text.secondary">{t('ai.tokensNone')}</Typography> : (
          <Box sx={{ overflowX: 'auto' }}>
            <Table size="small">
              <TableHead><TableRow>
                <TableCell>{t('ai.colName')}</TableCell><TableCell>{t('ai.colScopes')}</TableCell>
                <TableCell>{t('ai.colCreated')}</TableCell><TableCell>{t('ai.colLastUsed')}</TableCell>
                <TableCell align="right">{t('ai.colToday')}</TableCell><TableCell />
              </TableRow></TableHead>
              <TableBody>
                {doc.tokens.map((x) => (
                  <TableRow key={x.id}>
                    <TableCell sx={{ wordBreak: 'break-word' }}>{x.client_name || t('ai.unnamed')}</TableCell>
                    <TableCell><Scopes scopes={x.scopes} /></TableCell>
                    <TableCell sx={{ whiteSpace: 'nowrap' }}>{when(clock, x.created)}</TableCell>
                    <TableCell sx={{ whiteSpace: 'nowrap' }}>{x.last_used ? when(clock, x.last_used) : t('ai.never')}</TableCell>
                    <TableCell align="right">{callsToday(doc, x.client_name, clock.dayStart(Math.floor(now / 1000)))}</TableCell>
                    <TableCell align="right">
                      <Button size="small" color="error" disabled={busy} onClick={() => revoke(x.id, x.client_name)}>{t('ai.revoke')}</Button>
                    </TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Box>
        )}
      </Section>

      <VersionsCard versions={versions} error={versionsErr} clock={clock} onChanged={loadHistory} />
      <EventsCard events={events} error={eventsErr} clock={clock} />

      <Section title={t('ai.logTitle')}
        right={doc.log_total > doc.log.length ? <Typography variant="caption" color="text.secondary">{t('ai.logShown', { n: doc.log.length, total: doc.log_total })}</Typography> : undefined}>
        {doc.log.length === 0 ? <Typography color="text.secondary">{t('ai.logNone')}</Typography> : (
          <Box sx={{ overflowX: 'auto' }}>
            <Table size="small">
              <TableHead><TableRow>
                <TableCell>{t('ai.colTime')}</TableCell><TableCell>{t('ai.colClient')}</TableCell><TableCell>{t('ai.colTool')}</TableCell>
                <TableCell>{t('ai.colResult')}</TableCell><TableCell align="right">{t('ai.colMs')}</TableCell>
              </TableRow></TableHead>
              <TableBody>
                {doc.log.map((l, i) => (
                  <TableRow key={`${l.ts}-${i}`} sx={l.config_changed ? { bgcolor: 'action.selected' } : undefined}>
                    <TableCell sx={{ whiteSpace: 'nowrap' }}>{when(clock, l.ts, true)}</TableCell>
                    <TableCell sx={{ wordBreak: 'break-word' }}>{l.client === 'web' ? t('ai.src_web') : (l.client || t('ai.unnamed'))}</TableCell>
                    <TableCell><Box component="code">{l.tool}</Box>
                      {l.config_changed && <Chip size="small" color="primary" label={t('ai.configChanged')} sx={{ ml: 1 }} />}
                    </TableCell>
                    <TableCell>
                      {l.result === 'ok'
                        ? <Typography variant="body2" color="success.main">{t('ai.ok')}</Typography>
                        : <Typography variant="body2" color="error.main">{knownCode(l.result) && <>{t(`ai.err_${l.result}`)} </>}<Box component="code" sx={{ opacity: knownCode(l.result) ? 0.7 : 1 }}>{l.result}</Box></Typography>}
                    </TableCell>
                    <TableCell align="right">{l.ms}</TableCell>
                  </TableRow>
                ))}
              </TableBody>
            </Table>
          </Box>
        )}
      </Section>
    </Box>
  );
};

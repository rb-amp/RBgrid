import React from 'react';
import { render, screen, fireEvent } from '@testing-library/react';
import { parseDynPrice, parseRules, parsePriceLog, parseSignal, DynPriceError } from '../../core/dynprice/api';
import { makeDeviceClock } from '../../core/deviceTime';
import { liveRate } from '../../economics/costRate';
import { dynPricePatch, zonesFor, DynPriceForm } from './DynPriceForm';
import { ruleProblem, nextRuleId, PriceRulesPanel } from './PriceRulesPanel';
import { ConfirmProvider } from '../common/ConfirmDialog';
import { buildPriceLogCsv, provText } from './PriceLogCard';
import { priceTicks } from './PriceChart';
import { priceStateSev, shownProviders, PriceStatus } from './PriceStatus';
import dynprice from '../../core/dynprice/fixtures/dynprice.json';
import rules from '../../core/dynprice/fixtures/rules.json';
import signal from '../../core/dynprice/fixtures/signal.json';

const doc = parseDynPrice(dynprice);
const utc = makeDeviceClock('UTC0');

describe('dynamic prices UI rules (fw 1.2.0, root dynprice-webapp)', () => {
  it('sends only what changed; a token only when typed', () => {
    const d = { ...doc.settings, token: '', backup_token: '' };
    expect(dynPricePatch(doc.settings, d)).toEqual({});
    expect(dynPricePatch(doc.settings, { ...d, markup: 0.02, floor_buy: 0 })).toEqual({ markup: 0.02, floor_buy: 0 });
    expect(dynPricePatch(doc.settings, { ...d, token: 'x' })).toEqual({ token: 'x' });
  });

  it('zones: the network provider\'s; with two, only those both serve; none for HA alone', () => {
    expect(zonesFor(doc.providers, 'energy_charts', 'ha').map((z) => z.id)).toContain('NL');
    const both = zonesFor(doc.providers, 'nordpool', 'elprisetjustnu').map((z) => z.id);
    expect(both).toEqual(['SE1', 'SE2', 'SE3', 'SE4']);
    expect(zonesFor(doc.providers, 'ha', 'none')).toEqual([]);
  });

  it('a zone in another currency than the plan is said, and blocks saving', () => {
    const se = { ...doc, settings: { ...doc.settings, source: 'nordpool', backup_source: 'none', zone: 'SE3' } };
    render(<DynPriceForm doc={se} canWrite onSave={jest.fn()} />);
    expect(screen.getByText(/priced in SEK, the plan in EUR|в SEK, а план в EUR/)).toBeTruthy();
  });

  it('a hub refusal names the field', async () => {
    const onSave = jest.fn().mockRejectedValue(new DynPriceError('zone currency differs', 400, 'zone'));
    render(<DynPriceForm doc={doc} canWrite onSave={onSave} />);
    fireEvent.click(screen.getByLabelText(/Bill at the hourly mean|Считать по среднему за час/));
    fireEvent.click(screen.getByRole('button', { name: /^(Save|Сохранить)$/ }));
    expect(await screen.findByText(/refused «Price zone»|не принял «Ценовая зона»/)).toBeTruthy();
    expect(onSave).toHaveBeenCalledWith({ hourly_mean: true });
  });

  it('rules: name, levels and periods are checked before the hub sees them', () => {
    const r = parseRules(rules).rules;
    expect(r.map(ruleProblem)).toEqual([null, null, null]);
    expect(ruleProblem({ ...r[1], off_above: 0.1 })).toMatch(/off|выключения/);
    expect(ruleProblem({ ...r[0], periods: 97 })).toMatch(/96/);
    expect(ruleProblem({ ...r[0], name: ' ' })).toBeTruthy();
    expect(nextRuleId(r)).toBe(4);
    expect(nextRuleId([{ ...r[0], id: 2 }])).toBe(1);
  });

  it('price log CSV: hub time, numbers as numbers, provenance in words', () => {
    const csv = buildPriceLogCsv(parsePriceLog({ currency: 'EUR', cols: ['from', 'buy', 'sell', 'prov'], rows: [[1794273300, -0.0123, null, 'backup+hourly_mean']] }), utc);
    const lines = csv.replace(/^﻿/, '').trim().split('\r\n');
    expect(lines).toHaveLength(2);
    expect(lines[1]).toMatch(/^2026-11-10 01:15[;,]-0[.,]0123[;,][;,]/);
    expect(provText('backup+hourly_mean')).toMatch(/backup|запасн/);
  });

  it('axis ticks cover negative prices and include zero', () => {
    const tk = priceTicks(-0.05, 0.71);
    expect(tk[0]).toBeLessThanOrEqual(-0.05);
    expect(tk[tk.length - 1]).toBeGreaterThanOrEqual(0.71);
    expect(tk).toContain(0);
  });

  it('a negative dynamic price is a price, not «no plan»', () => {
    const r = liveRate({ dynamic: true, price_state: 'ok', buy_rate: -0.02, buy_rate_all_in: -0.01, sell_rate: -0.03, currency: 'EUR' });
    expect(r).toMatchObject({ buy: -0.01, sell: -0.03, gross: true });
    expect(liveRate({ buy_rate: 0, sell_rate: 0 }).buy).toBeNull();
  });

  it('status: state in words, provider, attribution verbatim', () => {
    expect(priceStateSev('plan_fallback')).toBe('error');
    expect(shownProviders(doc).map((p) => p.id)).toEqual(['energy_charts', 'ha']);
    render(<PriceStatus doc={doc} signal={parseSignal(signal)} clock={utc} fmtRate={(v) => v.toFixed(4)} />);
    expect(screen.getByText(/Price from the primary provider|Цена от основного поставщика/)).toBeTruthy();
    expect(screen.getByText('Bundesnetzagentur | SMARD.de / Energy-Charts (CC BY 4.0)').closest('a')?.getAttribute('href')).toBe('https://api.energy-charts.info/');
  });

  it('a re-read with the same settings keeps the user\'s edits (price_gen moves, rules poll)', () => {
    const { rerender } = render(<DynPriceForm doc={doc} canWrite onSave={jest.fn()} />);
    fireEvent.click(screen.getByLabelText(/Bill at the hourly mean|Считать по среднему за час/));
    rerender(<DynPriceForm doc={parseDynPrice(dynprice)} canWrite onSave={jest.fn()} />);
    expect((screen.getByLabelText(/Bill at the hourly mean|Считать по среднему за час/) as HTMLInputElement).checked).toBe(true);
    const rd = parseRules(rules);
    const view = (d: typeof rd) => <ConfirmProvider><PriceRulesPanel doc={d} clock={utc} fmtRate={String} canWrite onSaveRules={jest.fn()} onSaveSignal={jest.fn()} /></ConfirmProvider>;
    const r2 = render(view(rd));
    fireEvent.click(r2.getAllByLabelText(/«Cheap now» below|«Сейчас дёшево» ниже/)[0]);
    r2.rerender(view(parseRules(rules)));
    expect((r2.getAllByLabelText(/«Cheap now» below|«Сейчас дёшево» ниже/)[0] as HTMLInputElement).checked).toBe(false);
  });

  it('a rule name is limited in bytes, as the hub stores it', () => {
    const r = parseRules(rules).rules[0];
    expect(ruleProblem({ ...r, name: 'Бойлер ночью' })).toBeNull(); // 23 bytes
    expect(ruleProblem({ ...r, name: 'Бойлер ночью!' })).toMatch(/23/);
  });
});

/**
 * PageSkeleton — the page's outline while its data loads (UI redesign S8, SPEC §6.7), instead of a
 * spinner in the middle of an empty page: the blocks appear where they will be, so nothing jumps.
 * `dashboard`: a title, two half blocks and a whole-row one; `cards`: a grid of tiles; `form`: a
 * title and a panel of fields. Announced as one «Loading» progress for screen readers.
 */
import React from 'react';
import { Box, Grid, Skeleton, Stack, Typography } from '@mui/material';
import { t } from '../../i18n';

const Block: React.FC<{ h: number }> = ({ h }) => (
  <Skeleton variant="rounded" height={h} sx={{ borderRadius: 3 }} />
);

export const PageSkeleton: React.FC<{ kind?: 'dashboard' | 'cards' | 'form'; message?: string }> = ({ kind = 'form', message }) => (
  <Box role="progressbar" aria-label={t('common.loading')} aria-busy>
    <Skeleton variant="text" width={200} sx={{ fontSize: 22, mb: 2 }} />
    {message && <Typography color="text.secondary" sx={{ mb: 2 }}>{message}</Typography>}
    {kind === 'dashboard' && (
      <Grid container spacing={2}>
        <Grid item xs={12} md={6}><Block h={190} /></Grid>
        <Grid item xs={12} md={6}><Block h={190} /></Grid>
        <Grid item xs={12}><Block h={320} /></Grid>
      </Grid>
    )}
    {kind === 'cards' && (
      <Grid container spacing={2}>
        {[0, 1, 2, 3, 4, 5].map((i) => <Grid item xs={12} sm={6} md={4} key={i}><Block h={140} /></Grid>)}
      </Grid>
    )}
    {kind === 'form' && (
      <Stack spacing={2}>
        <Block h={180} />
        <Block h={260} />
      </Stack>
    )}
  </Box>
);

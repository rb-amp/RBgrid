import React from 'react';
import { Card as MuiCard, CardContent } from '@mui/material';
import { palettes } from '../tokens';

/** Card weight (UI redesign S1): the page's main answer · an ordinary block · reference text. */
export type CardTone = 'hero' | 'default' | 'quiet';

export interface CardProps {
  /** Card contents. */
  children?: React.ReactNode;
  /** Dim the card to signal an inactive / offline / dormant entity. */
  muted?: boolean;
  /** Outlined (no elevation) variant — for secondary / nested cards. */
  outlined?: boolean;
  /**
   * `hero` — one per screen, the main figure (raised surface, roomier);
   * `quiet` — reference text: no frame, the page background tone;
   * `default` — everything else.
   */
  tone?: CardTone;
}

/** The sx of a card of this tone (shared by Card and PanelCard). */
export const toneSx = (tone: CardTone) => (tone === 'hero'
  // A raised surface and a firmer frame — a shadow alone vanished in the dark theme (UI/UX audit U13).
  ? { bgcolor: (t: { palette: { mode: string } }) => (t.palette.mode === 'dark' ? palettes.dark.surfaceRaised : palettes.light.surface), boxShadow: (t: { shadows: string[] }) => t.shadows[4], border: 1, borderColor: (t: { palette: { mode: string } }) => palettes[t.palette.mode === 'dark' ? 'dark' : 'light'].borderStrong }
  : tone === 'quiet'
    ? { bgcolor: 'action.hover', boxShadow: 'none', border: 'none', backgroundImage: 'none' }
    : {});

/**
 * Base surface for RBgrid — the HA-style "ha-card": 12px radius, soft border,
 * comfortable padding. Every panel/card composes from this.
 *
 * @category Surfaces
 */
export const Card: React.FC<CardProps> = ({ children, muted = false, outlined = false, tone = 'default' }) => (
  <MuiCard
    variant={outlined && tone === 'default' ? 'outlined' : 'elevation'}
    sx={{ height: '100%', borderRadius: 3, opacity: muted ? 0.5 : 1, ...toneSx(tone) }}
  >
    <CardContent sx={tone === 'hero' ? { p: { xs: 2, md: 3 }, '&:last-child': { pb: { xs: 2, md: 3 } } } : undefined}>{children}</CardContent>
  </MuiCard>
);

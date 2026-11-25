import React, { createContext, useContext } from 'react';
import { Box, Card as MuiCard, CardContent } from '@mui/material';

/** Inside a PanelGroup, PanelCards draw as sections of one card (no card of their own). */
export const PanelGroupContext = createContext(false);
export const useInPanelGroup = (): boolean => useContext(PanelGroupContext);

export interface PanelGroupProps {
  children?: React.ReactNode;
}

/**
 * One card for a settings section (UI redesign 22.11): its panels follow one another as sections
 * with a divider between them, in one column — no uneven two-column grid of cards.
 *
 * @category Settings
 */
export const PanelGroup: React.FC<PanelGroupProps> = ({ children }) => (
  <MuiCard sx={{ borderRadius: 3 }}>
    <CardContent sx={{ p: { xs: 2, md: 3 }, '&:last-child': { pb: { xs: 2, md: 3 } } }}>
      <PanelGroupContext.Provider value>
        {/* Dividers by CSS: a panel that renders nothing leaves no empty gap or double line. */}
        <Box sx={{
          '& > :empty': { display: 'none' },
          '& > :not(:empty) ~ :not(:empty)': { borderTop: 1, borderColor: 'divider', pt: 3, mt: 3 },
        }}>
          {children}
        </Box>
      </PanelGroupContext.Provider>
    </CardContent>
  </MuiCard>
);

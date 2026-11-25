import React from 'react';
import { Box, Card as MuiCard, CardContent, Button, Alert } from '@mui/material';
import { useInPanelGroup } from './PanelGroup';
import { BlockHeader } from './BlockHeader';
import { toneSx, CardTone } from './Card';

export interface PanelCardProps {
  /** Panel heading (e.g. "Status", "Time / NTP", "Telemetry"). */
  title: string;
  /** Optional action button label (e.g. "Scan", "Sync now"). */
  actionLabel?: string;
  /** Action button icon (optional ReactNode). */
  actionIcon?: React.ReactNode;
  /** Disable the action button. */
  actionDisabled?: boolean;
  /** Action click handler. */
  onAction?: () => void;
  /** Optional inline alert above the body. */
  alert?: string;
  /** Alert severity. */
  alertSeverity?: 'success' | 'info' | 'warning' | 'error';
  /** Panel body. */
  children?: React.ReactNode;
  /** A quiet caption under the title. */
  caption?: React.ReactNode;
  /** Card weight (see Card). */
  tone?: CardTone;
}

/**
 * The settings-panel shell — a titled ha-card with an optional header action
 * button and an optional inline alert. Every settings panel composes from this.
 *
 * @category Settings
 */
export const PanelCard: React.FC<PanelCardProps> = ({
  title,
  actionLabel,
  actionIcon,
  actionDisabled = false,
  onAction,
  alert,
  alertSeverity = 'info',
  children,
  caption,
  tone = 'default',
}) => {
  const inGroup = useInPanelGroup();
  const inner = (
    <>
      <BlockHeader title={title} caption={caption} component="h2" actions={actionLabel ? (
        <Button startIcon={actionIcon} onClick={onAction} disabled={actionDisabled}>{actionLabel}</Button>
      ) : undefined} />
      {alert && (
        <Alert severity={alertSeverity} sx={{ mb: 2 }}>
          {alert}
        </Alert>
      )}
      {children}
    </>
  );
  // A section of a PanelGroup's card, or a card of its own.
  if (inGroup) return <Box component="section">{inner}</Box>;
  return (
    <MuiCard sx={{ borderRadius: 3, ...toneSx(tone) }}>
      <CardContent>{inner}</CardContent>
    </MuiCard>
  );
};

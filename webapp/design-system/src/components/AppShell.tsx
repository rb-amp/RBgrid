import React from 'react';
import { Badge, Box, ButtonBase, Drawer, IconButton, Stack, SvgIcon, Tooltip, Typography, useMediaQuery } from '@mui/material';
import { useTheme } from '@mui/material/styles';
import { palettes } from '../tokens';

export interface NavItem {
  /** Item label. */
  label: string;
  /** Optional short label for the mobile bottom tab bar (defaults to `label`). */
  shortLabel?: string;
  /** Optional leading icon (ReactNode). */
  icon?: React.ReactNode;
  /** Whether this item is the active route. */
  active?: boolean;
  /** Click handler. */
  onClick?: () => void;
  /** Optional section header this item belongs to (consecutive same-group items render under one header). */
  group?: string;
  /**
   * Show this item as a slot in the mobile bottom tab bar (< md). Up to 4 tab items are
   * shown; the 5th slot is always «Ещё», which lists the remaining items + the footer.
   */
  tab?: boolean;
  /** Optional position of a `tab` item in the bottom bar (ascending; default = nav order). */
  tabOrder?: number;
  /** A count beside the label (e.g. active events); 0 / undefined = none. */
  badge?: number;
}

export interface AppShellProps {
  /** Brand / product name shown at the top of the rail. */
  brand?: string;
  /** Optional brand glyph rendered before the brand name. */
  brandIcon?: React.ReactNode;
  /** Sidebar navigation items. */
  nav: NavItem[];
  /** Optional right-aligned header content (status, actions). Header bar hidden when omitted. */
  headerRight?: React.ReactNode;
  /**
   * Optional content pinned to the bottom of the nav rail (e.g. device/connection footer,
   * theme switch). On mobile it is rendered at the bottom of the «Ещё» drawer.
   */
  footer?: React.ReactNode;
  /**
   * Optional content right under the brand at the top of the rail (e.g. the demo-site picker —
   * at the bottom it went unseen). On mobile it opens the «Ещё» drawer, above the sections.
   */
  railTop?: React.ReactNode;
  /** Label of the overflow tab on mobile. Default «Ещё». */
  moreLabel?: string;
  /** Accessible name of the main navigation. Default «Разделы» (pass a translated one). */
  navLabel?: string;
  /** Accessible name of the overflow navigation in the «Ещё» drawer. Default «Другие разделы». */
  moreNavLabel?: string;
  /** Main content. */
  children?: React.ReactNode;
  /** The rail shows icons only (64 px) — more room for charts on a laptop. */
  collapsed?: boolean;
  /** Toggles `collapsed`; the toggle button is shown only when this is given. */
  onToggleCollapsed?: () => void;
  /** Labels of the toggle (translated). */
  collapseLabel?: string;
  expandLabel?: string;
  /** The footer while collapsed (default: none). */
  footerCollapsed?: React.ReactNode;
}

const RAIL_WIDTH = 220;
const RAIL_COLLAPSED = 64;
const TABBAR_HEIGHT = 56;
const MAX_TABS = 4;

/**
 * Visible keyboard focus for the shell's own buttons (spec §9: 2 px `focus` token). Inset
 * (offset −2) so the ring isn't clipped by the rail's overflow / the viewport edge.
 */
const focusRing = (mode: 'light' | 'dark') =>
  ({
    '&.Mui-focusVisible, &:focus-visible': {
      outline: `2px solid ${palettes[mode].focus}`,
      outlineOffset: '-2px',
    },
  }) as const;

const MoreIcon: React.FC = () => (
  <SvgIcon>
    <path d="M6 10a2 2 0 1 0 0 4 2 2 0 0 0 0-4zm6 0a2 2 0 1 0 0 4 2 2 0 0 0 0-4zm6 0a2 2 0 1 0 0 4 2 2 0 0 0 0-4z" />
  </SvgIcon>
);

/** The grouped vertical nav list (desktop rail + mobile «Ещё» drawer). */
const NavList: React.FC<{ items: NavItem[]; onNavigate?: () => void; collapsed?: boolean }> = ({ items, onNavigate, collapsed = false }) => {
  const mode = useTheme().palette.mode;
  return (
  <Stack component="ul" spacing={0.5} sx={{ listStyle: 'none', m: 0, px: collapsed ? 1 : 1.5, py: 0 }}>
    {items.map((item, i) => {
      const showGroupHeader = !!item.group && item.group !== items[i - 1]?.group;
      return (
        <React.Fragment key={item.label}>
          {showGroupHeader && collapsed && i > 0 && <Box component="li" aria-hidden sx={{ borderTop: 1, borderColor: 'divider', mx: 1, my: 1 }} />}
          {showGroupHeader && !collapsed && (
            <Typography
              component="li"
              variant="overline"
              sx={{ px: 1.5, pt: i === 0 ? 0 : 1.5, color: 'text.secondary', lineHeight: 2 }}
            >
              {item.group}
            </Typography>
          )}
          <li>
            <Tooltip title={collapsed ? item.label : ''} placement="right" disableInteractive>
            <ButtonBase
              onClick={() => {
                item.onClick?.();
                onNavigate?.();
              }}
              aria-current={item.active ? 'page' : undefined}
              aria-label={collapsed ? (item.badge ? `${item.label} (${item.badge})` : item.label) : undefined}
              sx={{
                width: '100%',
                justifyContent: collapsed ? 'center' : 'flex-start',
                gap: 1.5,
                px: 1.5,
                py: 1,
                minHeight: 40,
                borderRadius: 2,
                textAlign: 'left',
                color: item.active ? 'primary.main' : 'text.secondary',
                bgcolor: item.active ? 'action.selected' : 'transparent',
                fontWeight: item.active ? 700 : 600,
                '& svg': { color: 'inherit', fontSize: 22 },
                '&:hover': { bgcolor: item.active ? 'action.selected' : 'action.hover', color: item.active ? 'primary.main' : 'text.primary' },
                ...focusRing(mode),
              }}
            >
              {collapsed
                ? <Badge color="error" variant="dot" invisible={!item.badge}>{item.icon}</Badge>
                : item.icon}
              {!collapsed && (
                <Typography component="span" variant="body2" fontWeight="inherit" noWrap sx={{ flex: 1, minWidth: 0 }}>
                  {item.label}
                </Typography>
              )}
              {!collapsed && !!item.badge && (
                <Box component="span" sx={{ minWidth: 20, height: 20, px: 0.75, borderRadius: 10, bgcolor: 'error.main', color: 'error.contrastText', fontSize: 12, fontWeight: 700, lineHeight: '20px', textAlign: 'center' }}>
                  {item.badge}
                </Box>
              )}
            </ButtonBase>
            </Tooltip>
          </li>
        </React.Fragment>
      );
    })}
  </Stack>
  );
};

const Brand: React.FC<{ brand: string; brandIcon?: React.ReactNode; compact?: boolean }> = ({ brand, brandIcon, compact }) => (
  <Stack direction="row" alignItems="center" spacing={1.5} sx={{ px: compact ? 2 : 2.5, py: compact ? 1 : 2, minWidth: 0 }}>
    {brandIcon}
    <Typography variant="h6" component="span" fontWeight={800} noWrap letterSpacing={0.5} sx={{ color: 'primary.main' }}>
      {brand}
    </Typography>
  </Stack>
);

/**
 * The HA-style application shell. Theme-aware (follows the active MUI light/dark palette).
 *
 * - ≥ md: a sticky brand + nav rail (220 px) on the left, content on the right.
 * - < md: no rail; a slim top bar with the brand and a fixed bottom tab bar
 *   (items flagged `tab` + «Ещё», which opens a bottom drawer with the rest of the nav
 *   and the footer).
 *
 * Content is centred with `maxWidth: 1440`.
 *
 * @category Layout
 */
export const AppShell: React.FC<AppShellProps> = ({
  brand = 'RBgrid',
  brandIcon,
  nav,
  headerRight,
  footer,
  railTop,
  moreLabel = 'Ещё',
  navLabel = 'Разделы',
  moreNavLabel = 'Другие разделы',
  children,
  collapsed = false,
  onToggleCollapsed,
  collapseLabel = 'Свернуть меню',
  expandLabel = 'Развернуть меню',
  footerCollapsed,
}) => {
  const theme = useTheme();
  const mobile = useMediaQuery(theme.breakpoints.down('md'));
  const [moreOpen, setMoreOpen] = React.useState(false);

  React.useEffect(() => {
    if (!mobile) setMoreOpen(false);
  }, [mobile]);

  const tabs = nav
    .map((n, i) => ({ n, i }))
    .filter(({ n }) => n.tab)
    .sort((a, b) => (a.n.tabOrder ?? a.i) - (b.n.tabOrder ?? b.i))
    .map(({ n }) => n)
    .slice(0, MAX_TABS);
  const rest = nav.filter((n) => !tabs.includes(n));
  const moreActive = rest.some((n) => n.active);

  const main = (
    <Box
      component="main"
      sx={{
        flexGrow: 1,
        minWidth: 0,
        width: '100%',
        maxWidth: 1440,
        mx: 'auto',
        px: { xs: 2, lg: 3 },
        pt: { xs: 2, lg: 3 },
        pb: mobile ? `calc(${TABBAR_HEIGHT + 32}px + env(safe-area-inset-bottom, 0px))` : 3,
      }}
    >
      {children}
    </Box>
  );

  if (mobile) {
    return (
      <Box sx={{ minHeight: '100vh', bgcolor: 'background.default', color: 'text.primary', display: 'flex', flexDirection: 'column', minWidth: 0 }}>
        <Box
          component="header"
          sx={{
            position: 'sticky',
            top: 0,
            zIndex: theme.zIndex.appBar,
            display: 'flex',
            alignItems: 'center',
            minHeight: 48,
            bgcolor: 'background.paper',
            borderBottom: 1,
            borderColor: 'divider',
          }}
        >
          <Brand brand={brand} brandIcon={brandIcon} compact />
          {headerRight && <Box sx={{ ml: 'auto', pr: 2, minWidth: 0 }}>{headerRight}</Box>}
        </Box>

        {main}

        <Box
          component="nav"
          aria-label={navLabel}
          sx={{
            position: 'fixed',
            left: 0,
            right: 0,
            bottom: 0,
            zIndex: theme.zIndex.appBar,
            display: 'grid',
            gridTemplateColumns: 'repeat(5, minmax(0, 1fr))',
            height: `calc(${TABBAR_HEIGHT}px + env(safe-area-inset-bottom, 0px))`,
            pb: 'env(safe-area-inset-bottom, 0px)',
            bgcolor: 'background.paper',
            borderTop: 1,
            borderColor: 'divider',
          }}
        >
          {[
            ...tabs.map((t) => ({ ...t, more: false })),
            { label: moreLabel, icon: <MoreIcon />, active: moreActive || moreOpen, more: true },
          ].map((t: NavItem & { more: boolean }) => (
              <ButtonBase
                key={t.label}
                onClick={t.more ? () => setMoreOpen(true) : t.onClick}
                aria-current={!t.more && t.active ? 'page' : undefined}
                aria-haspopup={t.more ? 'dialog' : undefined}
                aria-expanded={t.more ? moreOpen : undefined}
                sx={{
                  flexDirection: 'column',
                  gap: '2px',
                  minWidth: 0,
                  height: TABBAR_HEIGHT,
                  px: 0.5,
                  color: t.active ? 'primary.main' : 'text.secondary',
                  '& svg': { fontSize: 22, color: 'inherit' },
                  ...focusRing(theme.palette.mode),
                }}
              >
                <Badge color="error" variant="dot" invisible={!t.badge}>{t.icon}</Badge>
                <Typography
                  component="span"
                  noWrap
                  sx={{ fontSize: 12, lineHeight: 1.2, fontWeight: t.active ? 700 : 500, maxWidth: '100%' }}
                >
                  {t.shortLabel ?? t.label}
                </Typography>
              </ButtonBase>
            ))}
        </Box>

        <Drawer
          anchor="bottom"
          open={moreOpen}
          onClose={() => setMoreOpen(false)}
          PaperProps={{
            sx: {
              borderTopLeftRadius: 12,
              borderTopRightRadius: 12,
              maxHeight: '85vh',
              pb: 'env(safe-area-inset-bottom, 0px)',
              bgcolor: 'background.paper',
              backgroundImage: 'none',
            },
          }}
        >
          <Box role="dialog" aria-label={moreLabel} sx={{ pt: 1 }}>
            <Box sx={{ width: 36, height: 4, borderRadius: 2, bgcolor: 'divider', mx: 'auto', mb: 1.5 }} />
            {railTop && <Box sx={{ px: 2, pb: 1.5 }}>{railTop}</Box>}
            {rest.length > 0 && (
              <Box component="nav" aria-label={moreNavLabel} sx={{ pb: 1 }}>
                <NavList items={rest} onNavigate={() => setMoreOpen(false)} />
              </Box>
            )}
            {footer && <Box>{footer}</Box>}
          </Box>
        </Drawer>
      </Box>
    );
  }

  return (
    <Box sx={{ display: 'flex', minHeight: '100vh', bgcolor: 'background.default', color: 'text.primary' }}>
      <Box
        component="aside"
        sx={{
          width: collapsed ? RAIL_COLLAPSED : RAIL_WIDTH,
          transition: 'width .15s',
          flexShrink: 0,
          position: 'sticky',
          top: 0,
          height: '100vh',
          // The rail itself doesn't scroll: the footer (device / connection) stays pinned at the
          // bottom and only the nav list scrolls on a short window (16.10: at 700 px the device
          // block sat below the edge, under «Устройство»).
          overflow: 'hidden',
          borderRight: 1,
          borderColor: 'divider',
          bgcolor: 'background.paper',
          display: 'flex',
          flexDirection: 'column',
        }}
      >
        {collapsed
          ? <Box sx={{ display: 'flex', justifyContent: 'center', py: 2 }}>{brandIcon}</Box>
          : <Brand brand={brand} brandIcon={brandIcon} />}
        {railTop && !collapsed && <Box sx={{ flexShrink: 0, px: 2, pb: 1.5 }}>{railTop}</Box>}
        <Box component="nav" aria-label={navLabel} sx={{ flex: '1 1 auto', minHeight: 0, overflowY: 'auto', overflowX: 'hidden' }}>
          <NavList items={nav} collapsed={collapsed} />
        </Box>
        {(collapsed ? footerCollapsed : footer) && <Box sx={{ flexShrink: 0, pt: 1, borderTop: 1, borderColor: 'divider' }}>{collapsed ? footerCollapsed : footer}</Box>}
        {onToggleCollapsed && (
          <Box sx={{ flexShrink: 0, display: 'flex', justifyContent: collapsed ? 'center' : 'flex-end', px: 1, py: 0.5, borderTop: 1, borderColor: 'divider' }}>
            <Tooltip title={collapsed ? expandLabel : collapseLabel} placement="right">
              <IconButton size="small" onClick={onToggleCollapsed} aria-label={collapsed ? expandLabel : collapseLabel} aria-expanded={!collapsed}>
                <SvgIcon fontSize="small">{collapsed ? <path d="M10 6 8.59 7.41 13.17 12l-4.58 4.59L10 18l6-6z" /> : <path d="M15.41 7.41 14 6l-6 6 6 6 1.41-1.41L10.83 12z" />}</SvgIcon>
              </IconButton>
            </Tooltip>
          </Box>
        )}
      </Box>
      <Box sx={{ flexGrow: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
        {headerRight && (
          <Stack direction="row" alignItems="center" sx={{ px: 3, py: 2, borderBottom: 1, borderColor: 'divider' }}>
            <Box sx={{ ml: 'auto' }}>{headerRight}</Box>
          </Stack>
        )}
        {main}
      </Box>
    </Box>
  );
};

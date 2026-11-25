/**
 * RBGrid webapp root: providers + routing + HA-style shell.
 * HashRouter (not BrowserRouter) — the app is served from the device (SPIFFS) with no
 * SPA route fallback, so client routing lives in the URL hash.
 */
import React from 'react';
import { HashRouter, Routes, Route, Navigate } from 'react-router-dom';
import { ThemeProvider } from './theme/ThemeProvider';
import { DeviceProvider } from './core';
import { DeviceLanguage } from './i18n/DeviceLanguage';
import { AppShell } from './app/AppShell';
import { ConnectScreen } from './pages/ConnectScreen';
import { useConnection } from './hooks/useConnection';
import { DashboardPage } from './pages/DashboardPage';
import { OnboardingPage } from './pages/OnboardingPage';
import { DevicesPage } from './pages/DevicesPage';
import { PointDetailPage } from './pages/PointDetailPage';
import { HistoryPage } from './pages/HistoryPage';
import { EventsPage } from './pages/EventsPage';
import { EconomicsPage } from './pages/EconomicsPage';
import { ConfigurationPage } from './pages/ConfigurationPage';
import { TariffsPricesPage } from './pages/TariffsPricesPage';
import { AiPage } from './pages/AiPage';
import { KioskPage } from './pages/KioskPage';
import { AppliancePage } from './pages/AppliancePage';
import { ConfirmProvider } from './components/common/ConfirmDialog';

/** Gate the app behind a device connection — first step is choosing a device or demo mode. */
const ConnectionGate: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const { connected } = useConnection();
  if (!connected) return <ConnectScreen />;
  return <>{children}</>;
};

const App: React.FC = () => (
  <ThemeProvider>
    <ConfirmProvider>
    <ConnectionGate>
      <DeviceProvider>
        <DeviceLanguage />
        <HashRouter>
          <Routes>
            {/* «Wall display»: a board on the whole screen, outside the shell (UI redesign §7). */}
            <Route path="/board/:id" element={<KioskPage />} />
            <Route path="/" element={<AppShell />}>
              <Route index element={<Navigate to="dashboard" replace />} />
              <Route path="dashboard" element={<DashboardPage />} />
              <Route path="onboarding" element={<OnboardingPage />} />
              {/* the old English «Setup» page is retired — the wizard is the one setup flow (PNT-23) */}
              <Route path="setup" element={<Navigate to="/onboarding" replace />} />
              <Route path="devices" element={<DevicesPage />} />
              <Route path="point/:unitKey" element={<PointDetailPage />} />
              <Route path="appliance/:groupId" element={<AppliancePage />} />
              <Route path="history" element={<HistoryPage />} />
              <Route path="events" element={<EventsPage />} />
              <Route path="economics" element={<EconomicsPage />} />
              <Route path="tariffs" element={<TariffsPricesPage />} />
              <Route path="tariffs/:tab" element={<TariffsPricesPage />} />
              <Route path="prices" element={<Navigate to="/tariffs/dynamic" replace />} />
              <Route path="ai" element={<AiPage />} />
              <Route path="config" element={<ConfigurationPage />} />
              <Route path="config/:section" element={<ConfigurationPage />} />
              <Route path="settings" element={<Navigate to="/config" replace />} />
              <Route path="*" element={<Navigate to="dashboard" replace />} />
            </Route>
          </Routes>
        </HashRouter>
      </DeviceProvider>
    </ConnectionGate>
    </ConfirmProvider>
  </ThemeProvider>
);

export default App;

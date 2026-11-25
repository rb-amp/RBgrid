# Changelog — RBGrid webapp

All notable changes to the RBGrid web interface. Newest first.

## 1.3.0

For hub firmware 1.3.0. Requires hub API version 1. App changes since 1.2.0. The new pages and blocks appear only where the hub reports them (AI assistants, I2C buses, the layout on the hub, status lights); on older hubs the app keeps its behaviour, and the Overview layout stays in the browser.

### Added
- **Status lights:** in Settings → Device, where the hub has status lights (hub firmware 1.3.0, `/api/v1/led`): what the green and red lights show now, in words (a red code with its number of flashes); the mode — all, quiet (no green beat in normal work) or off; «Find this hub» makes both lights blink for 10 seconds; when the red light reports an event (an unexpected restart, a rolled-back update), «Acknowledge» clears it. Found by quick search too.
- **Assistant changes kept:** the AI page says how many settings versions the hub keeps — the first, from before any assistant change, and the newest 7 (hub firmware 1.3.0); it said «the last 5».
- **I2C buses after a restart:** after «Restart now» the «I2C buses» block re-reads the hub until it is back and clears the «applies after a restart» note by itself; the block stays on screen while the hub restarts.
- **History slots from the hub:** the «history for N points» notes (a point's page, adding a point, ESP-NOW nodes) use the number the hub reports (`history_slots`: 49 on an ESP32-S3 hub with the statistics chip, 12 in the hub's own flash); hubs that don't report it keep 12. With the chip missing (0) the statistics-chip warning speaks instead.
- **Overview you arrange yourself:** a «Customise» button in the Overview's header (in «View» on phones), with a one-time note pointing to it. The Overview is made of blocks on boards («Summary», «Details» and your own). «View → Customise» lets you drag blocks (or move them with the arrows; the keyboard works too), make them a third, half or the whole row, change their settings (the flow's look, a point for a single-point tile, insights folded or full), remove them, add blocks from a catalogue and add, rename, reorder or delete boards. «Done» saves the view in this browser; «Back to the standard view» returns the hub's standard one.
- **The view on the hub:** where the hub keeps the app's layout (hub firmware with `/api/system/ui-state`), «Done» saves the Overview on the hub, so every phone and computer shows the same view; the browser keeps a copy that shows at once. If the view was saved from another device meanwhile, the app asks: keep yours or take the hub's. «My own view on this device» keeps a wall tablet's view in its browser only. A view arranged in the browser before goes to the hub with the first «Done». Older hubs keep the view in the browser.
- **Standard view from what is wired:** the Overview's standard blocks follow the hub's connections — phases for a multi-phase input, the car-park blocks where stations make up the site, the input-against-limit view for a plant — instead of a site scenario chosen by hand.
- **One tile per appliance:** on the Overview the per-phase points of one machine (a heat pump, a three-phase charger) make one tile with the sum; a tap shows the phases. The tiles keep their order (no re-sorting by power as the readings change); in «Customise» a tile moves with ← → or by dragging it to its place, and «Shown / Hidden» takes it off the Overview (it stays in the flow, the tables and Points). The tiles block can order by name instead and leave out points without load.
- **Points in two tabs:** «Points» and «Hardware» (modules, I2C buses, ESP-NOW nodes). A point's wiring (module, channel, bus, address) is under «Connection»; «Set up» and «Unbind» are in the tile's «⋮» menu.
- **Insights folded:** on the Overview the insights show as headlines with their amounts; a tap opens one.
- **AI assistants:** an «AI assistants» page under System, where the hub reports its API for AI assistants:
  - switch access on, with what a paired assistant can read and propose; while the hub has no admin token it says to set one first, with a link to Security;
  - «Read only»: assistants may read but never change settings;
  - pairing: the code an assistant asks for, big and copyable, with the assistant's name, its access and the time left; the page checks for new codes every few seconds while it is open;
  - paired assistants with their access, when paired and last used, and «Revoke»;
  - the newest calls: time, assistant, tool, result (error codes with a short text), duration; calls that changed settings are marked;
  - **connect an assistant:** a tab per client (Claude Desktop, Claude Code, Cursor, VS Code, LM Studio, other) with the command or config to copy, the hub address filled in (its `.local` name, or its IP), an option to let the assistant only read; the remote path for Claude on the web or phone and ChatGPT is shown as coming later;
  - **Deny** on a pairing request; calls today per paired assistant;
  - **changes made by assistants:** the settings versions the hub keeps, with «Restore» showing what changes back before it applies;
  - **hub events:** the newest 20, as an assistant reads them.
- **Pairing banner:** while an assistant waits for its code, every page shows who asks, the code and the time left, with «Show» and «Deny».
- **Settings → Security:** a line that AI assistants need an API token, with their state and a link to the page.
- **Two I2C buses:** where the hub runs modules on bus 0, bus 1 or both, «Add a module» lets you choose the bus (scan, add and the address move work on it), up to the hub's module total. A new «I2C buses» block on Points shows each bus with its pins and modules and sets the pins of bus 1 — the hub checks them against its board and uses them after a restart, offered right there. Module cards and the point's technical details show the bus, since the same address can be used on both buses.
- **Statistics chip missing:** where the hub keeps its statistics on an external memory chip and the chip stops responding, History, Economics and the point page say so: history is not shown and new history is not saved until the chip is back; live values work. The hub firmware panel shows where history is stored.

### Changed
- **Money that agrees across screens:** Economics' «Where the energy went» and the charging purchase use each point's own priced records from the hub (as its Totals and History do), estimating only today's part; the station table has headed columns (energy, fees, purchase, margin) and marks the station picked above; the balance and charging count the input's today whichever point is picked; the appliance page's period total follows the same rule, includes today and draws today's column.
- **Period steps stay on their page:** ‹ belongs to the page it was pressed on; another page, another window or the window's own button returns to the current period; no window button is lit while a past one shows; ‹ stops at an empty period.
- **A home's charger** shows the energy's cost, not fees and margin, on its point page too.
- **A line above its rating** shows on Events and as the point's status; one word «above rating» in the lines table, Needs attention and Events. The rules and «Set limits» stay on Events while a warning is on.
- **No zone advice under a dynamic price** («Night — 1.2× the Night rate» at night).
- **Wall display:** a board taller than the screen scrolls itself down and back while untouched; the hidden top bar leaves no empty band.
- **Search** finds sensors, calibration, revenue, no link, alarms, lines, Wi-Fi password, time zone, electricity price and points by role.
- **Smaller things:** full point names on Points tiles; point chart labels placed by their position, never run together; a round money scale with earnings signed «+»; the Tariffs save bar only while editing; an empty block in Customise says it has nothing to show now; Hardware lists channels in order with factory-paired sensors; an offline station's day is «—», its status «no link».
- **Step through past periods everywhere:** ‹ › with the period's date («October 2026», «10–16 Nov») now sit next to the period buttons in Economics and on the point and appliance pages too, not only in History. A stepped period is a closed one: Economics shows its totals, charging and balance from the hub's closed records, hides «By zone» (the hub counts zones since its restart). The step is kept for the tab's session with its window, so History and Economics show the same past period and a resize no longer drops it.
- **Economics chart with a money scale:** the costs-and-earnings columns have the top value and 0 on a scale at the left.
- **Input against limit leads with kW:** the big figure is the input's power whatever the View; in money mode the ≈ €/h stands next to «of N kW».
- **A home's Overview without the phases block:** the standard Summary of a home keeps phases (current imbalance, cos φ, % of the breaker) in Details; a car park and a plant keep them on the Overview.
- **Network & time:**
  - one «Wi-Fi» card instead of «Status» and «Connection», with «Join another network» under the current link;
  - the Wi-Fi and the access-point password say they save with their own button, apart from the section's bar;
  - the hub's clock: «On the hub now: 22.11, 21:26 · synced 5 min ago» from the live data, and a warning when the hub's clock is minutes off this device's.
- **One word for removing a point:** «Delete point» in the Points menu and on the point page (the two did the same); the dialog names the point in full with its short name in brackets and says first that the history is erased.
- **Point setup:** the save bar names what it will write («Not saved: Name · Current sensor»); the current sensor's hint says it is written with this form's «Save», not at once.
- **Menu by what you want to do:** «Look» (Overview, History, Economics) · «Watch» (Events, with the number of active ones) · «Set up» (Points, Tariffs & prices, AI assistants, Settings). The setup wizard is listed until the hub is set up.
- **Tariffs & prices:** one page; dynamic prices are its second tab (the old Prices address leads there).
- **Rail:** folds to icons with the button at its foot (folded by default on 820–1099 px); the theme moved to the device menu (press the device at the foot of the rail) and to Settings → Interface.
- **Phones:** the bottom bar is Overview · Economics · History · Points · More; events are the bell with the count in the header.
- **Settings in sections:** Site · Interface · Network & time · Integrations · Security · Device, chosen in a list on the left (on a phone: the list, then the section). Each section is one card with its parts one under another — no uneven two-column grid. Old links to a settings anchor open its section; the setup wizard is started from Device.
- **One way to save settings:** each settings section has one bar «Cancel · Save» that saves everything changed in it — the site fields, consumer groups, the time zone and NTP, the MQTT broker, browser access (CORS). Choices that apply at once say so under the field. Actions with consequences (Wi-Fi, the hub's access point, the token, restore, restart, reset) keep their own confirmed buttons. What waits for a hub restart is in one line at the top of Settings and on the Overview, with «Restart now»; it goes away by itself once the hub has restarted.
- **Today by hour:** the scale follows today's hours (yesterday's evening peak no longer flattens them; a yesterday mark above the scale sits at the top with ▲), the chart is taller and has a real title with «As a table».
- **Point page:** breadcrumbs «Overview › Kitchen» or «Points › Kitchen», by where you came from.
- **Events with nothing active:** says how many sensors are heard, when the hub last restarted and which limits are set, with «Set the limits».
- **History points:** «Points: N» opens a list with search and ticks; any point can be picked, up to 6 at once (said in the list), each keeping its colour while picked.
- **One period picker:** History and Economics pick the period the same way (Today · Yesterday · 7 days · This month · Last month · Year) and share it — switching between them keeps the period. Economics links to the same point and period by the hour in History; History in money links back to the totals.
- **Economics chart:** above the totals, what was paid per hour / day / month of the period and what exports earned, on one scale. The by-zone block says in its title that it counts since the hub's last restart.
- **Left to set up:** the Overview opens with a checklist of what isn't set up yet — site name, hub time, metering points, and optionally the tariff, the access token and integrations; each step leads to its place in the app, an optional one can be marked «not needed». It goes away once all is done. It replaces the factory-defaults banner.
- **Quick search:** Ctrl+K (⌘K), the «Find…» field at the top of the rail or the search icon on a phone — go to a page, a point or a setting by its name.
- **Terms explained:** «point», «input», «line», «zone», «unmetered» and «standby use» have one definition each behind an (i), the same wherever they appear; empty places say what will be there, why it is empty and what to do.
- **Wall display:** «View → Wall display» shows a board on the whole screen without the menu, with bigger figures, the screen kept on where the browser allows, and the boards changing every 15 / 30 / 60 s if asked; «Close» goes back.
- **Density:** Settings → Interface → «Roomy / Compact» (tighter cards and table rows, the same type size), per browser.
- **Point page trend:** picks its period like History and Economics, and shares it with them.
- **Points of one appliance together:** on Points, a heat pump's or a charger's phases sit under the appliance's name.
- **One look per role:** a point's icon follows its role (input, solar, battery, charger, heating, room, appliance) — the same on the Overview tile, the appliance tile and the point page.
- **Shorter explanations:** long panel explanations show their first sentence, the rest behind «More».
- **An appliance as one:** a heat pump or a three-phase charger has its own page — the sum of its phases now and per hour, today's energy and money, a trend for the period, its phases with links. The Overview tile opens it (the arrow unfolds the phases); a single-point block, Points (phases folded under the appliance) and the History point list offer the appliance as one line.
- **Round-2 remainder:** a station's page shows today's fees (current price), the energy bought and the margin, and «bought» under its day counter; a point without link says so at the top with «What to check»; the wall display shows «Alarms / No link» when its board has no «Needs attention»; a phase's crumbs lead to its appliance; Economics' totals have one big figure and drop the export rows where nothing is exported or generated; «where the energy went» folds an appliance's phases; History's «Money» is the second tab; «the previous period is incomplete» instead of «no data» beside its drawn line; Customise names blocks as their headers; the appliance chart's columns (MUI `minWidth: 1` = 100 %).
- **Round-2 acceptance fixes:** Economics «Charging» loads the customer price itself (a bookmark showed «not set»), folds a three-phase charger into one row and on a home is «Car charging» — energy and its cost, no fees or margin; the Overview's margin and purchase use one buying price; one «close to the limit» rule (80 %) for the input bar, «Needs attention» and Events (no «no active alarms» under the warning); the appliance page's chart has a scale, dates and the period's total in kWh and ≈ money, money leads; Settings restart: «restarting» stays, Cancel / Restart now are hidden meanwhile, the dialog names everything it writes; quick search: «limit» opens the input's setup, «modules» the Hardware tab, places instead of «page», finds «sensor» and «group»; Points: an appliance header in a frame with its power, single points under their own header; the save bar below the phone header; «no link» stations in the warning tone, a free station shows its state instead of «0.00 €/h»; zone colours the same in Tariffs and Economics; the wall display never below ×1; a new Customise block goes to the end and the page scrolls to it; «loading» instead of «no data» while Economics loads; no white flash before the dark theme.
- **Where the energy went, month per line, station list settings:** Economics → Balance lists each line's energy for the period with its share and an estimated cost (the period's average buying price — the hub prices only the input); the lines table has «Month to date» and keeps a stable order instead of re-sorting by power every second; the car-park «Stations» block has its own settings in Customise — order (as configured / by name / charging first) and free stations shown or hidden.
- **More from the role round:** «Yesterday» in the Today block; a plant's standard view has Today (with the month) and «Needs attention» next to the input limit; the lines table says «normal» instead of an empty status; Events warn «the input is close to its limit» from 80 % (the hub has no warning level of its own); the day's charging fees are marked «≈ at the current price» (the hub keeps no price history); «By zone» in Economics is said to count since the hub's restart and is hidden for a past period; the wall display scales to the screen (×0.9–1.5); in Customise a new block goes to the top and the bottom bar is one row on a phone; the demo car park has a fourth charger that has lost its link.
- **Visual polish:** figures in one typeface everywhere (the monospace stays for codes and logs); readable text at 13 px and field labels at 12; «Today», «Energy flow» and the main card carry a real block title; the main card stands out in both themes; side figures of the input card no longer break at 1100 px; tiles of a row are the same height; a period list instead of six buttons on a phone; History's two downloads behind one «Download»; the single-line diagram fits the card from a tablet up; the theme switch has words; no «Ctrl K» on touch screens; the menu follows the breadcrumbs; spaced capitals only in the menu.
- **Role-agent round fixes:** quick search understands Russian words and says which section a setting is in; changing the mains system asks first and names the points whose phase would not fit; «Save and restart» saves the section's other changes too; «Grid export price» in Tariffs; car-park blocks only on a car park; the wall display fits a laptop; the Security settings section no longer crashes.
- **Visual round fixes:** imports are the same slate colour on every chart (blue is for selection and links); a new page opens at its top; the settings bar appears only when there is something to save; «Done» in Customise is always «Done»; Compact is visibly tighter.
- **One type scale:** page titles, block titles, text and captions follow one scale; no readable text below 12 px. Block headings share one pattern (the name on the left, its actions on the right).
- **Flow colours everywhere:** the export colour in «Key figures» now shows (the flow colours were defined inside the diagrams only).
- **Card weight:** the page's main figure («Now», «Input against its limit», «Charging now») sits on a raised card; insights and notes on a quiet one.
- **Loading:** pages show their outline while the data loads instead of a spinner in the middle.
- **Name:** «RBgrid» everywhere in the app.

### Removed
- **Site scenario:** «Home / Business / Charging stations» is no longer chosen in Settings, in the setup wizard or on the Overview; the Overview follows what is wired. A hub that was set to «Business» or «Charging stations» before keeps that look until its view is customised. The demo still offers its sites.

## 1.2.0

For hub firmware 1.2.0. Requires hub API version 1. App changes since 1.1.8. The Prices page appears only where the hub reports dynamic prices; on older hubs the app keeps its behaviour.

### Added
- **Dynamic prices:** a «Prices» page, where the hub reports them. It covers what the Home Assistant panel shows:
  - **Price now:** the price in force for this 15-minute period, its rank in the day and its source in plain words (primary, backup, Home Assistant, or the plan when nothing covers). It also shows how long the stored prices last, the last fetch error, and the «cheap now» and «low export price» flags.
  - **Today and tomorrow:** buy (all-in) and sell per 15-minute period, with «now» and negative prices. Periods priced by the backup or the plan are marked. Hovering shows each period's prices and source.
  - **Where the price comes from:** a provider and a backup from the hub's own list, and the zone with its currency (a zone in another currency than the plan is refused up front). For a spot price: the contract formula (factor, markup, adding the plan's zone rate), billing at the hourly mean, and the lowest buy and sell prices. The export price comes from the provider, the plan or a formula. Octopus products and a provider token are asked for where they apply.
  - **Cheap windows:** up to 8 rules (cheapest periods in a time span, cheapest before a time, or price below a level) with their live state, plus the notification flags. The hub only reports these; it switches nothing.
  - **Import peaks:** the highest 15-minute and hour averages and the three highest days in the billing cycle, with the cycle's start day and a reset. Changing the start day asks first: it starts a new cycle, and the current figures move to the previous cycle.
  - **Applied prices:** the price the hub booked each closed 15-minute period at, any of the last 40 days, as a table and a CSV.
  - The providers' attribution is shown verbatim wherever their prices are, such as Energy-Charts (CC BY 4.0).
- **Overview:** with a dynamic price in force, the Today card reads «Price now: dynamic» with the all-in rate. Zero and negative prices count as prices. **Tariffs** says the plan's zone rates now apply only where the dynamic price has none.

- On a hub without dynamic prices the page isn't in the menu; opened by its address, it says the hub doesn't report them and asks the hub nothing.
- **Remote nodes:** a warning with «Restart now» when the hub reports its node-data task isn't running (`inject_age_ms` −1, hub 1.1.8): the nodes' readings and records don't reach the hub until it restarts.

### Fixed
- **A point measured inside another (1.1.8):** a chain of three (a sub-panel inside a panel, a charger inside the sub-panel) counted the innermost point twice when the middle one wasn't in the sum, for example in History «Total and parts» with only the outer and the inner point selected. Now the whole chain is followed.
- **History with remote nodes:** the input comes first among the points, so it is selected when History opens and is always among the points that can be selected. Before, a node's points (keys 6Axx) sorted ahead of the input (9001), opened first, and could push the input out of the first six.
- **History chart, a part of an hour:** a part that starts after the half hour (the hub came back at 18:40, or a restart split the hour) is drawn on its own hour. Before, it sat on the next hour's bar.
- **Economics → Balance on a site with solar or another source:** the site's use is now import + generation − export, against the metered lines. Counting import alone made «Unmetered» negative on a solar house, since the panels feed the lines too.
- **Settings → Home Assistant, «Individual devices»:** a point measured inside another names that point as its upstream device, so HA's Energy dashboard doesn't count its energy twice.
- **History note for a break in measurement (quality bit 12):** it now also says the hour's cost is approximate, since which rate applied during the break isn't known.

## 1.1.8

For hub firmware 1.1.8. Requires hub API version 1. App changes since 1.1.7. «Measured inside» and the broker CA appear only where the hub reports them; on older hubs the app keeps its behaviour.

### Added
- **A point measured inside another, on hub firmware 1.1.8:** a point's settings have «Measured inside». Use it for a load behind another point's meter, such as an EV charger in a garage sub-panel that has its own meter. Its power then counts once, in that point. The unmetered rest, the lines' share of the input, the phase loads, the energy flow, the balance and the bill shares no longer count it twice. In History, «Total and parts» stacks only the outer point and names the inner one in the tooltip. The list offers other lines only: not the input, and not a point already inside this one. Older hubs don't show the setting.
- **History notes for hub firmware 1.1.8:** an hour that includes energy from a break in measurement says so (for example a remote node's period that began before it had the hub's clock). The «estimate» note now also covers power taken from the nominal voltage, as on a node channel that measures current only.
- **Secure MQTT (mqtts) on hub firmware 1.1.8:** «Secure connection (TLS, mqtts)» can be switched on. Under it, «Broker CA (for mqtts)» uploads your broker's certificate authority (a .pem or .crt file, up to 8 KB), as for Mosquitto in Home Assistant with its own CA. It lists each certificate's subject, issuer and dates, and can be removed. Both apply after a restart, and the app offers «Restart now». A file with a private key is never sent. Next to the broker, the hub's TLS state reads in plain words: verified, waiting for the clock, certificate expired or not yet valid, issued for another host name, or an authority the hub doesn't know.

### Changed
- **Turning the radio for nodes off:** the confirmation says what is kept. The nodes stop sending live values, but keep their 15-minute energy records and send them when the radio is on again (up to 2 hours on node firmware 1.4, about 5 days on 1.5).

## 1.1.7

For hub firmware 1.1.7. Requires hub API version 1. App changes since 1.1.6. New sections appear only where the hub reports them (live-stream ticket, restore-point checks, remote nodes); on older hubs the app keeps their behaviour.

### Added
- **Live readings on hub firmware 1.1.7:** the app opens the live stream with a one-time ticket that is valid for 30 seconds. The hub token no longer goes into the connection address. Older hubs connect as before.
- **Restore point on hub firmware 1.1.7:** saving and restoring are all or nothing on the hub, and the panel says what happened. A failed save says the point couldn't be saved. A saved point that is incomplete is refused, with a prompt to save a new one. A restore that stopped part-way says the hub put back its previous settings. In both refusals nothing changed and the hub doesn't restart. When the hub already knows a saved point is incomplete, the panel says so up front and keeps «Restore and restart» off.
- **Remote nodes (ESP-NOW) on hub firmware 1.1.7:** a section on the Points page, shown where the hub's firmware can run nodes. Turn the radio for nodes on or off, and see its channel (the Wi-Fi router's). Pair a node with a countdown and a cancel; the new node is marked, and its points appear without a restart. See «N of 6 nodes», and how many of the 12 history slots are in use, before pairing. Each node card shows when it was last heard, lost packets and clock sync, and its channels with links to their points. A node can be turned off and on (turning it off asks first: the hub discards its energy while it's off), or removed with its points and their history after a confirmation. A firmware that can't run nodes says so. A node the hub hasn't heard since it started reads «not heard since the hub started». Where the hub reports a node's settings, «Settings» sets, per channel, on/off and the current clamp (the energy period is fixed at 15 min and shown); the card shows «settings not applied yet» until the node confirms. The setup wizard offers the Nodes step on a hub that can run nodes.

### Fixed
- **Hub token:** kept per hub. Switching hubs, or opening a link to another hub address, no longer sends one hub's token to another. A token saved before this version stays with the hub it was saved for.
- **Economics, ESP-NOW, the setup status:** on a hub with a read token, these reads now carry it, so they no longer read as «no data».
- **History by month and year:** a month's record starts on the 1st by the hub's calendar. A 31-day month used to start on the 4th, and «This year» dropped January. Days with a clock change (23 or 25 h) land on their own day.
- **History:** switching points or periods while a slow answer is still on the way keeps the new series. The old answer no longer replaces it.
- **Today in period totals:** month to date, Economics and History take today by the same rule as the Overview cards: the input's own counters where the hub's sums stay 0 (older hubs with an rbAmp input).
- **Summary by point (CSV):** a failed read stops the export with an error. It used to download a file with zeros.
- **CSV files:** a point name that starts like a spreadsheet formula (=, +, -, @) is written as text, so a spreadsheet shows it rather than runs it.
- **Month forecast:** counted on the hub's calendar. A browser in another time zone near the turn of the month used the wrong month.
- **Economics:** the net-metering card shows only once the plan is read and has net metering on. It used to flash while the plan was loading.
- **Tariffs:** a zone time of 24:01–24:59 is refused. 24:30 used to be saved as 24:00.
- **History, «Total and parts»:** when the lines add up to more than the input, the bar fits the scale. It used to run past the top of the chart.
- **Switching hubs:** the module and sensor list of the previous hub no longer shows up under the new one.
- **Hub address:** live readings work with an IPv6 address such as `http://[fe80::1]`.
- **Tariffs, the week map and «zone now»:** each condition of a zone is checked on its own, as the hub does. A zone with two time windows, or two day sets joined by «or», showed only one.
- **Tariffs:** a zone type another zone already uses isn't offered for a second zone. The hub keeps zones by type, so two «Night» zones mixed their hours and money.
- **A hub that wants a token:** when the hub withholds its readings for want of a read token, the app says so and opens Settings → Security. It used to show «No connection».
- **Settings, «Save and reboot»:** once the hub is back, the app re-reads what was written and says whether it took. Before, «Written, applies after a reboot» stayed up after the restart.
- **Tariffs → Net metering:** the selling ratio stays visible while net metering is on. The hub scales each surplus month's kWh credit by it (0.7 on a new plan), so it matters even when every zone has its own selling price. The app used to hide it there and say it wasn't used.

## 1.1.6

For hub firmware 1.1.6. Requires hub API version 1. App changes since 1.1.5. On older hubs the app keeps their behaviour: the input's history reads the main point, and the time settings are saved whole.

### Added
- **Per-phase input, history and money:** on a hub with firmware 1.1.6 the input's history and money per period cover all three phase points, not L1 only: Overview (today by hour, vs yesterday, month to date, forecast) and Economics (the input's totals, balance, generation, billing). Older hubs keep reading the main point.
- **Settings → Time:** the NTP servers and the resync interval are checked as the hub checks them (a host without spaces or “/”, up to 63 characters; 60–86 400 s) before Save.
- **Settings → Hub firmware:** a second image sent while the hub still confirms the last one (fw 1.1.6, 409) says to try again in about a minute instead of a bare failure.

### Fixed
- **Settings → Time on hub firmware 1.1.5 and earlier:** Save sends the whole time configuration again. Those hubs replace the stored settings with what they receive, so saving only the changed field (the time zone, say) erased the NTP servers and the hub had no time sync after the next restart. Hubs on 1.1.6 merge and still get only the changes.
- **Three-phase input switch:** the result and **Restart now** show on the module card where the button was pressed (they sat above the module grid, often out of view); after a switch the card says the change applies after a restart and hides the buttons until then, so it can't be pressed twice; a repeat bind answered «already bound» on a phase that already carries an input point counts as done, not as «Not every phase got its point» (bench round trip on .139, 06.11).

## 1.1.5 — 2026-11-06

For hub firmware 1.1.5. Requires hub API version 1. App changes since 1.1.4. With the input metered per phase, history and money per period show L1 only in 1.1.5; live values and the hub's system totals are the full sum.

### Added
- **Three-phase input: one point, or one point per phase** (hub ≥ 1.1.5, root u3i3-per-phase-points). On the STD-U3I3 card that carries the input (Points): «Input: one point for all three phases (phases netted)» / «Input: one point per phase», with the buttons «One point, phases netted» and «One point per phase», a confirm saying what each means for the bill, and the restart prompt. Per phase: the input keeps its history on L1, L2/L3 become input points of their own. Back to one point: those points are deleted (their own history erased, their totals move into the input). With the input split, the app reads «the input» as the sum of its phase points, each one a phase of it, as the hub sums them.

### Fixed
- **Sensor limit (Point settings, module cards):** the accurate range's upper bound and the «line breaker is above the sensor's limit» warning use the hub's own figure for where the clamp starts clipping (`ct_fs_a`; SCT-013 ≈ 16 % above its rating), no longer 90 % of the rating — a 10 A clamp on a 10 A breaker doesn't warn any more; with no hub figure (STD factory-paired sensors) there is no range and no warning (088381b; added to this list 06.11, it was missing at release).
- **Tariffs, export-only tariffs (e.g. SEG):** every zone shows its **Sell price**, whatever the net-metering toggle; a new zone always gets an explicit sell price (0 when no other zone has one); the toggle reads «Net metering (monthly kWh credit)» and its help says to leave it off for a paid-per-kWh export tariff (content sellprice-gated-on-netmetering).
- **Hub's day on a clock-change day:** «today» windows, hour axes and day totals start at the hub's real midnight on the days summer time begins or ends — after the change they started an hour early (Berlin 29.03: 22:00Z instead of 23:00Z). Where midnight doesn't exist (Santiago's 24:00 change) the day starts at 01:00, like on the hub.
- **Time:** southern-hemisphere zones in the timezone list and detected from the browser — Sydney/Melbourne, Adelaide, Brisbane, Perth, Auckland, Santiago (with their summer time), São Paulo, Johannesburg; the raw config editor no longer shows «Daylight saving» (`enable_dst`): the hub takes summer time from the zone's rule alone, so the flag changed nothing (content dst-toggle-dead).
- **Tariffs, texts:** the priority hint says a Weekend zone ranks 3rd only on Saturday and Sunday (last on weekdays) and that a zone's days come from «Days»; «Remove zone» says where its hours go — to the default zone, or unpriced when there is none or the zone removed is the default.
- **Point page:** «readings not corrected» sends you to Point settings → Current sensor, where the model is changed (it pointed at the module card).
- **Car park:** a three-phase charger bound as one point per conductor is one station when its points share a group (2–3 station points, each on its own phase L1/L2/L3, same rated power) — one card, one load-balance segment and one «Sessions today» session, counted on the sum of its points (was ×3). The group's name from Settings → Consumer groups names the card.
- **Economics → Balance:** «Unmetered, % of input» shows «—» when the input imported under 10 Wh for the period (was −3 157 043 % on a near-idle input); Overview's unmetered share likewise hides below 50 W of input load (root econ-balance-percent).
- **Tariffs:** a zone's sell price stays visible and editable with net metering off (the hub values exports at it
  either way), with a note to set it to 0 without a feed-in contract; the net-metering text calls the ratio the
  «selling ratio», as the field does.

## 1.1.4 — 2026-11-04

For hub firmware 1.1.4. Requires hub API version 1. App changes since 1.1.3:

### Fixed
- **Unbind / Delete point:** both dialogs say the point's stored history is erased for good (it is — the hub frees
  the slot); the sensor channel stays for a new point.
- The quick tariff doesn't ask before replacing the hub's factory example plan, only a plan you set up.
- The input's role texts no longer send you to the setup wizard (the hub keeps it fixed).
- **Home Assistant:** with no broker saved the panel says MQTT is off (the hub has no default broker).
- **Add a module:** when the hub's module at 0x50 isn't answering, the dialog points to «Find one at another address».
- **Add a module (hub 1.1.4):** an empty placeholder module at 0x50 (`placeholder: true`) no longer blocks the dialog — a new module from the box connected there takes that slot.
- **History:** points past the first six are greyed out (there is no «Other» group).
- **Connect screen:** the address hint names the router's device list and `rbgrid-xxxx.local`.

## 1.1.3 — 2026-11-04

For hub firmware 1.1.3, which answers browsers only from allowed origins and always allows the hosted app
at `https://webapp.rbgrid.io`.

### Changed
- **Browser access (CORS):** the hub's built-in origins are shown as fixed rows («always allowed»); your own
  origins are the editable list (up to the hub's limit); «Allow any website (not recommended)» is the wildcard.
  Saving always sends the choice explicitly. A page served by the hub itself is never asked to add its origin.
- **Connect screen:** in the hub kit it offers only the hub's IP address (the broker way was never in the kit);
  on an https page it reminds about the browser's local-network permission when no hub answers.
- **Demo:** the demo-site picker sits under the logo.

### Fixed
- **Where the money goes:** when some part has no cost, the bar shows energy for all parts (it drew Wh as money).
- **Standby insight:** the share reads «% of the month's consumption» — it is computed from energy, not cost.
- **Surplus insight:** says the buy/sell price difference instead of «better than selling it».
- **Economics:** the net-metering credit block is hidden while the plan has net metering off; the progressive
  block shows only when the hub runs one.
- **Setup wizard:** the quick tariff asks before replacing a fuller plan (taxes and levies, net metering, fixed
  payments, more zones); «Open MQTT / Home Assistant settings» lands on the Home Assistant panel.
- Wording: two-pole phases on a three-phase site no longer say «240 V»; current-only modules don't mention load
  control; hints name the real panels (Settings → Connection / Security).

## 1.1.2 — 2026-11-03

The web UI for hub firmware 1.1.2: `rbgrid-ui_v1.1.2.zip` = `kit/` + `ui-manifest.json`, installed the same
way as 1.1.0. Requires hub API version 1. The new screens (add a module, the input on a U3I3, the hub's
access point, factory reset) show on hubs with firmware 1.1.2 or later; on older hubs the app works as 1.1.0.

### Changed (2026-11-03)
- **Release:** the v1.1.2 screens are on (bench gate 03.11, hub image 4456a483). Adding a module also finds one
  already set to another address (scan shows its model and firmware) — only a module from the box needs 0x50 freed.
- **Hub access warning** (Overview + Settings, hub 1.1.2+, rides with the access-point block): while the hub's
  own Wi-Fi keeps the setup password and no admin token is set, a warning says anyone nearby can join and
  change the hub, with links to both; a lighter note while only one is done. Hideable for the session.
- **Add a module** is on for hubs with firmware 1.1.2+ (bench run 03.11: scan 0.17 s, readdress ≈ 2 s, the
  module keeps its points). The «bus stuck» text names the real causes (a module without power, in another
  mode, or miswired) instead of pull-ups. Points warns when a bus line is held low (`bus_line_low_s`) — it
  clears by itself, no restart.
- **Live-only points** (hub 1.1.2 `history:false`): the point page and History say the point has no stored
  history and why (the hub keeps history for 12 points, or a restart starts it); the bind dialog warns when all
  12 history slots are taken. Older hubs don't send the flag — nothing shown.
- **Factory reset** (Administration): on hubs with firmware 1.1.2+ it is a real reset (`/api/system/factory-reset`):
  settings, points, tariff, Wi-Fi, tokens, the access-point password and — unless the box is cleared — the
  history; the hub comes back with its own access point, and the dialog says the app loses it. Older hubs
  get no reset button: their call only undid the last site-settings save. Hidden in release builds until the
  bench run (`RELEASE_HIDDEN.factoryReset`).
- **Points:** «The module bus is stuck — no readings for N s…» with Restart when the hub reports
  `bus_stalled_s` (v1.1.2). The add-module steps say to unplug a module's whole cable, not only its power.
- **Hub access point** (Settings): shown on hubs with firmware 1.1.2+ that list `/api/system/ap`, hidden
  in release builds until the bench run (`RELEASE_HIDDEN.apPassword`); when the hub answers `applied: false`
  the card says the password takes effect after a restart and offers «Restart now».
- **Time:** the status chip reads «not synchronized» for every hub state that isn't synced or syncing
  (UNINITIALIZED, TIMEOUT, ERROR, FALLBACK) — those showed no chip at all.
- **Points:** «Use as the three-phase input» on an STD-U3I3 card (hub firmware 1.1.2+): the hub regroups
  the module under the input's point after a restart, history kept (`POST /api/v1/units/<input>/rebind`).
  Older hubs get a caption to update. Hidden in release builds until the bench run (`RELEASE_HIDDEN.inputGroup`).
  The old «Bind as one 3-phase point» offer is gone — every hub version refused it.
- **Demo:** the demo hub reports no firmware version (demo.rbgrid.io names only a published firmware,
  site team 03.11); its HA note drops the sentence about firmware 1.1.2.
- **Russian:** the month forecast reads «Прогноз стоимости за месяц», not «Прогноз счёта» (a bill isn't promised).
- **MQTT broker:** the «Secure connection (TLS, mqtts)» switch is hidden in release builds — the hub
  can't verify a broker certificate yet. A hub that already has an `mqtts://` address keeps it; the
  switch shows it read-only.
- **Home Assistant:** a note under the Energy-dashboard map for hubs before firmware 1.1.2 — HA may not
  offer «Cost (Total)» / «Revenue (Total)»; 1.1.2 fixes the type and names the second Export value.
  On a 1.1.2+ hub the map names «Export value (Total)» and the note goes (version from `/api/system/version`).

### Changed (2026-11-02)
- Binding a point no longer asks for **History storage**: the hub keeps hourly records and rolls
  days, weeks, months and years up from them for every point whatever was picked there.
- «Savings» is now **Insights**; money for exported energy reads **Export value** (was
  «Revenue»), as on rbgrid.io.
- «Monthly bill forecast» reads **Month cost forecast** (the hub's estimate at the plan's rates, not the
  supplier's bill).
- New insight: **EV charging yesterday in a dearer zone** (titled by the user's own group / point name) — what the same energy would have cost
  in the cheapest zone, from the plan's rates (a past fact, not a forecast).
- «Where the money goes» splits the remainder honestly: measured points past the top are «Other points»,
  what no point measures is «Unmetered» (one «Other» read as full coverage).
- History by days (7 days, this month) draws today too — a partial bar from the live counters (the
  period total already counted it).
- «Where the money goes» counts an appliance on several phases (a group whose points share one
  role — a three-phase charger, a heat pump) as one share.
- The standby estimate leaves out heat pumps and chargers (they run on their own schedule).
- The tariff editor no longer shows **fixed payments** or the **per-zone credit ratio**: a v1.1.x hub
  counts neither in any cost (fixed charges) or settlement (the plan-wide ratio is used); the plan
  keeps them and a save sends them back unchanged.
- The tariff editor no longer offers the **Holiday** zone type: v1.1.x hubs have no holiday dates
  to fill, so the zone never activates (a plan that already has one still shows it).


## 1.1.0 — 2026-10-20

First release of the web UI as a hub-installable kit: `rbgrid-ui_v1.1.0.zip` = `kit/` +
`ui-manifest.json` (`npm run build:embedded`; installed with `ui-upload` / `ui-commit`, see the
hub documentation *Installing the web UI*). Requires hub API version 1. Interface language:
English by default, Russian included.

### Release scope
- Some screens in the code are not part of this release and are hidden in a release build
  (`src/constants/release.ts`).
- Telemetry frame v2 is the hub's default; the UI reads v2 and older v1 hubs alike.
- Split-phase mains is deferred: not offered in a release build (still shown on a hub already set
  to it). The input is channel I0 of the first module; choosing it comes later.

### Public demo
- `npm run build:demo` → `rbgrid-demo_v1.1.0.zip` for demo.rbgrid.io: built-in demo sites, each
  with its own hardware and situation. The default is a three-phase house with a heat pump and an
  EV charger on a two-zone DE tariff (8 points — the v1.1.x history limit); the others are a home with solar, a flat on one 7-channel
  module, a three-phase workshop and a car park with chargers. Links: `?scenario=heatpump-ev|solar|flat|workshop|carpark`
  (or `?demo=<id>`), optionally `&screen=overview|history|economics|events|tariffs|points|settings|onboarding` and
  `&range=today|yesterday|7d|month|year` for History.

### Added
- **Overview by site scenario** (home / business / charging stations): live power and money,
  energy-flow schemas (home, distribution, balance, single-line), «Needs attention», savings
  advice, month forecast, standby («vampire») estimate and a guided hunt.
- **Charts** (one module for all screens): today by hour with a live running hour — with solar
  on site the energy view shows the house's hour in two sections, from the grid and from its own
  solar, with export below the line, point trends,
  History with time windows, previous-period ghost, hover card, «as a table» view, CSV export
  and a per-point summary CSV. Charts refresh by themselves (hours every minute).
- **Points**: bind a free sensor channel (point ID, reuse of a removed point's ID), rename, role,
  phase, rating, groups; module cards with model, channels in use («3 of 7 in use»), the CT of
  each channel and its accurate range; module firmware update for STD modules.
- **BASIC modules** (UI1/UI3/UI7, I1–I3): consumption-only metering said up front; CT model per
  channel (with a warning that the factory calibration is replaced); a backwards CT is reported,
  not shown as export; CT clipping / near-limit («clipping» / «sensor limit») on the tile, in «Needs
  attention» and the tables; SCT correction status per channel (uncorrected readings are marked).
- **Multi-phase**: mains system (1P / split / 3P), per-phase cards, voltage band and unbalance.
- **Hub log** (Events → Log), hub firmware update from Settings, settings with the hub's own
  ranges and on-restart fields, interface language and number/date formats.
- **Hub access point** (Settings → Network): the hub's own Wi-Fi `rbgrid-<mac4>`; the user sets
  its password, with a reminder while the first-login password is in use.
- **Setup wizard** for a fresh hub: Wi-Fi, time zone, tariff as printed on the bill, points,
  admin token.

### Changed
- A point bound to a channel the fitted module doesn't have (a 3-channel module in place of a
  7-channel one) reads «no such channel on the module», not «offline», and is left out of alarms
  and the flow; such channels are never offered for binding.
- A free channel is «free», not an error. A CT model change restarts the module; its points keep
  their last reading for a few seconds instead of flashing a fault.
- Unbinding a point warns that the hub deletes its history (download CSV first).
- The UI re-reads the hub's hardware list as soon as the frame's `hw_rev` changes.

### Fixed
- Language packs are served from `static/lang/` (the only kit folder a hub serves).
- Erased-flash history records (year 2106) are dropped; day totals are checked against the
  hub's hour records.
- Energy-flow cards keep their text inside the card in every language.

### Security / build
- Flow schemas XML-escape every data-derived name before the SVG is injected.
- Clean-clone build: the design system builds itself on install; `package-lock.json` tracked.

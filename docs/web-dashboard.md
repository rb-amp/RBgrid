# Overview

The **Overview** is the first page of the RBgrid app. It shows what your hub measures right now and today: the power at your grid input, the power on each metered line, the phases, the day's energy and, once you've set a tariff, what that energy costs at your rates.

Every figure on the page comes from two places:

- **the hub's measurements**: the rbAmp modules wired to your input and your lines, read live and stored as hourly history on the hub;
- **your tariff**: the price plan you enter on the [Tariffs](web-tariffs.md) page. Without a tariff the Overview shows power and energy only.

RBgrid measures. It doesn't switch or regulate any load; everything on this page is information for you to read.

**From firmware v1.3.0 the page is yours to arrange.** It is built from blocks laid out on boards, and you can
move them, resize them, add them from a catalogue and take them away.

![Overview · Power · Summary](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-power-mode.webp)

## Boards and blocks

The Overview comes with two boards, shown as tabs above the blocks: **Summary** and **Details**. You can add
boards of your own, rename them, reorder them and delete them.

Each board holds **blocks**. A block is one panel showing one thing — the power now, today's energy, the
phases, the energy-flow diagram — and you choose which blocks sit on which board, in what order, and how wide
each one is.

**What the standard boards show follows what you have connected**, not a setting you pick: the phases block
appears when your mains is multi-phase, the car-park blocks when your stations make up a car park, and the
input-against-its-limit block where there is generation. Up to v1.2.x this was a *Site scenario* chip you chose
yourself; from v1.3.0 the page reads the hardware instead.

The top of the page also holds the page title, an optional **data delayed** chip, and the **View** menu, which
is where you switch between power and money, open the wall display, and start **Customise**.

### Arranging it yourself

**Customise** in the page header starts it; on a phone it is under **View**. While it is on, you can:

- **move a block** — drag it, or use the arrow keys;
- **set its width** — a third of a row, a half, or the whole row;
- **change what a block shows** — each block has its own settings, and for several of them this page says what
  they are;
- **remove a block**, or add one from the **Add a block** catalogue;
- **work on the boards** — add, rename, reorder or delete them.

**Done** saves the arrangement. On a hub running v1.3.0 it is saved **on the hub**, so the same arrangement
greets you on every device you open the app from. On an older hub it is saved in the browser you are using, and
another browser still gets the standard view.

**Back to the standard view** drops your arrangement and returns the standard boards.

### A board on the whole screen

**View → Wall display (full screen)** shows one board with nothing around it: no menu, larger figures, and the
screen kept awake where the browser allows it. Its address is `#/board/<board>`, so a tablet by the panel can
open straight into it.

With several boards, the wall display can cycle through them every **15**, **30** or **60** seconds. A board
taller than the screen scrolls down and back by itself. **Close** returns to the app.

### Left to set up

A new hub shows a check-list, **Left to set up**, at the top of the Overview, and each line leads to the place
in the app where you do that step.

- **Needed:** the site's name, the hub's time, the metering points.
- **Optional:** the tariff, an access token, the integrations. An optional line can be marked **Not needed** to
  clear it.

The check-list goes away once everything is done. It is also a block in the catalogue, so you can keep it on a
board of your own while you are still setting the hub up; with nothing left to do, the block stays out of sight
on the Overview and shows as an empty slot only while you are arranging the page. It is not the setup wizard: the wizard is its own
page, started from **Settings → Device**.

### Showing money instead of power

With a tariff set, **View → Show money** and **View → Show power** choose which figure leads on the blocks
(up to v1.2.x this was a **Power | Money** switch on the page itself):

- **Power** leads with watts and kilowatt-hours; the cost is the small line underneath.
- **Money** leads with the cost in your tariff's currency; the power or energy is the small line.

The flow diagrams always stay in kW. Limits, headroom, phase currents, voltage and frequency always stay physical units.

Your choice is remembered **in this browser, for this hub**; it isn't a hub setting.

Without a tariff the switch is hidden and the page stays in Power.

**How the money figures are worked out**

Money values marked **≈** are the app's estimates at your current rates. The day's import cost and export value on the **Today** block come from the hub's own counters.

| What | How the app values it |
|---|---|
| Live power you import | power × your buy rate, shown as "≈ 0.12 €/h" |
| Live power you export | power × your sell rate (no sell rate set: 0) |
| Solar generation | the part your home uses itself × buy rate + the part exported × sell rate |
| A day's energy without a hub cost total | energy × today's average price at your input (or the current buy rate) |

The **buy rate** is the hub's all-in rate when it reports one; otherwise the app takes the current zone's rate, adds the per-kWh energy tax, grid fee and renewable levy from your plan, and applies VAT. The **sell rate** is the one in your plan. See [Tariffs](web-tariffs.md).

### The two standard boards

![Overview · Money · Details](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-details.webp)

- **Summary** is the at-a-glance board: what is happening now and what today has cost.
- **Details** is the analytical one: the key figures, phases, the hourly chart with the attention list, Insights
  and the tariff ribbon, the energy-flow diagram and a table of all points.

Up to v1.2.x these were a switch; from v1.3.0 they are boards, and they are tabs above the blocks. Which tab you
are on is kept while the browser tab stays open; a new tab starts on Summary.

### The "data delayed" chip

**data delayed** appears next to the title when the hub's live frames arrive with gaps (some frames were skipped). The figures are still real, but they may lag a few seconds behind.

## The blocks

Every panel on the page is a block, and each one can go on any board. This is what each block shows; they are
in the order that reads best, not the order of the **Add a block** catalogue. Up to v1.2.x these same cards were fixed sections of the page, grouped by the site
scenario; from v1.3.0 they are yours to place.

![Overview · Money · Summary (house with heat pump and EV)](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-money-mode.webp)

### Now

The big figure is the power at your **grid input** right now, with an arrow: ↓ importing from the grid, ↑ exporting to it. In Money mode the hourly cost (≈ €/h) leads and the power is under it.

The label tells you the state:

| Label | Meaning |
|---|---|
| Importing from grid / Exporting to grid | The input module measures the direction. |
| Grid input now | The input doesn't report a direction. |
| Input · no flow | The power is too small to tell from zero (twice the input's resolution, or 0.5 W). |
| Input · no data | No input point, or it isn't answering. |
| … · held 12 s | The hub hasn't had a new reading for that long; the last value is shown greyed. |

Below the figure:

- **Voltage**: from the input, the voltage module, or the average of the input's phases; per-phase values (L1, L2, L3) underneath when a voltage module reports them.
- **Frequency**: only when something on the site measures it; otherwise "—".
- **House load**: what your home uses now (see *Energy flow*); "—" when it can't be known.
- **House from solar**: with a solar (generation) point and a directional input, a bar shows what share of the house load comes from solar and what from the grid.

Two checks can appear here:

- *The input reads export to the grid, but there is no generation or storage on site…* — the input shows more than 5 W of export and you have no solar or battery point. Most often the input's current sensor is clamped the wrong way round, or the point has the wrong role.
- *The phases' power sign disagrees with the input total…* — the phases and the total point in opposite directions (both above 50 W). Check the direction of the phase current sensors.

### Today

The **Today** block shows the day so far:

- **Imported**: energy from the grid, and its cost from the hub's own cost counter.
- **Exported**: shown once at least 10 Wh went out; its value from the hub's export counter.
- **Net**: imported minus exported, in energy and money. Shown together with **Exported** only.
- **Month to date**: the closed days of this month from the hub's history plus today.
- With a three-phase input **metered per phase**, these figures cover the whole input — the three phase points summed by the hub (firmware **v1.1.6** with the matching app package; on **v1.1.5** they covered one phase only).
- The current zone and rate, for example "Zone: night · 0.18 €/kWh incl. VAT & levies". With no tariff the line isn't shown.
- **While a dynamic price is in force** (web app **1.2.0**) that line reads **"Price now: dynamic"** with the rate actually in effect for this 15-minute period, instead of a zone name — because there is no zone doing the pricing, there is a market price.

  ![The Today block's rate line reading «Price now: dynamic · €0.2446/kWh incl. VAT & levies»](https://www.rbgrid.io/assets/img/webapp-docs/wa-overview-price-dynamic.webp)

  The figure is the all-in price: what a kilowatt-hour costs you right now, with your contract and charges in it. Where it came from, and what it was for every period already booked, is on [Dynamic prices](web-prices.md). A dynamic price that covers no period right now falls back to the zone, and the line goes back to naming it.

"Today" means the hub's day. If the hub's clock isn't set, a note says that "today" counts from the hub's last midnight or restart, not your calendar day. Set the clock on the [Time](web-time.md) page.

If money is still counted with the factory example tariff, the block says so and links to **Set your tariff →**, which opens the [setup wizard](web-setup-wizard.md).

If the hub reports a cost of 0 next to real energy, the app treats the cost as unknown and shows its own estimate (≈ energy × rate) instead of 0.00.

From v1.3.0 the block also carries a **Yesterday** line.

### Points

One tile per metered **line** (the input and the voltage module aren't tiles), the ones answering first, then by power. Each tile shows an icon guessed from the point's name, the name, its power (Money mode: ≈ cost per hour) and its voltage and current. Click a tile to open that point (see [Points](web-points.md)).

A status word appears only when something deviates: **offline**, **no such channel on the module**, **clipping**, **sensor limit**, or **sensor reversed — not metered** for a basic module wired backwards.

A ≈ before a line's power means it's an estimate: a multi-phase line measured with one current sensor is multiplied by the number of its conductors.

From v1.3.0: a device wired as several points — a heat pump on three phases, a three-phase charger — shows as **one tile with its total** instead of three, and has a page of its own for the detail. The tiles keep a stable order rather than jumping as readings change; in **Customise** you move them with ← → and mark each **Shown** or **Hidden**; and the block's own settings can order them **by name** and leave out points with no load.

### One point

A tile for **one** point that you choose in the block's settings, and you can put as many of them on a board as you like. New in v1.3.0.

### Key figures

| Tile | What it shows |
|---|---|
| **Grid input now** (or **Exporting to grid**) | The input power; "offline" when the input isn't answering. |
| **Imported today** | The day's import; "hub clock not set" when it isn't. |
| **Exported today** | Only with a solar point. |
| **Cost today** or **Net today** | Net today (cost − export value) in Money mode when you have solar and both hub totals are known. Otherwise Cost today, except in Money mode, where **Imported today** already shows the cost and no separate Cost tile is drawn. |
| **Self-sufficiency** | With solar: the share of the house load covered by solar now. |
| **Input power factor** | Without solar, when the input reports it: the value and leading/lagging. |

The export figure now has a colour of its own, so import and export read apart at a glance.

### Points table

All points including the input: **Point**, **Role**, **P** (Money mode: ≈ per hour), **Trend** (power since you opened the page), **Today** and **Status**. The input's Today matches the Today card; a solar point shows its output. Besides the tile statuses, a line can show **high · {pct}%** (above 80 % of its rating) or **above rating**.

### Tariff today

![«Tariff today»: the day's zones and the current one](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-tariff-ribbon.webp)

![«Tariff · today»: the day's zones and the current one](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-tariff-ribbon.webp)

A 24-hour strip of today's time zones from your tariff plan, with the current zone highlighted, a **now** marker on the hub's clock and a legend "zone · rate". With no time zones: "No tariff zones set." Without a connection it says so and retries.

Under a dynamic price the ribbon gives no zone advice.

### Needs attention

When all is well: **Needs attention ✓ No alarms · 12 points online**.

Otherwise a list, with the count in the header and "{n} of {m} online" at the bottom. Each row links to its point:

| Row text | Tag | Meaning |
|---|---|---|
| "current clipped by the sensor ({pct} % of rating) · under-metered" | **clipping** | The current reached the sensor's range; the real value is higher than shown. |
| "{power} — {pct} % of the line's rating" | **above rating** | The line draws more than its rated power (not checked on the input and generation). |
| "near the sensor limit ({pct} % of rating)" | **sensor limit** | The current is close to the sensor's clip point. |
| "no new data for {s} s — the last value is shown" | **held** | The hub has had no new reading for that long. |
| "sensor not responding" | **offline** | The sensor isn't responding. |

The {pct} in these rows is against the sensor's **nominal** rating. For SCT-013 clamps, **sensor limit** comes at about 104 % of the rating and **clipping** at about 116 % (for an SCT-013-050: 52 A and 58 A); readings are correct up to the clip point. The demo site flags a sensor limit earlier.

From v1.3.0 **close to its limit** starts at **80 %**, the same rule the Events page uses, and the grid input against its own limit is counted here too.

### Input against its limit

The big figure is the input power against your **input limit**, with a bar, "{pct}% of limit" and the **headroom** left. Above 92 % the bar is marked **near limit**, above 100 % **above limit**.

The limit is the smaller of two values from the input point's settings: its rated power, and what its breaker passes (amps × nominal voltage × number of conductors, 230 V if no nominal voltage is set).

- With only one of the two set, that one is the limit.
- With neither set, the block says: "Input limit is not set — set the input's rated power to see load and headroom."
- When the rated power is less than half of what the breaker passes, the block asks whether that's intended (a contracted capacity) or a single-phase figure, with a **Check** link.

Side figures:

- **Unmetered (input − lines)**: the input minus all metered lines (solar lowers it). Highlighted above 10 % of the input; unknown when any line is offline.
- **Max phase**: the most loaded phase and its share of the breaker (above 80 % "high", above 100 % "overload"). When phases are estimated from lines and more than 20 % is unmetered, it reads "at least" (≥).
- **Frequency**.

From v1.3.0 the big figure is always in kilowatts; with money showing, the hourly cost (≈ €/h) stands beside it rather than replacing it.

### Load lines

One row per line: **Line**, **Phase**, **P**, **≈ per hour** (with a tariff; first in Money mode), **I**, **Today** (output for generation, import for the others), **Load** and **Status**.

**Load** is the line's power against its rated power. The status appears only on a deviation: offline, no such channel on the module, clipping, sensor limit, **high** (above 80 % of rating) or **above limit** (above 100 %). Generation lines have no load status.

From v1.3.0 the table has a **Month to date** column, keeps a stable row order, and reads **normal** where it used to leave the status blank.

### Energy flow, the single-line view

A one-line drawing of your installation: the input (transformer and meter), the main bus, then the switchboards and their lines. Switchboards are your point **groups**; lines without a group sit under **MDB**. Solar appears as a second feed into the bus. A label like "≈ L1L2L3" marks a multi-phase line estimated from one sensor.

### Blocks for charging stations

These four appear on the standard boards when your stations make up a car park, and are in the catalogue for any board.

![A car park's Overview](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-charging-stations-view.webp)

#### Charging now

- **Charging now**: "3 of 8", plus how many are available and offline.
- **Parking load balance**: a bar of the input limit filled with the site's other loads ("Infrastructure") and one segment per station, and the **headroom** left. At 92 % of the limit or more it reads "At the limit · stations draw {pct}% of the power".

The limit comes from the input point's settings. Without it, the block asks you to set it.

RBgrid measures the power on each station's line, not the connector state, so the status is **derived from power**:

| Status | When |
|---|---|
| **Charging** | The line draws more than the charging threshold (500 W unless the hub or the station sets another value). |
| **Available** | The station answers and draws less than the threshold. |
| **Offline** | The station's sensor isn't answering. |

From v1.3.0 this one block carries both halves: the station counters against the limit, and the charging / free / no-link line. There is no separate station-status block.

#### Charging today

- In Power mode, or with no sale price: **Energy today** (all stations together) and **Charging fees today** (at the sale price, when one is set).
- In Money mode with a sale price per kWh: **Charging fees today** leads, with the energy underneath, then **Margin today** ≈ fees − the energy at your buy rate.
- **Sessions today**, estimated from the hourly history; or **Charging now** when there's no history to count from.

#### Stations

Each card: name, status, power now, energy **Today** and the station's **Limit** (its rated power). In Money mode with a sale price: ≈ fees per hour and ≈ margin per hour at the current power.

From v1.3.0 the block's settings order the blocks — as configured, by name, or charging first — and choose whether free stations are shown at all.

#### Car-park power

Hourly average power of the parking lines, with the day's **peak** and its hour, and the **limit** line. With more than 8 parking lines (or none) it shows the grid input instead, titled "· from grid input".

## Energy flow

**Energy flow** is one block with several **views**, chosen by its **Look** setting. You can put the block on a
board more than once, each copy showing a different view.

| Look | What you get |
|---|---|
| **Diagram (with the phase and scene views)** | the house diagram, and the *By phase* and *Scene* tabs inside it |
| **Flows and balance** | the *Distribution* and *Balance* views inside it |
| **Single-line diagram** | the single line, as a workshop or a car park reads it |

The views show where the power goes right now. They are built from:

- **Grid**: the input's power, signed when the input measures direction (an rbAmp input always does).
- **Solar**: the power of your generation points.
- **House**: grid + solar when direction is known; otherwise the sum of the lines; otherwise the input.
- **Consumers**: every other metered line, batteries included. Lines under a group's main meter are counted once.
- **Rest (not metered)**: house minus all metered lines, shown in grey when it's more than 5 % of the house and more than 50 W. When only the input is metered, it's always shown.

When nothing flows (everything below 5 W), the block says so and waits for a load to switch on.

### The diagram, and its phase and scene views

![The energy-flow block, Scene tab](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-scene.webp)

This Look has three views inside it; your choice is remembered in this browser:

- **Diagram** (default): Grid and Solar on top, the house in the middle, loads below, with live values. A ring shows "{pct}% from solar" when solar covers part of the house. On a narrow screen it becomes a compact list.
- **By phase**: offered when the input reports more than one phase. One column per phase with its power, current and share of the input; under each phase its single-phase lines and the rest of that phase that no line accounts for; multi-phase lines listed under **Loads across phases**. Lines without a phase set aren't placed under any phase; the block shows how many there are: "Lines without a phase: {n} — set the phase on the point page to place them under it."
- **Scene**: an illustrated house with up to five load objects; smaller loads are folded into "The rest". Line thickness shows power; the size of the objects means nothing.

If you've set up point **groups**, tabs above the diagram show **All** and one tab per group with its power. Within a group, power its main meter sees but no member line does appears as **Other in group**.

### Flows and balance

- **Distribution**: a flow chart from the sources (Solar, Grid) to each consumer and "To grid" for export. Solar is spread across loads in proportion. On a narrow screen it shows Balance instead.
- **Balance**: two bars on one kW scale, **From** (Solar, Grid) and **To** (loads and To grid).

### The single line

![A three-phase workshop's Overview](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-business-view.webp)

The **single-line diagram** draws the supply as one line with its loads hanging off it, which is how a workshop
or a car park is usually read. It is a Look of this same block — up to v1.2.x it was a block of its own, and the
old link to it still lands here.

## Phases

**Phases** is a block. On the standard boards it sits on **Details** for a house, and on **Summary** where
there are charging stations or generation. The *% of the breaker* and *cos φ* readings are in it.

![The Phases block on the Details board: per-phase load, unbalance, voltage band](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-phases.webp)

The **Phases** block appears when your mains is multi-phase (declared on the hub, or the input has reported more than one phase).

The header says where the figures come from:

- **measured at the input**: a three-phase module (U3I3) on the input measures each phase;
- **estimated from lines**: phases are added up from the lines you've assigned to a phase.

and your breaker setting ("breaker 3 × 25 A (per phase)", or "breaker rating not set").

Each phase row (L1, L2, L3) shows power (↑ for export) and current. When measured, it also shows:

- **voltage**, flagged **low** or **high** when it's more than 10 % below or above nominal;
- the **angle** against L1, for example ∠ −119.8°;
- **cos φ**, marked **lagging** or **leading**.

A bar shows the phase current against the breaker: above 80 % **high**, above 100 % **overload**.

### The checks it reports

| Check | What it says |
|---|---|
| **Phase rotation** (measured, three phases) | "Phase rotation forward (L1 → L2 → L3)." — or reversed (L1 → L3 → L2), with a warning that three-phase motors and pumps will turn backwards — or that the phases aren't 120° apart, so a phase looks missing or wired to the wrong place. |
| **Current unbalance** | The largest deviation of a phase current from the average, as a percentage; above 20 % marked **high**. Assessed only from 1 A per phase; below that the block says so, because a single lamp swings the percentage. |
| **Voltage band** | whether all phases are within 207–253 V (10 % either side of 230 V), or which phases are outside, plus the voltage unbalance (highest minus lowest, over the average). |
| **Coverage** (estimate only) | When your lines cover less than 80 % of the input, the block warns that the phase load is understated; a U3I3 module on the input gives exact phase figures. |

If a frame arrives without phase data, the rows stay and show "no data".

## Today by hour

**Today by hour** is a block. From v1.3.0 its scale follows today's own hours, so yesterday's peak no longer
flattens it, and **As a table** shows the same figures as rows.

![«Today by hour»](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-hourly.webp)

24 bars for **today on the hub's clock**: import upwards, export downwards, on one scale. In Money mode the title adds "· in money" and each hour shows its cost from the hub's hourly records (or energy × the current rate when the hub has none). **As a table** opens the same hours in [History](web-history.md).

- With a solar point (Power mode), each bar shows the house's hour split into **From grid** and **From solar** (from the first generation point).
- The **running hour** is hatched ("Current / partial hour").
- A past hour with less than an hour of records is drawn as an outline ("Partial hour").
- A cap line marks **Yesterday** at the same hour.
- Export under 10 Wh in an hour is ignored as noise.

## Insights

**Insights** is a block. From v1.3.0 it opens **collapsed**, showing its headings with their sums; press one to
open it, or set the block to open expanded. What it works out, and how, is unchanged.

![The Insights block, collapsed to its headings](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-insights.webp)

The **Insights** block turns your measurements and your tariff into short notes. It appears only when a tariff is set. Showing money or power changes its units (money: cost; power: energy).

All money figures here are **the app's estimates at your current rates**, marked ≈. The block's footnote says so: "“≈” marks an estimate at the device's current tariff rates." (Power mode: "at the current pace").

Until there's enough data it reads: "Tips will appear once enough data is collected and a tariff is set."

Notes appear in this order, each only when its condition is met:

| Insight | What the block shows (pattern) | When it appears | How it's worked out |
|---|---|---|---|
| **Surplus now** | "Surplus is going to the grid now: {power}", with the block's estimate of the per-hour difference between your buy and sell rate for that energy (≈ {money}). | You have a solar point and export more than 200 W now. | Export power × (buy rate − sell rate) for one hour. |
| **Expensive zone now** | "Expensive now: {zone} — {ratio}× the “{cheapest}” rate", with the hours of the cheapest zone and how soon it starts. | The current rate is at least 1.2× today's cheapest zone, and no surplus note is shown. | Current buy rate ÷ the cheapest rate in today's plan. |
| **Charging in a dearer zone** (not Business) | "{station} yesterday: {energy} in the “{zone}” zone", with the block's estimate of what the same energy costs at the “{cheapest}” zone's rate (≈ {money} difference). | Yesterday at least 1 kWh of EV charging fell in a zone dearer than the cheapest, and your plan has more than one rate. | Yesterday's hourly charging energy, each hour priced at its zone, compared with the cheapest rate. |
| **Where the money goes today** (Power: "Where the energy goes today") | One bar: your three biggest lines by name, then "Other points", then "Unmetered". It shows ≈ money only when every part has a cost; otherwise it shows kWh for all parts. | There's import today and more than one row to show. | Each line's energy today as a share of the input's, times today's cost. Points in one named group with the same role (for example a three-phase charger) are one share under the group's name. "Other points" = the remaining lines; "Unmetered" = input minus all lines. "Other points" and "Unmetered" are left out below 1 %. |
| **Month forecast** | "Month cost forecast ≈ {money}" — "Based on the pace of {elapsed} of {days} days; excluding standing charges, at current rates." (Power: "Month consumption forecast ≈ {energy}".) | There's month-to-date data. | The month so far, extended to the whole month at the pace so far. If the hub's data starts later than the 1st, the text says since when and that the forecast firms up over the next days. With less than one day of data: "The month forecast appears after the first full day." |
| **Today vs yesterday** | "Today costs ≈ {pct}% more/less than yesterday by this hour" (Power: "uses"), with both figures for the closed hours since midnight. | The difference is at least 10 % and yesterday's same hours are complete. | Today's closed hours compared with yesterday's same hours. No projection. |
| **Solar today** | "Solar today: produced {energy}", what the house used itself and its value at your buy price, and what was exported and its value. | A solar point produced energy today. | Self-used = produced − exported, valued at today's average buy price; export from the hub's export counter, or exported energy × sell rate. |
| **Standby use** (Business: **Base load outside working hours**) | "Standby use ≈ {power} round the clock — ≈ {energy} a month", and with a tariff its estimated monthly cost (≈ {money} a month) and its share: "({pct} % of the month's consumption)". If it rose over the last nights, the block says by how much and asks whether a new device is on standby. | The night-time floor is above 0.5 W. | See below. |

### How the standby figure is worked out

The app looks at the quiet hours of the night, **01:00–05:00** on the hub's clock:

1. It takes the input's hourly records of the last 8 days. Hourly power of EV-charging and heating/cooling lines is taken out first, since they run on their own schedule; the block names them under "Not counted".
2. For each night it takes the middle value (median) of those hours. A night is skipped if it has export, fewer than 2 hours, or an unusual spike.
3. The standby figure is the median over at least 3 nights. When the hub already keeps its own night-time base figure over 3 nights or more, the app uses that.
4. Energy per month = standby power × 730 hours. The monthly cost uses your plan's **week-average buy rate**.
5. "It rose" compares the last 3 nights with the earlier ones (needs 5 nights).

Before 3 nights are in, the block reads "Standby use: collecting data — {n} of 3 nights". With no usable night yet, it shows "Standby use: find devices on standby". The **Find vampires** button is there in all these states; from v1.3.0 it sits inside the **Standby use** heading, so open that heading to reach it.

### Find vampires

![«Find vampires»: measure one device's standby draw](https://www.rbgrid.io/assets/img/webapp-docs/wa-dashboard-find-vampires.webp)

**Find vampires** opens a short guided test that finds devices on standby from the steps they leave in the live input power:

1. Press **Start** and leave everything as it is. The app measures the background for 30 seconds.
2. Switch off or unplug **one** device and press **Switched off** right away. The app tells you the smallest step it can tell from noise at the moment.
3. After a 3-second pause it measures for another 30 seconds. Don't switch anything else.

The result is the difference between the two 30-second medians, for example "−4.2 W (L2)" (the phase is named when one phase carries 70 % of the step or more), with "Round the clock that is ≈ {energy} a month · ≈ {money}/mo" at your week-average rate. Give it a name and press **Save**, then **Next device**.

Other outcomes: the power rose (something switched on — try again); the step is below what the meter can tell from noise; or too few readings came in (the connection dropped).

- A step counts when it's at least twice the input's resolution and three times the measured noise.
- The test can't run while the input isn't answering. While you export, the app suggests hunting in the evening or at night, because the sun moves the power more than a standby device draws.
- The **Found** list, with its total, is kept **in this browser only**, not on the hub.

## Banners and alerts

Banners appear at the top of the Overview, in this order:

| Banner | When it appears |
|---|---|
| **data delayed** (chip) | Live frames from the hub arrived with gaps. |
| *No connection to the device — showing data from {time}.* | The hub doesn't answer; figures dim and show the last data. |
| *The device withholds live data without a read token…* | The hub needs a read token; enter it in **Settings → Security**. |
| *Restart the hub to apply …*, with **Restart now** | A saved change needs a restart. From v1.3.0 this line stands at the top of both the Overview and Settings. |
| Hub access warning | Hub firmware v1.1.2 and later: no API token is set, or the hub's own Wi-Fi still uses its setup password, or both. Without a token the hub doesn't report the access-point status, so the warning stays until both are set. Buttons **Access-point password** and **API token**; **Hide** hides it for this browser session. |
| Hub time note | The hub's time zone differs from your browser's by 30 minutes or more. If the hub is where you are, set its time zone ([Time](web-time.md)); otherwise days, hours and tariff zones are shifted. |
| *Sensors not responding: {n} of {m}. Their readings are not counted.* | At least one point isn't answering. **What to check** opens the Events page. |

From v1.3.0 the factory-settings banner is gone: its job belongs to the **Left to set up** check-list, which
names each remaining step and leads you to it. The hub time note never shows in the demo. While the connection
is lost, the hub access warning, the hub time note and the "Sensors not responding" strip are hidden.

Inside the blocks you may also see: the reversed-sensor and phase-sign checks (**Now**), the example-tariff and clock notes (**Today**), and the limit-below-breaker question (**Input against its limit**).

When a point stops answering, the blocks show it in place: "held" on **Now**, "offline" on tiles, "—" in tables, greyed flow diagrams.

With no points at all, the Overview shows "No metering points yet" with an **Add sensor** button that opens the setup wizard.


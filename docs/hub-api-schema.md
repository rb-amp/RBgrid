# Hub API reference (firmware 1.3.0, API v1)

Generated from the hub's own `GET /api/v1/schema`. Do not edit by hand.

## Endpoints

**Admin for writes** = writes on this route need the admin token once one is set. This schema does not say which reads need the read token; see the token table in the API overview.

| Path | Methods | Admin for writes | Restart to apply | Parameters |
|---|---|---|---|---|
| `/api/system/ap` | GET,POST | yes | no | GET: AP ssid/password (admin token, armed hub only); POST {password}: the user sets the AP password ("" = setup password) |
| `/api/system/backup` | GET,POST | yes | no |  |
| `/api/system/factory-reset` | POST | yes | yes | history(erase\|keep) |
| `/api/system/hardware-config` | GET,POST | yes | yes |  |
| `/api/system/lang` | GET,POST | yes | no | code (query, ISO 639-1); body=language pack JSON ≤ 200 KB; one pack kept; served at /lang/<code>.json |
| `/api/system/mqtt-ca` | GET,POST,DELETE | yes | yes | POST body = PEM (<= 8192 B, one or more certificates); DELETE = public CA bundle |
| `/api/system/mqtt-config` | GET,POST | yes | yes | broker_uri |
| `/api/system/ota` | POST | yes | yes | (binary) |
| `/api/system/provisioning` | GET | no | no |  |
| `/api/system/reboot` | POST | yes | no | delay_ms |
| `/api/system/restore` | POST | yes | yes |  |
| `/api/system/restore-system-config` | POST | yes | no | body = system-config JSON |
| `/api/system/security` | GET,POST | yes | no | dev_cors,origins |
| `/api/system/system-config` | GET,POST | yes | no |  |
| `/api/system/tariff-plan-config` | GET,POST | yes | no |  |
| `/api/system/telemetry-config` | GET,POST | yes | no | profile(lean\|full) |
| `/api/system/time-config` | GET | no | no | read-only time settings view |
| `/api/system/time-sync` | GET,POST | yes | yes |  |
| `/api/system/token` | GET,POST | yes | no | token,role(admin\|read) |
| `/api/system/ui-commit` | POST | yes | no | force (query); body=ui-manifest.json |
| `/api/system/ui-state` | GET,PUT,DELETE | yes | no | the app's dashboard layout, stored as is: GET → document + X-UI-State-Rev (404 {rev:0} when none); PUT ?rev=<n> body=JSON ≤ limits.ui_state_bytes → {rev:n+1}, 409 {rev} on a stale rev (0 = none expected); DELETE → {rev:0} |
| `/api/system/ui-upload` | POST | yes | no | path (query); body=raw file |
| `/api/system/units-config` | GET,POST | yes | no |  |
| `/api/system/version` | GET | no | no |  |
| `/api/system/wifi-config` | GET | no | no | read-only Wi-Fi settings view |
| `/api/telemetry/latest` | GET | no | no |  |
| `/api/v1/dynprice` | GET,POST | yes | no | dynamic price config, providers, live state |
| `/api/v1/economics` | GET | no | no | unit |
| `/api/v1/espnow` | GET,POST | yes | no | action(pair\|add\|remove\|disable\|enable\|latch),mac,seconds,channels |
| `/api/v1/export.csv` | GET | no | no | unit,period,type,from,to |
| `/api/v1/history` | GET | no | no | unit_id,period,type(electrical\|energy\|economics\|all),offset,limit,from,to,since |
| `/api/v1/i2c` | GET | no | no | buses, board pin profile, modules |
| `/api/v1/i2c/buses/{b}` | POST | yes | yes | sda,scl \| off (board pin profile checked; applied after a restart: reboot_pending) |
| `/api/v1/i2c/modules` | POST | yes | no | bus,addr |
| `/api/v1/i2c/modules/{m}/address` | POST | yes | no | addr |
| `/api/v1/i2c/modules/{m}/bus` | POST | yes | no | bus |
| `/api/v1/i2c/modules/{m}/drdy` | POST | yes | no | gpio |
| `/api/v1/i2c/scan` | POST | yes | no | bus |
| `/api/v1/led` | GET,POST | yes | no | status LEDs: GET what they show; POST mode(full\|quiet\|off), identify(seconds\|true\|false), ack(true) |
| `/api/v1/llm` | GET,POST | yes | no | AI / LLM page: enabled, read_only, pairing codes, tokens, call log, connect \| revoke, pair_cancel |
| `/api/v1/llm/call` | POST | yes | no | {tool,args}: an LLM tool run by the hub page (caller web) |
| `/api/v1/peaks` | GET,POST | yes | no | cycle_day(1..28) \| reset |
| `/api/v1/price` | GET,POST | yes | no | price push (HA source) / resolved prices / log |
| `/api/v1/rules` | GET,POST | yes | no | cheap windows (threshold\|cheapest\|deadline) |
| `/api/v1/schema` | GET | no | no | this description |
| `/api/v1/sensors` | GET | no | no |  |
| `/api/v1/signal` | GET | no | no | price_signal v1 |
| `/api/v1/summary` | GET | no | no | unit,period,from,to |
| `/api/v1/system/events` | GET | no | no | since,limit(<=1000),level(all\|event\|fail) |
| `/api/v1/units` | POST | yes | no | sensor,name,role,stat_class,point,reuse |
| `/api/v1/units/{hex}` | POST,DELETE | yes | no | name,short_name,phase,rated_power_w,max_current_a,role,group_id,role_trunk,invert_sign,charge_threshold_w,idle_timeout_min,ct_code (POST=params patch; invert_sign + ct_code apply live; ct_code = enums.ct_model, rbAmp current channels; EV overrides 0 = system.ev) |
| `/api/v1/units/{hex}/rebind` | POST | yes | no | module,ch |
| `/api/v1/units/{hex}/rename` | POST | yes | no | point,reuse |
| `/api/v1/ws-ticket` | POST | no | no | → {ticket,expires_s}; open /ws-telemetry?ticket=<ticket> once within expires_s |

## Transports

- **ws:** `/ws-telemetry (push + get/start/stop_telemetry)`
- **mqtt:** `rbgrid/<mac>/{telemetry,status,cmd,cmd/resp} + HA discovery`

## Quality bits (`q`)

| Bit | Name |
|---|---|
| 0 | `PARTIAL` |
| 1 | `TIME_UNSYNC` |
| 2 | `CLOCK_STEP` |
| 3 | `SRC_GAP` |
| 4 | `RECOMPUTED` |
| 5 | `ESTIMATED` |
| 6 | `UNSIGNED_SRC` |
| 7 | `OVERRANGE` |
| 8 | `MIGRATED` |
| 9 | `SRC_RESTART` |
| 10 | `SRC_FW` |
| 11 | `REVERSE_LOST` |
| 12 | `OUTAGE` |

## Capabilities

| Capability | This hub |
|---|---|
| `espnow` | true |
| `pointNesting` | true |
| `dynamicTariffs` | true |
| `economics` | true |
| `summary` | true |
| `exportCsv` | true |
| `telemetryConfigPost` | true |
| `llm` | true |
| `mqttConfig` | true |
| `adcUnboundDiscovery` | false |
| `historyTimeRange` | true |
| `auth` | true |
| `https` | true |
| `provisioning` | true |
| `configBackup` | true |
| `inputAggregate` | true |
| `wsTicket` | true |

## Hub fields

Top-level fields of this schema (also in `GET /api/system/version` for the storage ones). `stats_media`: `external` = history on the statistics chip, `internal` = in the hub's own flash, `none` = statistics kept in memory only. `history_slots`: how many points on this hub can keep history.

| Field | This hub |
|---|---|
| `storage_state` | "ready" |
| `stats_media` | "external" |
| `history_slots` | 49 |
| `history_schema_ver` | 4 |
| `telemetry_schema` | 1 |

## Units and money

- `energy`: Wh
- `power`: W
- `voltage`: V
- `current`: A
- `frequency`: Hz
- `money`: major
- `rate`: major_per_kwh
- money decimals: 2, rate decimals: 4 (currency = the tariff plan's; this hub: EUR)
- point keys: `HEX4` (four hex digits)

## Enumerations

- **role:** `MAIN_SUPPLY`, `ALTERNATIVE_SUPPLY`, `CIRCUIT_BREAKER`, `DEVICE_LOAD`, `ZONE_LOAD`, `RENEWABLE_SOURCE`, `STORAGE_SYSTEM`, `EV_CHARGING`, `HEATING_COOLING`, `SMART_APPLIANCE`
- **site_profile:** `home`, `industrial`, `ev`
- **ct_model:** `1` = SCT-013-005, 5 A, `2` = SCT-013-010, 10 A, `3` = SCT-013-030, 30 A, `4` = SCT-013-050, 50 A, `5` = SCT-013-100, 100 A, `6` = SCT-013-020, 20 A, `7` = SCT-013-060, 60 A
- **ct_model_ct:** `1` = Toroid 1:1000, 196 R burden, `2` = Toroid 1:2000, 370 R burden, `3` = Toroid 1:3000, 572 R burden, `4` = CT 2000:1, 92 R burden, `5` = Clamp 2000:1, 92 R burden, `6` = Clamp 2000:1, 1 A, `7` = Clamp 2000:1, 5 A, `8` = Clamp 2000:1, 10 A, `9` = Clamp 2000:1, 20 A, `10` = Clamp 2000:1, 30 A, `12` = Clamp 2000:1, 80 A
- **phase:** `0` = UNSET, `1` = L1, `2` = L2, `3` = L3, `4` = 3P, `5` = L1L2, `6` = L2L3, `7` = L3L1
- **grid_system:** `1P`, `SPLIT`, `3P`
- **grid_system_validated:** `1P`, `3P`
- **tariff_zone:** `NIGHT`, `MORNING_PEAK`, `DAY`, `EVENING_PEAK`, `LATE_EVENING`, `WEEKEND`, `HOLIDAY`, `CRITICAL_PEAK`
- **tariff_plan_type:** `SIMPLE`, `TWO_ZONE`, `THREE_ZONE`, `MULTI_ZONE`, `PROGRESSIVE`, `SEASONAL`, `DYNAMIC`, `CUSTOM`
- **tariff_condition_type:** `TIME_RANGE`, `WEEKDAY`, `WEEKEND`, `HOLIDAY`, `SEASON`, `MONTH`, `DATE_RANGE`, `SPECIAL_EVENT`

## Limits

- `short_name_bytes`: 24
- `ui_state_bytes`: 8192
- `name_bytes`: 31
- `origins_max`: 8

| Setting | Section | Min | Max |
|---|---|---|---|
| `logging_level` | system | 0 | 3 |
| `max_retry_attempts` | system | 0 | 255 |
| `retry_delay_ms` | system | 0 | 600000 |
| `ev.charge_threshold_w` | system | 0 | 100000 |
| `ev.idle_timeout_min` | system | 0 | 1440 |
| `ev.price_per_kwh` | system | 0 | 1000 |
| `adc.channels_count` | hardware | 0 | 8 |
| `adc.sampling_frequency` | hardware | 1000 | 50000 |
| `adc.adc_resolution` | hardware | 9 | 12 |
| `adc.adc_attenuation` | hardware | 0 | 3 |
| `network.nominal_voltage` | hardware | 90 | 480 |
| `network.nominal_frequency` | hardware | 45 | 65 |
| `grid.nominal_v` | system | 90 | 480 |
| `grid.nominal_hz` | system | 45 | 65 |
| `json_buffer_size` | telemetry | 1024 | 65536 |

## Live fields by source

- **rbamp:** `v`, `i`, `p_act`, `v_min`, `v_max`, `i_min`, `i_max`, `p_min`, `p_max`
- **espnow:** `v`, `i`, `p_act`

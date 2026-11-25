/**
 * I2C fleet API (`main/rbi2c_api.c`, in the firmware since v1.1.0, frozen). The app uses add /
 * readdress on bus 0, and — on hub 1.3.0 (root i2c-bus1-support 22.11) — bus 1 too, with its pins
 * (POST /api/v1/i2c/buses/1, applied after a restart). Per-module DRDY and moving a module between
 * buses stay out of the UI.
 */

/** GET /api/v1/i2c → modules[]: one configured rbAmp module. */
export interface I2cModule {
  /** Fleet index (the `module` of /api/v1/sensors). */
  module: number;
  bus: number;
  /** 7-bit address, e.g. 80 = 0x50. */
  addr: number;
  present: boolean;
  model?: string;
  n_i?: number;
  /** Channels bound to a point. */
  points?: number;
  /** v1.1.4: absent and no metering point depends on it (at most the mains-voltage point) — an empty
   *  slot, typically the product-default module 0 @0x50. Absent on older hubs. */
  placeholder?: boolean;
}

export interface I2cBus { bus: number; up: boolean; configured?: boolean; sda?: number; scl?: number; port?: number }

/** A GPIO the board profile keeps for something else (`use`: «boot strap», «I2C bus 0», «USB D-»…). */
export interface I2cReservedPin { gpio: number; use: string }

/** GET /api/v1/i2c (and the body of a successful write, with `status:"ok"`). */
export interface I2cState {
  buses: I2cBus[];
  modules: I2cModule[];
  max_modules?: number;
  /** New bus pins saved, applied at the next restart. */
  reboot_pending?: boolean;
  /** The board profile: GPIOs a bus may use, and the ones kept for something else. */
  pins_ok?: number[];
  pins_reserved?: I2cReservedPin[];
  status?: string;
}

/** POST /api/v1/i2c/scan → devices[]: one address that answered. */
export interface I2cDevice {
  addr: number;
  /** Fleet index when the hub already has a module there, else null. */
  module: number | null;
  /** rbamp = an rbAmp module; rbdimmer / unknown = something else at 0x50–0x5F; device = outside that range. */
  kind: 'rbamp' | 'rbdimmer' | 'unknown' | 'device';
  fw?: string;
  uid?: string;
  /** v1.1.2 scan: product line «basic» / «std» and model, e.g. «BASIC-UI3», «STD-U3I3». */
  line?: string;
  model?: string;
}

export type I2cScanResult =
  | { status: 'ok'; devices: I2cDevice[] }
  | { status: 'error'; code: string; message: string; http?: number };

export type I2cWriteResult =
  | ({ status: 'ok' } & I2cState)
  | { status: 'error'; code: string; message: string; http?: number };

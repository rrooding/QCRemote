// StoreController at its own seam: what a component can do through it.
// The host and store are stand-ins - the controller only forwards.
//
//   node --test web/ui/test/

import { test } from 'node:test';
import assert from 'node:assert/strict';

import { StoreController } from '../src/store-controller.js';

const fakeHost = () => ({ addController() {}, requestUpdate() {} });

test('takeOver() asks the store to take over', () => {
  let takeOvers = 0;
  const controller = new StoreController(fakeHost(), { state: {}, takeOver: () => (takeOvers += 1) });

  controller.takeOver();

  assert.equal(takeOvers, 1);
});

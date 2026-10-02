/* Browser contract tests for the embedded, offline client.
 * Run: NODE_PATH=<installed playwright modules> node --test tests/web/test_client.cjs
 * Uses a real Chromium DOM with a deterministic device WebSocket, no firmware or network.
 */
'use strict';
const { test, before, after } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { chromium } = require('playwright');
const html = fs.readFileSync(path.resolve(__dirname, '../../main/web/index.html'), 'utf8');
let browser;
before(async () => { browser = await chromium.launch({ headless: true }); });
after(async () => { if (browser) await browser.close(); });

async function fixture(t, options = {}) {
  const context = await browser.newContext({ viewport: options.mobile ? { width: 390, height: 844 } : { width: 1280, height: 900 } });
  const page = await context.newPage();
  if (options.clock) await page.clock.install();
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/*', route => route.fulfill({ status: 200, contentType: 'text/html; charset=utf-8', body: html }));
  await page.addInitScript(({ storedToken, rejectClaim }) => {
    if (storedToken) sessionStorage.setItem('nmea-token', storedToken);
    const base = {
      type: 'state', mode: 'idle', baud: 4800, can_speed: 250000,
      groups: [false, false, false, false, false], nav: {}, version: 0, theme: 0,
      rotated: false, internal_free: 120000, internal_min: 90000, psram_free: 4000000,
      wifi_mode: 0, ip: '192.168.4.1'
    };
    const device = window.__device = {
      frames: [], sockets: [], autoAck: true, rejectClaim, failures: {}, base,
      token: '0123456789abcdef0123456789abcdef',
      current() { return this.sockets[this.sockets.length - 1]; },
      emit(data) { this.current().onmessage?.({ data: JSON.stringify(data) }); },
      state(extra) { this.base = { ...this.base, ...extra }; this.emit(this.base); },
      close() { this.current().close(); }
    };
    window.WebSocket = class {
      constructor(url) {
        this.url = url; this.readyState = 0;
        device.sockets.push(this);
        setTimeout(() => { if (device.offline) { this.readyState = 3; this.onclose?.(); } else { this.readyState = 1; this.onopen?.(); } }, 0);
      }
      send(raw) {
        if (this.readyState !== 1) throw new Error('Socket is not open');
        const message = JSON.parse(raw);
        device.frames.push(message);
        queueMicrotask(() => {
          if (this.readyState !== 1) return;
          const emit = data => this.onmessage?.({ data: JSON.stringify(data) });
          if (message.op === 'claim') {
            if (device.rejectClaim) return emit({ type: 'ack', id: 0, ok: false, error: 'ESP_ERR_INVALID_STATE' });
            emit({ type: 'claimed', token: device.token, next_id: 7, timeout_ms: 60000 });
            emit(device.base);
          } else if (message.op === 'release') {
            emit({ type: 'released', reason: 'released' });
          } else if ((device.autoAck || message.op === 'heartbeat') && !['events_ack', 'terminal_ack'].includes(message.op)) {
            emit({ type: 'ack', id: message.id, ok: !device.failures[message.op], error: device.failures[message.op] || 'ESP_OK' });
          }
        });
      }
      close() {
        if (this.readyState === 3) return;
        this.readyState = 3;
        queueMicrotask(() => this.onclose?.());
      }
    };
  }, options);
  await page.goto('http://nmea.test/');
  t.after(async () => { assert.deepEqual(errors, [], 'No uncaught browser errors'); await context.close(); });
  return page;
}
async function connect(page) {
  await page.locator('#connect').click();
  await page.waitForFunction(() => !document.getElementById('controls').disabled);
}
async function emit(page, data) { await page.evaluate(data => window.__device.emit(data), data); }
async function state(page, data) { await page.evaluate(data => window.__device.state(data), data); }
async function frames(page, op) { return page.evaluate(op => window.__device.frames.filter(f => f.op === op), op); }
async function last(page, op) { const matches = await frames(page, op); assert(matches.length, `Expected ${op} request`); return matches.at(-1); }
async function settings(page) {
  await page.locator('[data-tab="settings"]').click();
  assert(await page.locator('#settings').isVisible());
}
async function selectMode(page, mode) {
  await page.locator(`[data-mode="${mode}"]`).click();
  await state(page, { mode });
  assert(await page.locator(`[data-mode="${mode}"]`).evaluate(node => node.classList.contains('active')));
}
async function receiverView(page, view) {
  await page.locator(`[data-view="${view}"]`).click();
}
async function openTemplates(page) {
  await page.locator('#openTemplates').click();
  assert(await page.locator('#templateDialog').isVisible());
}

// Includes disabled fieldset behaviour on a real browser, not a fake DOM.
test('claim, mode workspaces, repeated selection, Settings and explicit release', async t => {
  const page = await fixture(t);
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  assert.equal(await page.locator('#mode, #startMode').count(), 0);
  await connect(page);
  assert.equal((await last(page, 'claim')).token, '');
  assert.equal(await page.evaluate(() => sessionStorage.getItem('nmea-token')), '0123456789abcdef0123456789abcdef');
  for (const mode of ['rx485', 'tx485', 'rs485_bridge', 'n2k', 'sailor']) {
    await selectMode(page, mode);
    assert.equal((await last(page, 'mode')).mode, mode);
    const count = (await frames(page, 'mode')).length;
    await page.locator(`[data-mode="${mode}"]`).click();
    assert.equal((await frames(page, 'mode')).length, count, 'Repeated active selection must not restart hardware');
    assert(await page.locator(`[data-mode="${mode}"]`).evaluate(node => node.classList.contains('running')));
  }
  const runningLabel = await page.locator('#currentMode').textContent();
  const count = (await frames(page, 'mode')).length;
  await settings(page);
  assert.equal((await frames(page, 'mode')).length, count, 'Settings is a presentation change');
  assert.equal(await page.locator('#currentMode').textContent(), runningLabel);
  await state(page, { mode: 'sailor', terminal_ready: true });
  assert(await page.locator('#settings').isVisible(), 'State refresh must not leave Settings');
  await selectMode(page, 'tx485');
  await page.locator('#baud').selectOption('38400');
  assert.equal((await last(page, 'baud')).value, 38400);
  await page.locator('#stopMode').click();
  assert.equal((await last(page, 'mode')).mode, 'idle');
  await state(page, { mode: 'idle' });
  await page.locator('#startInstrument').click();
  assert.equal((await last(page, 'mode')).mode, 'tx485', 'Start uses the stopped selected instrument');
  await page.locator('#release').click();
  assert.equal((await last(page, 'release')).token, '0123456789abcdef0123456789abcdef');
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  assert.equal(await page.evaluate(() => sessionStorage.getItem('nmea-token')), null);
});

test('navigation validity, bounded event rendering, receipts, replay suppression and RX HEX', async t => {
  const page = await fixture(t);
  await connect(page);
  await state(page, { mode: 'rx485', nav: { valid_fields: 1 | 8 | 64, stale_fields: 64, lat: 12.205, lat_dir: 'N', lon: 45.913333333, lon_dir: 'E', gps_sog: 5.5, gps_cog: 80, depth: 9.2 } });
  assert.match(await page.locator('#metrics').textContent(), /12° 12\.300′ N/);
  assert.match(await page.locator('#metrics').textContent(), /45° 54\.800′ E/);
  assert.match(await page.locator('#metrics').textContent(), /stale/i);
  const batch = { type: 'events', mode: 'rx485', cursor: 3, dropped: 2, items: [{ seq: 1, ms: 1000, kind: 1, text: '$GPHDT,12.3,T\r\n' }, { seq: 2, ms: 1200, kind: 1, text: '<img src=x onerror=alert(1)>' }] };
  await emit(page, batch); await emit(page, batch);
  assert.equal((await page.locator('#events').textContent()).split('\n').length, 2);
  assert.equal(await page.locator('#events img').count(), 0);
  assert.equal((await last(page, 'events_ack')).cursor, 3);
  assert.match(await page.locator('#drops').textContent(), /2/);
  await page.locator('#hex').check();
  assert.equal((await last(page, 'hex')).value, true);
  await page.locator('#pause').check();
  assert.equal((await last(page, 'pause')).value, true);
  const items = Array.from({ length: 350 }, (_, i) => ({ seq: i + 3, ms: 1300 + i, kind: 1, text: `line-${i}` }));
  await emit(page, { type: 'events', mode: 'rx485', cursor: 353, items });
  assert.equal((await page.locator('#events').textContent()).split('\n').length, 300);
  await page.locator('#clearLog').click(); assert.equal(await page.locator('#events').textContent(), '');
  await state(page, { mode: 'rs485_bridge' });
  await emit(page, { type: 'events', mode: 'rs485_bridge', cursor: 2, items: [{ seq: 1, ms: 2500, kind: 4, hex: '0041ff' }] });
  assert.match(await page.locator('#events').textContent(), /RX 00 41 ff/);
});

test('Receiver HEX filters confirmed-format history without losing navigation, receipts or replay protection', async t => {
  const page = await fixture(t); await connect(page);
  await state(page, { mode: 'rx485', hex: false, nav: { valid_fields: 8, stale_fields: 0, gps_sog: 5.5, gps_cog: 80 } });
  const nmea = ['$GPHDT,12.3,T', '$GPGLL,1212.300,N,04554.800,E', '$GPHDT,24.6,T', '$GPHDT,36.9,T'];
  const hex = ['24 47 50 48 44 54 2C 31 32', '24 47 50 47 4C 4C 2C 34 33', '24 47 50 48 44 54 2C 32 34', '24 47 50 48 44 54 2C 33 36'];
  const visible = () => page.locator('#events .event-rx').allTextContents();
  const initial = { type: 'events', mode: 'rx485', cursor: 5, items: [
    { seq: 1, ms: 1000, kind: 1, text: nmea[0] },
    { seq: 2, ms: 1001, kind: 2, text: hex[0] },
    { seq: 3, ms: 1200, kind: 1, text: nmea[1] },
    { seq: 4, ms: 1201, kind: 2, text: hex[1] }
  ] };
  await emit(page, initial); await emit(page, initial);
  assert.deepEqual(await visible(), nmea.slice(0, 2), 'NMEA view excludes HEX and suppresses replay');
  assert.equal((await page.locator('#events').textContent()).split('\n').length, 2, 'Filtering adds no empty lines');

  await page.locator('#hex').check();
  assert.equal((await last(page, 'hex')).value, true);
  assert.deepEqual(await visible(), nmea.slice(0, 2), 'Command ACK alone must not replace the confirmed format');
  await state(page, { hex: true, nav: { valid_fields: 8, stale_fields: 0, gps_sog: 7.4, gps_cog: 81 } });
  assert.deepEqual(await visible(), hex.slice(0, 2), 'Confirmed HEX immediately filters the retained history');
  assert.match(await page.locator('#metrics').textContent(), /7\.4 kn \/ 81\.0°/, 'Navigation still updates in HEX');

  const hidden = { type: 'events', mode: 'rx485', cursor: 6, items: [{ seq: 5, ms: 1400, kind: 1, text: nmea[2] }] };
  await emit(page, hidden); await emit(page, hidden);
  assert.deepEqual(await visible(), hex.slice(0, 2), 'A batch containing only filtered events does not alter HEX output');
  assert.equal((await last(page, 'events_ack')).cursor, 6, 'Filtered events still receive transport acknowledgement');
  const mixed = { type: 'events', mode: 'rx485', cursor: 9, items: [
    { seq: 6, ms: 1401, kind: 2, text: hex[2] },
    { seq: 7, ms: 1600, kind: 1, text: nmea[3] },
    { seq: 8, ms: 1601, kind: 2, text: hex[3] }
  ] };
  await emit(page, mixed);
  assert.deepEqual(await visible(), hex);
  await page.locator('#hex').uncheck();
  assert.equal((await last(page, 'hex')).value, false);
  assert.deepEqual(await visible(), hex, 'Switching back also waits for the confirmed state');
  await state(page, { hex: false });
  assert.deepEqual(await visible(), nmea, 'Returning to NMEA restores each retained sentence once, without HEX');
  await emit(page, mixed);
  assert.deepEqual(await visible(), nmea, 'Replay suppression also includes events hidden in the earlier format');
  assert.equal((await page.locator('#events').textContent()).split('\n').length, 4);
  assert.deepEqual((await frames(page, 'events_ack')).map(frame => ({ mode: frame.mode, cursor: frame.cursor })),
    [5, 5, 6, 6, 9, 9].map(cursor => ({ mode: 'rx485', cursor })),
    'Every complete batch is acknowledged independently of display filtering and replay');
});

test('generator groups, manual NMEA and typed template delta preserve dirty edits', async t => {
  const page = await fixture(t); await connect(page);
  await selectMode(page, 'tx485');
  await page.locator('#active-gps').check();
  assert.deepEqual({ group: (await last(page, 'group')).group, value: (await last(page, 'group')).value }, { group: 'gps', value: true });
  await page.locator('#manual').fill('$GPHDT,120.0,T'); await page.locator('#manualForm button').click();
  assert.equal((await last(page, 'send')).text, '$GPHDT,120.0,T');
  await page.locator('#groups button').first().click();
  assert(await page.locator('#templateDialog').isVisible());
  assert.equal((await last(page, 'template_get')).group, 'gps');
  const template = { type: 'template', group: 'gps', fields: [
    { name: 'sog_kn', kind: 'n', min: 0, max: 100 },
    { name: 'send_rmc', kind: 'b' }, { name: 'lat_dir', kind: 'c', options: 'NS' },
    { name: 'talker_id', kind: 's', size: 3 }
  ], values: { sog_kn: 5, send_rmc: false, lat_dir: 'N', talker_id: 'GP' } };
  await emit(page, template);
  await page.locator('#field-sog_kn').fill('7.25');
  await page.locator('#field-send_rmc').check();
  await emit(page, { ...template, values: { ...template.values, sog_kn: 99 } });
  assert.equal(await page.locator('#field-sog_kn').inputValue(), '7.25');
  await page.locator('#editor button[type=submit]').click();
  assert.deepEqual((await last(page, 'template_patch')).values, { sog_kn: 7.25, send_rmc: true });
  await page.locator('#saveTemplates').click(); assert(await last(page, 'save'));
});

test('AIS/N2K tables and settings render untrusted strings only as text', async t => {
  const page = await fixture(t, { mobile: true }); await connect(page);
  const hostile = '<img src=x onerror="window.__xss=true">';
  await selectMode(page, 'rx485');
  const modeCount = (await frames(page, 'mode')).length;
  await receiverView(page, 'ais');
  assert.equal((await frames(page, 'mode')).length, modeCount, 'AIS is a Receiver presentation view');
  await emit(page, { type: 'ais', total: 25, offset: 0, targets: [{ mmsi: 123456789, name: hostile, call: 'TEST', lat: 12, lon: 45, sog: 4, cog: 20, heading: 511, status: 0, age_ms: 2500 }] });
  assert.match(await page.locator('#aisRows').textContent(), /<img/);
  assert.equal(await page.locator('#aisRows img').count(), 0);
  await page.locator('#aisNext').click(); assert.equal((await last(page, 'ais_get')).offset, 24);
  await emit(page, { type: 'ais', total: 25, offset: 24, targets: [] });
  await page.locator('#aisPrev').click(); assert.equal((await last(page, 'ais_get')).offset, 0);
  await selectMode(page, 'n2k');
  await emit(page, { type: 'n2k', rx_count: 15, dropped: 1, nodes: [{ sa: 7, name: '0000000000001234', model: hostile, serial: '1', pgn: 129025, count: 15, age_ms: 900 }], traffic: [hostile] });
  assert.equal(await page.locator('#n2kRows img, #canLog img').count(), 0);
  await page.locator('#canSpeed').selectOption('500000'); assert.equal((await last(page, 'can_speed')).value, 500000);
  await settings(page);
  await emit(page, { type: 'settings', ap_ssid: hostile, ap_ip: '192.168.4.1', sta_ssid: 'old' });
  assert.equal(await page.locator('#apSsid').inputValue(), hostile);
  await page.locator('#staSsid').fill('user-edit');
  await emit(page, { type: 'settings', ap_ssid: 'AP', ap_ip: '192.168.4.1', sta_ssid: 'overwrite' });
  assert.equal(await page.locator('#staSsid').inputValue(), 'user-edit');
  await emit(page, { type: 'scan', items: [{ ssid: hostile, rssi: -30, secure: true }] });
  assert.equal(await page.locator('#networks img').count(), 0);
  assert.equal(await page.evaluate(() => window.__xss), undefined);
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
});

test('settings and Wi-Fi forms preserve passwords unless explicitly replaced', async t => {
  const page = await fixture(t); await connect(page); await settings(page);
  await page.locator('#version').selectOption('4'); await page.locator('#theme').selectOption('2'); await page.locator('#rotated').check();
  await page.locator('#settingsForm button').click();
  const config = await last(page, 'settings'); assert.equal(config.version, 4); assert.equal(config.theme, 2); assert.equal(config.rotated, true);
  await page.locator('#apSsid').fill('NMEA'); await page.locator('#apIp').fill('192.168.4.1');
  await page.locator('#apForm button').click(); assert.equal(Object.hasOwn(await last(page, 'wifi_ap'), 'password'), false);
  await page.locator('#apOpen').check(); await page.locator('#apForm button').click(); assert.equal((await last(page, 'wifi_ap')).password, '');
  await page.locator('#staSsid').fill('Bridge'); await page.locator('#staPass').fill('correcthorse');
  await page.locator('#staForm button[type=submit]').click(); assert.equal((await last(page, 'wifi_sta')).password, 'correcthorse');
  for (const [id, op] of [['wifiAP', 'wifi_mode'], ['wifiReconnect', 'wifi_connect'], ['wifiDisconnect', 'wifi_disconnect'], ['wifiOff', 'wifi_off'], ['scan', 'wifi_scan']]) { await page.locator('#' + id).click(); assert(await last(page, op)); }
});

// Synthetic bench identity and position; no installation data is embedded.
const freshAntenna = {
  online: true, identity_valid: true, signal_valid: true, position_valid: true, position_fresh: true,
  serial: '12340001', cn0_dbhz: 42, signal_bars: 5,
  latitude: 12 + 34.567 / 60, longitude: 45 + 6.789 / 60, position_utc: 1790740800,
  signal_age_ms: 100, position_age_ms: 200,
  ocean: 'Pacific', ocean_fresh: true, registration: 'Logged in', registration_fresh: true,
  protocol: 'Free', protocol_fresh: true, channel: '12580', channel_fresh: true
};

test('SAILOR network fields distinguish waiting, current, stale and offline values without commands', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  const names = ['Ocean', 'Registration', 'Protocol', 'Channel'];
  for (const name of names) {
    assert.equal(await page.locator('#antenna' + name).textContent(), '—');
    assert.equal(await page.locator('#antenna' + name + 'Status').textContent(), 'Waiting');
  }
  const sentBefore = await page.evaluate(() => window.__device.frames.filter(f => f.op !== 'heartbeat'));
  await state(page, { antenna: freshAntenna });
  for (const name of names) {
    assert.equal(await page.locator('#antenna' + name).textContent(), freshAntenna[name.toLowerCase()]);
    assert.equal(await page.locator('#antenna' + name + 'Status').textContent(), 'Live');
  }
  await state(page, { antenna: { ...freshAntenna, ocean: 'Unknown (255)', registration_fresh: false,
    protocol: null, protocol_fresh: false, channel: '0' } });
  assert.equal(await page.locator('#antennaOcean').textContent(), 'Unknown (255)', 'Unknown codes are supplied by the firmware formatter');
  assert.equal(await page.locator('#antennaRegistrationStatus').textContent(), 'Stale');
  assert.equal(await page.locator('#antennaRegistration').textContent(), 'Logged in', 'Retain stale data with an explicit status');
  assert.equal(await page.locator('#antennaProtocol').textContent(), '—');
  assert.equal(await page.locator('#antennaProtocolStatus').textContent(), 'Waiting');
  assert.equal(await page.locator('#antennaChannel').textContent(), '0', 'A zero channel is still a provided value');
  await state(page, { antenna: { ...freshAntenna, online: false } });
  for (const name of names) assert.equal(await page.locator('#antenna' + name + 'Status').textContent(), 'Stale');
  await state(page, { antenna: freshAntenna });
  await page.evaluate(() => window.__device.close());
  await page.waitForFunction(() => document.getElementById('controls').disabled);
  for (const name of names) {
    assert.equal(await page.locator('#antenna' + name + 'Status').textContent(), 'Stale');
    assert(await page.locator('#antenna' + name + 'Card').evaluate(node => node.classList.contains('stale')));
  }
  assert.deepEqual(await page.evaluate(() => window.__device.frames.filter(f => f.op !== 'heartbeat')), sentBefore,
    'Rendering status must not send any network-management command');
});

test('SAILOR network text is safe and fits desktop and mobile in both themes', async t => {
  const names = ['Ocean', 'Registration', 'Protocol', 'Channel'];
  const hostile = '<img src=x onerror="window.__networkXss=true">';
  for (const mobile of [false, true]) {
    const page = await fixture(t, { mobile }); await connect(page); await selectMode(page, 'sailor');
    await state(page, { antenna: { ...freshAntenna, ocean: hostile, registration: hostile, protocol: hostile, channel: hostile } });
    for (const name of names) assert.equal(await page.locator('#antenna' + name).textContent(), hostile);
    assert.equal(await page.locator('#antennaPanel img').count(), 0);
    assert.equal(await page.evaluate(() => window.__networkXss), undefined);
    for (const theme of ['Day', 'Night']) {
      await page.locator('#webTheme' + theme).click();
      const panel = await page.locator('#antennaPanel').boundingBox();
      const terminal = await page.locator('#terminalLog').boundingBox();
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
      assert(panel.y + panel.height < terminal.y, 'Extended fields and terminal do not overlap');
      const boxes = [];
      for (const name of names) {
        const box = await page.locator('#antenna' + name + 'Card').boundingBox(); boxes.push(box);
        assert(box.x >= panel.x && box.x + box.width <= panel.x + panel.width);
        assert(box.y >= panel.y && box.y + box.height <= panel.y + panel.height);
      }
      assert.equal(boxes[0].y, boxes[1].y);
      if (mobile) assert(boxes[2].y > boxes[0].y, 'Mobile uses two network columns');
      else assert.equal(boxes[0].y, boxes[3].y, 'Desktop uses four network columns');
    }
  }
});

test('SAILOR network updates preserve terminal history and its reading position', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  const output = terminalWriter(page);
  await output(Array.from({ length: 100 }, (_, i) => `status-history-${i}\r\n`).join(''));
  await terminalScrollTo(page, 5);
  const before = await terminalViewport(page), text = await page.locator('#terminalLog').textContent();
  await state(page, { antenna: freshAntenna });
  await state(page, { antenna: { ...freshAntenna, protocol: 'Retuning', channel: '12581' } });
  const after = await terminalViewport(page);
  assert.equal(await page.locator('#terminalLog').textContent(), text);
  assert(Math.abs(after.top - before.top) <= 2, 'Network updates do not scroll the terminal');
  assert.equal(after.firstLine, before.firstLine);
  assert.match(text, /^status-history-0\n/);
  assert.equal(await page.locator('#antennaProtocol').textContent(), 'Retuning');
  assert(await page.locator('#terminalLatest').isEnabled());
});

test('SAILOR antenna distinguishes missing, fresh, stale and valid zero coordinates', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  for (const field of ['Signal', 'Serial', 'Position']) {
    assert.equal(await page.locator('#antenna' + field).textContent(), '—');
    assert.equal(await page.locator('#antenna' + field + 'Status').textContent(), 'Waiting');
  }
  await emit(page, { type: 'terminal', seq: 1, hex: Buffer.from('READY\r\ncan0:/$ ').toString('hex') });
  const terminal = await page.locator('#terminalLog').textContent();
  const modeCount = (await frames(page, 'mode')).length;
  await state(page, { terminal_ready: true, antenna: freshAntenna });
  assert.equal(await page.locator('#antennaSignal').textContent(), '42 dBHz');
  assert.equal(await page.locator('#antennaSignalBars .lit').count(), 5);
  assert.equal(await page.locator('#antennaSerial').textContent(), '12340001');
  assert.equal(await page.locator('#antennaPosition').textContent(), '12° 34.567′ N\n45° 06.789′ E');
  assert.equal(await page.locator('#antennaPositionStatus').textContent(), 'Live');
  assert.doesNotMatch(await page.locator('#antennaPanel').textContent(), /%|SOG|COG|Speed|Course/);

  await state(page, { antenna: { ...freshAntenna, cn0_dbhz: 0, signal_bars: 0, latitude: 0, longitude: 0 } });
  assert.equal(await page.locator('#antennaSignal').textContent(), '0 dBHz');
  assert.equal(await page.locator('#antennaSignalBars .lit').count(), 0);
  assert.equal(await page.locator('#antennaPosition').textContent(), '0° 00.000′ N\n0° 00.000′ E', 'Real zero coordinates must not be treated as missing');

  await state(page, { antenna: { ...freshAntenna, position_fresh: false, position_age_ms: 16000 } });
  assert.equal(await page.locator('#antennaPositionStatus').textContent(), 'Stale');
  assert.match(await page.locator('#antennaPosition').textContent(), /12° 34\.567′ N/);
  await state(page, { antenna: { ...freshAntenna, signal_valid: false, cn0_dbhz: null, signal_bars: null, signal_age_ms: 16000,
    position_valid: false, position_fresh: false, latitude: null, longitude: null, position_utc: null, position_age_ms: null } });
  assert.equal(await page.locator('#antennaSignal').textContent(), '—');
  assert.equal(await page.locator('#antennaSignalStatus').textContent(), 'Stale');
  assert.equal(await page.locator('#antennaSignalBars .lit').count(), 0);
  assert.equal(await page.locator('#antennaPosition').textContent(), '—');
  assert.equal(await page.locator('#antennaPositionStatus').textContent(), 'Waiting');
  assert.equal(await page.locator('#terminalLog').textContent(), terminal, 'Telemetry updates preserve terminal output');
  assert.equal((await frames(page, 'mode')).length, modeCount, 'Telemetry rendering must not control hardware');
});

test('SAILOR antenna and web disconnects mark retained position stale without clearing terminal', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  await state(page, { terminal_ready: true, antenna: freshAntenna });
  await emit(page, { type: 'terminal', seq: 7, hex: Buffer.from('keep this output').toString('hex') });
  const terminal = await page.locator('#terminalLog').textContent();
  await state(page, { antenna: { ...freshAntenna, online: false, signal_valid: false, cn0_dbhz: null, signal_bars: null, position_fresh: false } });
  assert.equal(await page.locator('#antennaPositionStatus').textContent(), 'Stale');
  assert.equal(await page.locator('#antennaSerialStatus').textContent(), 'Last known');
  assert.equal(await page.locator('#antennaSerial').textContent(), '12340001');
  assert.equal(await page.locator('#antennaSignalBars .lit').count(), 0);
  await state(page, { antenna: freshAntenna });
  await page.evaluate(() => window.__device.close());
  await page.waitForFunction(() => document.getElementById('controls').disabled);
  assert.equal(await page.locator('#antennaPositionStatus').textContent(), 'Stale');
  assert.equal(await page.locator('#antennaSignalStatus').textContent(), 'Stale');
  assert.equal(await page.locator('#antennaSignalBars .lit').count(), 0);
  assert.equal(await page.locator('#terminalLog').textContent(), terminal);
});

test('SAILOR antenna text and mobile layout remain safe in Day and Night themes', async t => {
  const page = await fixture(t, { mobile: true }); await connect(page); await selectMode(page, 'sailor');
  const hostile = '<img src=x onerror="window.__antennaXss=true">';
  await state(page, { antenna: { ...freshAntenna, serial: hostile, latitude: -freshAntenna.latitude, longitude: -freshAntenna.longitude } });
  assert.equal(await page.locator('#antennaSerial').textContent(), hostile);
  assert.equal(await page.locator('#antennaPanel img').count(), 0);
  assert.equal(await page.evaluate(() => window.__antennaXss), undefined);
  assert.equal(await page.locator('#antennaPosition').textContent(), '12° 34.567′ S\n45° 06.789′ W');
  for (const theme of ['Day', 'Night']) {
    await page.locator('#webTheme' + theme).click();
    assert.equal(await page.locator('html').getAttribute('data-web-theme'), theme.toLowerCase());
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), true);
    const panel = await page.locator('#antennaPanel').boundingBox();
    const terminal = await page.locator('#terminalLog').boundingBox();
    assert(panel.x >= 0 && panel.x + panel.width <= 390);
    assert(panel.y + panel.height < terminal.y, 'Antenna panel appears above the terminal without overlap');
    assert.equal(await page.locator('#antennaSignalBars .lit').count(), 5);
  }
});

test('actual SAILOR bytes, keyboard controls, terminal replay ACK, and bridge validation', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor'); await state(page, { terminal_ready: true });
  await emit(page, { type: 'terminal', seq: 1, hex: Buffer.from('READY\r\n> ').toString('hex') });
  const initial = await page.locator('#terminalLog').textContent(); assert.match(initial, /READY/);
  await emit(page, { type: 'terminal', seq: 1, hex: Buffer.from('READY\r\n> ').toString('hex') });
  assert.equal(await page.locator('#terminalLog').textContent(), initial); assert.equal((await last(page, 'terminal_ack')).seq, 1);
  await page.locator('#terminalInput').fill('help'); await page.locator('#terminalForm button[type=submit]').click();
  assert.equal((await last(page, 'terminal')).hex, '68656c700d');
  await page.locator('#terminalEsc').click(); assert.equal((await last(page, 'terminal')).hex, '1b');
  await page.locator('#terminalCtrlC').click(); assert.equal((await last(page, 'terminal')).hex, '03');
  await page.locator('#terminalLog').focus(); await page.keyboard.press('ArrowUp'); assert.equal((await last(page, 'terminal')).hex, '1b5b41');
  await page.keyboard.press('Control+c'); assert.equal((await last(page, 'terminal')).hex, '03');
  await selectMode(page, 'rs485_bridge');
  await page.locator('#bridgeHex').fill('00 ff 41'); await page.locator('#bridgeForm button').click(); assert.equal((await last(page, 'bridge_write')).hex, '00ff41');
  const before = (await frames(page, 'bridge_write')).length;
  await page.locator('#bridgeHex').fill('ab c'); await page.locator('#bridgeForm button').click();
  assert.equal((await frames(page, 'bridge_write')).length, before); assert.match(await page.locator('#message').textContent(), /hexadecimal/i);
});

test('server errors, pending-command disconnect, token reconnect and local revocation', async t => {
  const page = await fixture(t, { clock: true }); await connect(page);
  await selectMode(page, 'rx485');
  await page.evaluate(() => { window.__device.failures.mode = 'ESP_ERR_INVALID_STATE'; });
  await page.locator('#stopMode').click(); await page.waitForFunction(() => document.getElementById('message').textContent.includes('ESP_ERR_INVALID_STATE'));
  await page.evaluate(() => { window.__device.autoAck = false; });
  await page.locator('#stopMode').click();
  await page.evaluate(() => window.__device.close());
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  await page.clock.runFor(1600);
  await page.waitForFunction(() => !document.getElementById('controls').disabled);
  assert.equal((await frames(page, 'claim')).length, 2);
  assert.equal((await last(page, 'claim')).token, '0123456789abcdef0123456789abcdef');
  await emit(page, { type: 'released', reason: 'local_button' });
  assert.match(await page.locator('#message').textContent(), /local|device|button/i);
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  await page.clock.runFor(65000); assert.equal((await frames(page, 'claim')).length, 2);
});

test('stored-token resume, denied claim, and stale responses from replaced sockets', async t => {
  const page = await fixture(t, { storedToken: '0123456789abcdef0123456789abcdef' });
  await page.waitForFunction(() => !document.getElementById('controls').disabled);
  assert.equal((await last(page, 'claim')).token, '0123456789abcdef0123456789abcdef');
  await emit(page, { type: 'released', reason: 'local_button' });
  await page.evaluate(() => { window.__device.rejectClaim = true; });
  await page.locator('#connect').click();
  await page.waitForFunction(() => document.getElementById('message').textContent.includes('Another browser has control'));
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  await page.evaluate(() => { window.__device.sockets[0].onmessage({ data: JSON.stringify({ type: 'claimed', token: 'stale', next_id: 1 }) }); });
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  assert.equal(await page.evaluate(() => sessionStorage.getItem('nmea-token')), null);
});


test('command timeout and bounded reconnect expiry restore the connect button', async t => {
  const page = await fixture(t, { clock: true }); await connect(page); await selectMode(page, 'rx485');
  await page.evaluate(() => { window.__device.autoAck = false; });
  await page.locator('#stopMode').click();
  await page.clock.runFor(15100);
  assert.match(await page.locator('#message').textContent(), /mode.*timed out|No acknowledgement.*mode|No confirmation.*mode/i);
  await page.evaluate(() => { window.__device.offline = true; window.__device.close(); });
  await page.clock.runFor(62000);
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  assert(await page.locator('#connect').isVisible());
  assert.equal(await page.evaluate(() => sessionStorage.getItem('nmea-token')), null);
  assert.match(await page.locator('#message').textContent(), /reconnect.*expired|reconnect.*timed out/i);
});


test('VT100 fragmented cursor/erase sequences preserve fixed ASCII alignment', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor'); await state(page, { terminal_ready: true });
  let seq = 20;
  const output = text => emit(page, { type: 'terminal', seq: seq++, hex: Buffer.from(text).toString('hex') });
  await output('ABC\rZ'); assert.equal((await page.locator('#terminalLog').textContent()).split('\n')[0], 'ZBC');
  await output('\x1b[2J\x1b[H\x1b[2;');
  await output('4HXY');
  let lines = (await page.locator('#terminalLog').textContent()).split('\n');
  assert.equal(lines[0], ''); assert.equal(lines[1], '   XY'); assert.equal(lines.length, 2, 'Only occupied/cursor rows need display');
  await output('\x1b[2;5H\x1b[K');
  lines = (await page.locator('#terminalLog').textContent()).split('\n'); assert.equal(lines[1], '   X');
  await output('\x1b[2J\x1b[H+---+\r\n|A B|\r\n+---+');
  assert.deepEqual((await page.locator('#terminalLog').textContent()).split('\n').slice(0,3), ['+---+', '|A B|', '+---+']);
  await page.locator('#terminalClear').click(); assert.equal(await page.locator('#terminalLog').textContent(), '');
});


// Long SAILOR responses must remain readable without altering the remote VT screen.
function terminalWriter(page, firstSequence = 100) {
  let seq = firstSequence;
  return async text => {
    const packet = { type: 'terminal', seq: seq++, hex: Buffer.from(text).toString('hex') };
    await emit(page, packet);
    return packet;
  };
}
async function terminalViewport(page) {
  return page.locator('#terminalLog').evaluate(node => {
    const lineHeight = parseFloat(getComputedStyle(node).lineHeight);
    return { top: node.scrollTop, gap: node.scrollHeight - node.clientHeight - node.scrollTop,
      lineHeight, firstLine: node.textContent.split('\n')[Math.floor(node.scrollTop / lineHeight)] };
  });
}
async function terminalScrollTo(page, line) {
  await page.locator('#terminalLog').evaluate(async (node, line) => {
    node.scrollTop = line * parseFloat(getComputedStyle(node).lineHeight);
    node.dispatchEvent(new Event('scroll'));
    await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
  }, line);
}

test('SAILOR short terminal prompts remain visible on desktop and mobile', async t => {
  for (const mobile of [false, true]) {
    const page = await fixture(t, { mobile }); await connect(page); await selectMode(page, 'sailor');
    const output = terminalWriter(page);
    await output('READY\r\ncan0:/$ ');
    const view = await terminalViewport(page);
    assert(view.top <= view.lineHeight, 'Automatic following must keep the initial prompt in view');
    assert(await page.locator('#terminalLatest').isDisabled(), 'Short output remains in live-follow mode');
    await output('\x1b[2J\x1b[HStatus: 42 dBHz');
    assert((await terminalViewport(page)).top <= 2, 'A short status redraw stays visible at the top of the live screen');
    assert.equal(await page.locator('#terminalLog').textContent(), 'Status: 42 dBHz');
    await output('\x1b[24;1H');
    assert.equal((await page.locator('#terminalLog').textContent()).split('\n').length, 24, 'The remote cursor can still address all 24 rows');
    assert((await terminalViewport(page)).gap <= 2, 'Following also reaches an explicitly addressed bottom row');
  }
});

test('SAILOR long multi-packet responses retain their first lines and respect manual scrolling', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  const output = terminalWriter(page);
  let packet;
  for (let i = 0; i < 90; i += 10) {
    packet = await output(Array.from({ length: 10 }, (_, n) => `help-line-${i + n}\r\n`).join(''));
  }
  const beforeReplay = await page.locator('#terminalLog').textContent();
  assert.match(beforeReplay, /^help-line-0\nhelp-line-1\n/);
  assert.match(beforeReplay, /\nhelp-line-89\n/);
  assert((await terminalViewport(page)).gap <= 2, 'New output follows the bottom initially');
  await emit(page, packet);
  assert.equal(await page.locator('#terminalLog').textContent(), beforeReplay, 'Repeated WebSocket packet adds no history');
  assert.equal((await last(page, 'terminal_ack')).seq, packet.seq);
  await terminalScrollTo(page, 3);
  const reading = await terminalViewport(page);
  await output('new-line-90\r\nnew-line-91\r\nnew-line-92\r\n');
  const after = await terminalViewport(page);
  assert(Math.abs(after.top - reading.top) <= 2, 'Reading earlier output must not jump to the bottom');
  assert.equal(after.firstLine, reading.firstLine);
});

test('SAILOR ANSI status redraws and partial scrolling do not fabricate history', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  const output = terminalWriter(page);
  await output(Array.from({ length: 30 }, (_, n) => `original-${n}\r\n`).join(''));
  await output('\x1b[2J\x1b[HStatus: 40 dBHz');
  let text = await page.locator('#terminalLog').textContent();
  assert.match(text, /^original-0\n/, 'Erase-screen retains preceding scrollback');
  assert.doesNotMatch(text, /original-29/, 'Erase-screen does not archive the screen it replaces');
  const oldHistory = text.split('\n').filter(line => line.startsWith('original-'));
  await output('\x1b[2J\x1b[HStatus: 42 dBHz');
  await output('\x1b[2;4r\x1b[2;1HA\r\nB\r\nC\r\nD');
  await output('\x1b[2;1H\x1bM\x1b[1L\x1b[1M');
  text = await page.locator('#terminalLog').textContent();
  assert.deepEqual(text.split('\n').filter(line => line.startsWith('original-')), oldHistory, 'Regional scrolling and line editing leave history unchanged');
  assert.match(text, /^original-0\n/);
  assert.match(text, /Status: 42 dBHz/);
  assert.doesNotMatch(text, /Status: 40 dBHz/);
  await output('\x1b[3J');
  assert.equal((await page.locator('#terminalLog').textContent()).trim(), '', 'Erase-scrollback removes retained output');
  await output('\x1b[r\x1b[H' + Array.from({ length: 30 }, (_, n) => `reset-${n}\r\n`).join(''));
  await output('\x1bcRESET');
  text = await page.locator('#terminalLog').textContent();
  assert.equal(text, 'RESET', 'Reset removes the old screen and history');
  assert.doesNotMatch(text, /reset-/);
});

test('SAILOR scrollback is bounded and keeps the reading position when old lines expire', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  const output = terminalWriter(page);
  for (let i = 0; i < 2400; i += 40) {
    await output(Array.from({ length: 40 }, (_, n) => `row-${String(i + n).padStart(4, '0')}\r\n`).join(''));
  }
  let text = await page.locator('#terminalLog').textContent();
  assert.equal(text.split('\n').length, 2024, 'Memory use is limited to 2000 past lines and 24 live rows');
  assert.doesNotMatch(text, /row-0000/);
  assert.match(text, /row-2399/);
  await terminalScrollTo(page, 400);
  const reading = await terminalViewport(page);
  await output(Array.from({ length: 20 }, (_, n) => `row-${2400 + n}\r\n`).join(''));
  const afterTrim = await terminalViewport(page);
  assert.equal(afterTrim.firstLine, reading.firstLine, 'A retained line stays at the same place in the viewport');
  assert(Math.abs(afterTrim.top - reading.top + 20 * reading.lineHeight) <= 2);
  await page.locator('#terminalClear').click();
  assert.equal(await page.locator('#terminalLog').textContent(), '', 'Clear display also releases the scrollback');
  await output('fresh session');
  text = await page.locator('#terminalLog').textContent();
  assert.match(text, /^fresh session/);
  assert.doesNotMatch(text, /row-/);
});

test('SAILOR history position survives hidden Settings while new terminal output arrives', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor');
  const output = terminalWriter(page);
  await output(Array.from({ length: 70 }, (_, n) => `view-${n}\r\n`).join(''));
  await terminalScrollTo(page, 8);
  const reading = await terminalViewport(page);
  await settings(page);
  await output('while-hidden-1\r\nwhile-hidden-2\r\n');
  await selectMode(page, 'sailor');
  const restored = await terminalViewport(page);
  assert.equal(restored.firstLine, reading.firstLine);
  assert(Math.abs(restored.top - reading.top) <= 2, 'Opening Settings must not lose the terminal reading position');
});

test('SAILOR local history keys and Latest output preserve terminal command input', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor'); await state(page, { terminal_ready: true });
  const output = terminalWriter(page);
  await output(Array.from({ length: 80 }, (_, n) => `keyboard-${n}\r\n`).join(''));
  await page.locator('#terminalLog').focus();
  const count = (await frames(page, 'terminal')).length;
  await page.keyboard.press('Shift+PageUp');
  await page.waitForFunction(() => { const n = document.getElementById('terminalLog'); return n.scrollHeight - n.clientHeight - n.scrollTop > 10; });
  const up = await terminalViewport(page);
  await page.keyboard.press('Shift+PageDown');
  assert((await terminalViewport(page)).top > up.top);
  await page.keyboard.press('Shift+Home');
  assert((await terminalViewport(page)).top <= 2);
  await page.keyboard.press('Shift+End');
  assert((await terminalViewport(page)).gap <= 2);
  assert.equal((await frames(page, 'terminal')).length, count, 'History navigation must not send CAN terminal bytes');
  await page.keyboard.press('ArrowUp'); assert.equal((await last(page, 'terminal')).hex, '1b5b41');
  await page.keyboard.press('Control+c'); assert.equal((await last(page, 'terminal')).hex, '03');
  await terminalScrollTo(page, 0);
  await page.locator('#terminalLatest').click();
  assert((await terminalViewport(page)).gap <= 2, 'Latest output returns to the live end');
  await output('continued-output\r\n');
  assert((await terminalViewport(page)).gap <= 2, 'Latest output also resumes automatic following');
});

test('rejected terminal input survives and UTF-8 byte overflow is rejected locally', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'sailor'); await state(page, { terminal_ready: true });
  await page.evaluate(() => { window.__device.failures.terminal = 'ESP_ERR_NO_MEM'; });
  await page.locator('#terminalInput').fill('important-command'); await page.locator('#terminalForm button[type=submit]').click();
  await page.waitForFunction(() => document.getElementById('message').textContent.includes('ESP_ERR_NO_MEM'));
  assert.equal(await page.locator('#terminalInput').inputValue(), 'important-command');
  await page.evaluate(() => { delete window.__device.failures.terminal; window.__device.autoAck = false; });
  await page.locator('#terminalForm button[type=submit]').click();
  const request = await last(page, 'terminal');
  await page.locator('#terminalInput').fill('next-command');
  await emit(page, { type: 'ack', id: request.id, ok: true, error: 'ESP_OK' });
  assert.equal(await page.locator('#terminalInput').inputValue(), 'next-command');
  const count = (await frames(page, 'terminal')).length;
  await page.locator('#terminalInput').fill('界'.repeat(200));
  await page.locator('#terminalForm button[type=submit]').click();
  assert.equal((await frames(page, 'terminal')).length, count);
  assert.equal(await page.locator('#terminalInput').inputValue(), '界'.repeat(200));
  assert.match(await page.locator('#message').textContent(), /512 bytes/i);
});


test('pending mode switch does not restart twice or overwrite a newer Settings view', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'rx485');
  const before = (await frames(page, 'mode')).length;
  const actualLabel = await page.locator('#currentMode').textContent();
  await page.evaluate(() => { window.__device.autoAck = false; });
  await page.locator('[data-mode="tx485"]').click();
  const request = await last(page, 'mode');
  assert.equal(request.mode, 'tx485');
  assert.equal(await page.locator('#currentMode').textContent(), actualLabel,
    'Requested hardware state must not be presented as confirmed');
  await page.locator('[data-mode="tx485"]').evaluate(node => node.click());
  assert.equal((await frames(page, 'mode')).length, before + 1);
  await state(page, { mode: 'rx485' });
  assert(await page.locator('[data-mode="tx485"]').evaluate(node => node.classList.contains('active')),
    'An earlier runtime snapshot must not redirect the selected workspace');
  await settings(page);
  await emit(page, { type: 'ack', id: request.id, ok: true, error: 'ESP_OK' });
  await state(page, { mode: 'tx485' });
  assert(await page.locator('#settings').isVisible(), 'A completed earlier switch must not steal Settings focus');
  assert(await page.locator('[data-mode="tx485"]').evaluate(node => node.classList.contains('running')));
  assert.equal((await frames(page, 'mode')).length, before + 1);
});

test('failed mode switch retains confirmed runtime and allows retry', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'rx485');
  const actualLabel = await page.locator('#currentMode').textContent();
  await page.evaluate(() => { window.__device.failures.mode = 'ESP_ERR_INVALID_STATE'; });
  await page.locator('[data-mode="n2k"]').click();
  await page.waitForFunction(() => document.getElementById('message').textContent.includes('ESP_ERR_INVALID_STATE'));
  assert.equal(await page.locator('#currentMode').textContent(), actualLabel);
  assert(await page.locator('[data-mode="rx485"]').evaluate(node => node.classList.contains('running')));
  assert(!(await page.locator('[data-mode="n2k"]').evaluate(node => node.classList.contains('running'))));
  await page.evaluate(() => { delete window.__device.failures.mode; });
  const count = (await frames(page, 'mode')).length;
  await selectMode(page, 'n2k');
  assert.equal((await frames(page, 'mode')).length, count + 1);
  assert(await page.locator('[data-mode="n2k"]').evaluate(node => node.classList.contains('running')));
});

test('global templates and Receiver views preserve hardware and English UI', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'rx485');
  const count = (await frames(page, 'mode')).length;
  await receiverView(page, 'ais');
  assert(await page.locator('#aisRefresh').isVisible());
  await receiverView(page, 'instruments');
  assert(await page.locator('#metrics').isVisible());
  await openTemplates(page);
  assert.equal((await frames(page, 'mode')).length, count);
  assert.equal(await page.locator('#currentMode').textContent(), 'Receiver');
  assert(await page.locator('[data-mode="rx485"]').evaluate(node => node.classList.contains('running')));
  assert.doesNotMatch(await page.locator('body').innerText(), /[\u0400-\u04ff]/,
    'The web UI should render English labels');
});


const gpsDraftTemplate = {
  type: 'template', group: 'gps',
  fields: [{ name: 'sog_kn', kind: 'n', min: 0, max: 100 }, { name: 'send_rmc', kind: 'b' }],
  values: { sog_kn: 5, send_rmc: false }
};

test('template apply ACK preserves edits made after submission', async t => {
  const page = await fixture(t); await connect(page); await openTemplates(page);
  await emit(page, gpsDraftTemplate);
  await page.evaluate(() => { window.__device.autoAck = false; });
  await page.locator('#field-sog_kn').fill('7.25');
  await page.locator('#editor button[type=submit]').click();
  const request = await last(page, 'template_patch');
  assert.deepEqual(request.values, { sog_kn: 7.25 });
  await page.locator('#field-sog_kn').fill('9.5');
  await page.locator('#field-send_rmc').check();
  await emit(page, { type: 'ack', id: request.id, ok: true, error: 'ESP_OK' });
  await emit(page, { ...gpsDraftTemplate, values: { sog_kn: 7.25, send_rmc: false } });
  assert.equal(await page.locator('#field-sog_kn').inputValue(), '9.5');
  assert(await page.locator('#field-send_rmc').isChecked());
  await page.locator('#editor button[type=submit]').click();
  assert.deepEqual((await last(page, 'template_patch')).values, { sog_kn: 9.5, send_rmc: true });
});

test('template apply ACK retains newer draft after switching groups', async t => {
  const page = await fixture(t); await connect(page); await openTemplates(page);
  await emit(page, gpsDraftTemplate);
  await page.evaluate(() => { window.__device.autoAck = false; });
  await page.locator('#field-sog_kn').fill('7.25');
  await page.locator('#editor button[type=submit]').click();
  const request = await last(page, 'template_patch');
  await page.locator('#field-sog_kn').fill('9.5');
  await page.locator('[data-template="gyro"]').click();
  await emit(page, { type: 'template', group: 'gyro',
    fields: [{ name: 'heading_true_deg', kind: 'n', min: 0, max: 360 }],
    values: { heading_true_deg: 85 } });
  await page.locator('#field-heading_true_deg').fill('90');
  await emit(page, { type: 'ack', id: request.id, ok: true, error: 'ESP_OK' });
  await emit(page, { ...gpsDraftTemplate, values: { sog_kn: 7.25, send_rmc: false } });
  assert.match(await page.locator('#editorTitle').textContent(), /GYRO/);
  assert.equal(await page.locator('#field-heading_true_deg').inputValue(), '90');
  await page.locator('[data-template="gps"]').click();
  await emit(page, { ...gpsDraftTemplate, values: { sog_kn: 7.25, send_rmc: false } });
  assert.equal(await page.locator('#field-sog_kn').inputValue(), '9.5');
  await page.locator('#editor button[type=submit]').click();
  assert.deepEqual((await last(page, 'template_patch')).values, { sog_kn: 9.5 });
});


test('mode switch stays busy until both ACK and matching device state arrive', async t => {
  const page = await fixture(t); await connect(page); await selectMode(page, 'rx485');
  for (const stateFirst of [false, true]) {
    const count = (await frames(page, 'mode')).length;
    await page.evaluate(() => { window.__device.autoAck = false; });
    await page.locator('[data-mode="tx485"]').click();
    const request = await last(page, 'mode');
    if (stateFirst) await state(page, { mode: 'tx485' });
    else await emit(page, { type: 'ack', id: request.id, ok: true, error: 'ESP_OK' });
    assert(await page.locator('[data-mode="rx485"]').isDisabled(),
      stateFirst ? 'A matching snapshot alone is not a successful command' : 'ACK alone must not use stale actual mode for a new switch');
    await page.locator('[data-mode="rx485"]').evaluate(node => node.click());
    assert.equal((await frames(page, 'mode')).length, count + 1);
    if (stateFirst) await emit(page, { type: 'ack', id: request.id, ok: true, error: 'ESP_OK' });
    else await state(page, { mode: 'tx485' });
    assert(!(await page.locator('[data-mode="rx485"]').isDisabled()));
    await page.evaluate(() => { window.__device.autoAck = true; });
    await selectMode(page, 'rx485');
    assert.equal((await last(page, 'mode')).mode, 'rx485');
    assert.equal((await frames(page, 'mode')).length, count + 2);
  }
});

test('missing mode confirmation times out without inventing runtime state', async t => {
  const page = await fixture(t, { clock: true }); await connect(page); await selectMode(page, 'rx485');
  const confirmed = await page.locator('#currentMode').textContent();
  await page.locator('[data-mode="tx485"]').click(); // ACK arrives; state deliberately does not.
  assert(await page.locator('[data-mode="rx485"]').isDisabled());
  await page.clock.runFor(13000);
  assert(!(await page.locator('[data-mode="rx485"]').isDisabled()));
  assert.equal(await page.locator('#currentMode').textContent(), confirmed);
  assert.match(await page.locator('#message').textContent(), /Mode change was not confirmed/);
});


test('template apply of zero does not erase a newer empty numeric draft', async t => {
  const page = await fixture(t); await connect(page); await openTemplates(page);
  await emit(page, gpsDraftTemplate);
  await page.evaluate(() => { window.__device.autoAck = false; });
  await page.locator('#field-sog_kn').fill('0');
  await page.locator('#editor button[type=submit]').click();
  const request = await last(page, 'template_patch');
  assert.deepEqual(request.values, { sog_kn: 0 });
  await page.locator('#field-sog_kn').fill('');
  await emit(page, { type: 'ack', id: request.id, ok: true, error: 'ESP_OK' });
  await emit(page, { ...gpsDraftTemplate, values: { sog_kn: 0, send_rmc: false } });
  assert.equal(await page.locator('#field-sog_kn').inputValue(), '');
  assert(!(await page.locator('#field-sog_kn').evaluate(input => input.checkValidity())));
  const count = (await frames(page, 'template_patch')).length;
  await page.locator('#editor button[type=submit]').click();
  assert.equal((await frames(page, 'template_patch')).length, count,
    'The preserved empty draft must be validated, not coerced to zero');
});

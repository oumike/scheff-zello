const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {test} = require('node:test');
const vm = require('node:vm');

const html = fs.readFileSync(path.join(__dirname, '../../main/web/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const status = {
  wifi: {connected: true, ssid: 'Stored SSID', ip: '192.0.2.1'},
  account: {network: 'consumer', username: 'stored-user', work_network: '',
    has_password: true, has_token: true},
  zello: {state: 'online', channel: '', users_online: 0, talking: false,
    receiving: false, talker: '', error: ''},
  ap: {active: false, ssid: ''},
  audio: {volume: 40, mic_gain: 10, ptt_latch: false},
  join: {state: 'idle', ssid: '', msg: ''},
  channels: 0,
};

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => {resolve = yes; reject = no;});
  return {promise, resolve, reject};
}

class Element {
  constructor() {
    this.value = '';
    this.checked = false;
    this.disabled = false;
    this.textContent = '';
    this.className = '';
    this.innerHTML = '';
    this.style = {};
    this.listeners = {};
    this.classList = {toggle() {}};
    this.scrollTop = this.clientHeight = this.scrollHeight = 0;
  }
  addEventListener(event, callback) {
    (this.listeners[event] ||= []).push(callback);
  }
  dispatch(event) {
    const e = {target: this};
    this['on' + event]?.(e);
    for (const callback of this.listeners[event] || []) callback(e);
  }
  focus() {}
}

const settle = async () => {
  for (let i = 0; i < 3; i++) await new Promise(resolve => setImmediate(resolve));
};

async function page({holdInitialStatus = false} = {}) {
  const elements = new Map([...html.matchAll(/\bid="([^"]+)"/g)]
    .map(match => [match[1], new Element()]));
  elements.get('network').value = 'consumer';
  const requests = [], pending = new Map();
  const initialStatus = holdInitialStatus ? deferred() : null;
  if (initialStatus) pending.set('/api/status', [initialStatus]);
  const context = vm.createContext({
    document: {getElementById: id => {
      assert.ok(elements.has(id), 'Unknown DOM id: ' + id);
      return elements.get(id);
    }},
    fetch: async (url, options = {}) => {
      requests.push({url, body: options.body ? JSON.parse(options.body) : undefined});
      const wait = pending.get(url)?.shift();
      const data = wait ? await wait.promise :
        url === '/api/status' ? structuredClone(status) :
        url.startsWith('/api/channels') && !options.body ? [] :
        url === '/api/crash' ? {files: [], sd: true, last_reset: 'power-on'} :
        url === '/api/log' ? {log: ''} : {ok: true};
      return {ok: true, json: async () => data};
    },
    TextEncoder, setTimeout, clearTimeout,
    setInterval() {}, confirm: () => true,
    alert(message) {throw new Error(message);},
    location: {},
  });
  vm.runInContext(script, context, {filename: 'main/web/index.html'});
  await settle();
  return {
    el: id => elements.get(id),
    run: source => vm.runInContext(source, context),
    requests,
    initialStatus,
    sent: url => requests.filter(request => request.url === url && request.body),
    defer(url) {
      const wait = deferred();
      if (!pending.has(url)) pending.set(url, []);
      pending.get(url).push(wait);
      return wait;
    },
    input(id, value, event = 'input') {
      elements.get(id).value = value;
      elements.get(id).dispatch(event);
    },
    save: () => elements.get('save').onclick(),
  };
}

test('Wi-Fi joins preserve the exact SSID, including whitespace', async () => {
  const p = await page();
  for (const ssid of ['  Café Wi-Fi  ', '   ', 'é'.repeat(16)]) {
    p.input('ssid', ssid);
    await p.el('join').onclick();
    assert.equal(p.sent('/api/wifi').at(-1).body.ssid, ssid);
  }
});

test('Wi-Fi rejects an overlong UTF-8 SSID instead of truncating it', async () => {
  const p = await page();
  p.input('ssid', 'é'.repeat(17));
  await p.el('join').onclick();
  assert.equal(p.sent('/api/wifi').length, 0);
  assert.match(p.el('wmsg').className, /err/);
});

test('Wi-Fi rejects only the genuinely empty SSID', async () => {
  const p = await page();
  p.input('ssid', '');
  await p.el('join').onclick();
  assert.equal(p.sent('/api/wifi').length, 0);
  assert.match(p.el('wmsg').className, /err/);
});

test('channel saves and automatic joins preserve exact names', async () => {
  const p = await page();
  p.run('openForm(null)');
  p.input('f-name', '  Café channel  ');
  await p.save();
  assert.equal(p.sent('/api/channels')[0].body.name, '  Café channel  ');
  assert.equal(p.sent('/api/channels/join')[0].body.name, '  Café channel  ');
  assert.equal(p.el('save').disabled, true);
});

test('channel names are checked in UTF-8 bytes without splitting characters', async () => {
  const p = await page();
  p.run('openForm(null)');
  p.input('f-name', 'é'.repeat(33));
  await p.save();
  assert.equal(p.sent('/api/channels').length, 0);
  assert.match(p.el('smsg').className, /err/);
  assert.equal(p.el('save').disabled, false);
  p.input('f-name', 'é'.repeat(32));
  await p.save();
  assert.equal(p.sent('/api/channels')[0].body.name, 'é'.repeat(32));
});

test('empty channel names are rejected and whitespace-only names are retained', async () => {
  const p = await page();
  p.run('openForm(null)');
  await p.save();
  assert.equal(p.sent('/api/channels').length, 0);
  assert.match(p.el('smsg').className, /err/);
  p.input('f-name', '   ');
  await p.save();
  assert.equal(p.sent('/api/channels').at(-1)?.body.name, '   ');
});

test('editing audio during a save leaves the newer value unsaved', async () => {
  const p = await page();
  p.input('vol', '50');
  const pending = p.defer('/api/audio');
  const saving = p.save();
  p.input('vol', '62');
  pending.resolve({ok: true});
  await saving;
  await settle();
  assert.equal(p.el('vol').value, '62');
  assert.equal(p.el('save').disabled, false);
  await p.run('refresh()');
  assert.equal(p.el('vol').value, '62');
  await p.save();
  assert.deepEqual(p.sent('/api/audio').map(request => request.body.volume), [50, 62]);
  assert.equal(p.el('save').disabled, true);
});

test('editing while saving cannot start a second overlapping save', async () => {
  const p = await page();
  p.input('vol', '50');
  const pending = p.defer('/api/audio');
  const saving = p.save();
  p.input('vol', '62');
  assert.equal(p.el('save').disabled, true);
  await p.save();
  assert.equal(p.sent('/api/audio').length, 1);
  pending.resolve({ok: true});
  await saving;
});

test('account credentials typed during a save survive its response and refresh', async () => {
  const p = await page();
  p.input('user', 'submitted-user');
  p.input('apass', 'submitted-password');
  const pending = p.defer('/api/account');
  const saving = p.save();
  p.input('user', 'newer-user');
  p.input('apass', 'newer-password');
  p.input('token', 'newer-token');
  pending.resolve({ok: true});
  await saving;
  await settle();
  assert.equal(p.el('user').value, 'newer-user');
  assert.equal(p.el('apass').value, 'newer-password');
  assert.equal(p.el('token').value, 'newer-token');
  assert.equal(p.el('save').disabled, false);
});

test('a later section uses the click-time snapshot and retains subsequent edits', async () => {
  const p = await page();
  p.input('vol', '50');
  p.input('user', 'submitted-user');
  const pending = p.defer('/api/audio');
  const saving = p.save();
  p.input('user', 'newer-user');
  pending.resolve({ok: true});
  await saving;
  await settle();
  assert.equal(p.sent('/api/account')[0].body.username, 'submitted-user');
  assert.equal(p.el('user').value, 'newer-user');
  assert.equal(p.el('save').disabled, false);
});

test('channel edits made during rename remain open and target the saved identity next time', async () => {
  const p = await page();
  p.run('openForm({name:"Original",desc:"old note",favorite:false,uses:0})');
  p.input('f-name', 'Submitted');
  const pending = p.defer('/api/channels');
  const saving = p.save();
  p.input('f-name', 'Newer');
  pending.resolve({ok: true});
  await saving;
  await settle();
  assert.equal(p.el('cform').style.display, 'block');
  assert.equal(p.el('f-name').value, 'Newer');
  assert.equal(p.el('save').disabled, false);
  await p.save();
  assert.equal(p.sent('/api/channels')[1].body.orig_name, 'Submitted');
  assert.equal(p.sent('/api/channels')[1].body.name, 'Newer');
});

test('a channel form opened during a different save is not silently discarded', async () => {
  const p = await page();
  p.run('openForm({name:"Original",desc:"",favorite:false,uses:0})');
  p.input('f-name', 'Submitted');
  const pending = p.defer('/api/channels');
  const saving = p.save();
  p.run('openForm(null)');
  p.input('f-name', 'Other form');
  pending.resolve({ok: true});
  await saving;
  await settle();
  assert.equal(p.el('cform').style.display, 'block');
  assert.equal(p.el('f-name').value, 'Other form');
  assert.equal(p.el('save').disabled, false);
});

test('failed saves retain edits while already acknowledged sections remain clean', async () => {
  const p = await page();
  p.input('vol', '50');
  p.input('user', 'submitted-user');
  const pending = p.defer('/api/account');
  const saving = p.save();
  await settle();
  pending.reject(new Error('Device unavailable'));
  await saving;
  assert.match(p.el('smsg').className, /err/);
  assert.equal(p.el('save').disabled, false);
  await p.save();
  assert.equal(p.sent('/api/audio').length, 1);
  assert.equal(p.sent('/api/account').length, 2);
});

test('an older status request cannot overwrite a more recently saved account', async () => {
  const p = await page();
  const old = p.defer('/api/status');
  const refreshing = p.run('refresh()');
  p.input('user', 'new-user');
  const current = p.defer('/api/status');
  await p.save();
  old.resolve(structuredClone(status));
  await refreshing;
  assert.equal(p.el('user').value, 'new-user');
  current.resolve({...structuredClone(status),
    account: {...status.account, username: 'new-user'}});
  await settle();
});

test('the first status response does not overwrite a newly typed SSID', async () => {
  const p = await page({holdInitialStatus: true});
  p.input('ssid', '  New Wi-Fi  ');
  p.initialStatus.resolve(structuredClone(status));
  await settle();
  assert.equal(p.el('ssid').value, '  New Wi-Fi  ');
});

test('new audio edits survive a settings import and its automatic refresh', async () => {
  const p = await page();
  p.input('vol', '50');
  const pending = p.defer('/api/sd/import');
  const importing = p.el('sdi').onclick();
  p.input('vol', '62');
  pending.resolve({settings: true, channels: false, count: 0, reconnecting: false});
  await importing;
  await settle();
  assert.equal(p.el('vol').value, '62');
  assert.equal(p.el('save').disabled, false);
});

test('channel-only imports do not discard unsaved account or audio edits', async () => {
  const p = await page();
  p.input('vol', '62');
  p.input('user', 'unsaved-user');
  const pending = p.defer('/api/sd/import');
  const importing = p.el('sdi').onclick();
  pending.resolve({settings: false, channels: true, count: 1, reconnecting: false});
  await importing;
  await settle();
  assert.equal(p.el('vol').value, '62');
  assert.equal(p.el('user').value, 'unsaved-user');
  assert.equal(p.el('save').disabled, false);
});

test('save and restore requests cannot overlap', async () => {
  const p = await page();
  p.input('vol', '50');
  const pending = p.defer('/api/sd/import');
  const importing = p.el('sdi').onclick();
  p.input('vol', '62');
  assert.equal(p.el('save').disabled, true);
  await p.save();
  assert.equal(p.sent('/api/audio').length, 0);
  pending.resolve({settings: true, channels: false, count: 0, reconnecting: false});
  await importing;
});

test('failed restores retain edits and allow a later save', async () => {
  const p = await page();
  p.input('vol', '62');
  const pending = p.defer('/api/sd/import');
  const importing = p.el('sdi').onclick();
  pending.reject(new Error('Invalid backup'));
  await importing;
  assert.match(p.el('bmsg').className, /err/);
  assert.equal(p.el('vol').value, '62');
  assert.equal(p.el('save').disabled, false);
  await p.save();
  assert.equal(p.sent('/api/audio')[0].body.volume, 62);
});

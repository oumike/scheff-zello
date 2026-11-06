const select = document.getElementById('device');
const note = document.getElementById('note');
const install = document.getElementById('install');
const versionLabel = document.getElementById('version');
const status = document.getElementById('status');

const [devices, release] = await Promise.all([
  fetch('devices.json').then(r => r.json()),
  fetch('firmware/release.json').then(r => (r.ok ? r.json() : null)).catch(() => null),
]);

const available = new Set(release?.envs ?? []);

for (const device of devices) {
  const option = new Option(device.name, device.env);
  option.disabled = !available.has(device.env);
  select.add(option);
}

const saved = localStorage.getItem('device');
const first = devices.find(d => d.env === saved && available.has(d.env))
  ?? devices.find(d => available.has(d.env));
if (first) select.value = first.env;

function update() {
  const device = devices.find(d => d.env === select.value);
  const ready = device && available.has(device.env);
  install.hidden = !ready;
  note.hidden = !device?.note;
  note.textContent = device?.note ?? '';
  if (ready) {
    install.manifest = `firmware/${device.env}.json`;
    localStorage.setItem('device', device.env);
  }
  status.hidden = ready;
  status.textContent = available.size ? 'No build for this device in the latest release.' : 'No release yet.';
}

if (release?.version) {
  versionLabel.innerHTML = '';
  const link = document.createElement('a');
  link.href = release.url;
  link.textContent = `v${release.version}`;
  versionLabel.append(link);
}

select.addEventListener('change', update);
update();

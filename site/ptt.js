// The hero's talk button: amber while the stream "opens", green while held.
const ptt = document.getElementById('ptt');
const label = ptt.querySelector('.ptt-label');
const who = document.getElementById('ptt-help');
let timer;

function set(state, text, status) {
  ptt.dataset.state = state;
  label.textContent = text;
  who.textContent = status;
}

function press(e) {
  if (e.type === 'keydown' && (e.repeat || (e.key !== ' ' && e.key !== 'Enter'))) return;
  e.preventDefault();
  if (ptt.dataset.state !== 'idle') return;
  set('opening', 'Opening…', 'Zello is opening your stream');
  timer = setTimeout(() => set('talking', 'Talking', 'You have the channel'), 450);
}

function release(e) {
  if (e.type === 'keyup' && e.key !== ' ' && e.key !== 'Enter') return;
  const s = ptt.dataset.state;
  if (s !== 'opening' && s !== 'talking') return;
  clearTimeout(timer);
  // Let go and the channel answers, showing the red "someone else" state.
  set('busy', 'Alex', 'Alex is talking');
  timer = setTimeout(() => set('idle', 'Hold to talk', 'Ready'), 1600);
}

ptt.addEventListener('pointerdown', press);
ptt.addEventListener('keydown', press);
for (const ev of ['pointerup', 'pointercancel', 'pointerleave', 'keyup', 'blur']) ptt.addEventListener(ev, release);
ptt.addEventListener('contextmenu', e => e.preventDefault());

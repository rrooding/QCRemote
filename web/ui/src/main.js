import { Store, websocketUrl } from './store.js';
import { BUILD } from './build.js';

// One store for the page, created before any component upgrades so that a
// component's constructor can take it. A module-scope export would be
// tidier than a global, but every component would then import this module
// and this module imports them - a cycle. `window.qcStore` is the seam,
// and it is set exactly once, here.
window.qcStore = new Store({
  url: websocketUrl(location),
  build: BUILD,
  WebSocket,
  location,
  sessionStorage,
  events: window,
  document,
});

await import('./qc-app.js');
document.body.appendChild(document.createElement('qc-app'));
window.qcStore.connect();

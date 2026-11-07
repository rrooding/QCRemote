import { Store, websocketUrl } from './store.js';

// One store for the page, created before any component upgrades so that a
// component's constructor can take it. A module-scope export would be
// tidier than a global, but every component would then import this module
// and this module imports them - a cycle. `window.qcStore` is the seam, and
// it is set exactly once, here.
window.qcStore = new Store(websocketUrl(location));

await import('./qc-app.js');
document.body.appendChild(document.createElement('qc-app'));
window.qcStore.connect();

// Glue between the single Store and Lit's update cycle.
//
// Lit's ReactiveController interface is the supported way to tie an external
// data source to a component's lifecycle: it subscribes on connect and
// unsubscribes on disconnect, so a component that is removed from the DOM
// stops holding a reference into the store. Doing the same thing by hand in
// connectedCallback/disconnectedCallback works until a component forgets one
// half of it, and then the leak is invisible.
export class StoreController {
  #host;
  #store;
  #unsubscribe = null;

  constructor(host, store) {
    this.#host = host;
    this.#store = store;
    host.addController(this);
  }

  get state() {
    return this.#store.state;
  }

  get store() {
    return this.#store;
  }

  hostConnected() {
    this.#unsubscribe = this.#store.subscribe(() => this.#host.requestUpdate());
  }

  hostDisconnected() {
    this.#unsubscribe?.();
    this.#unsubscribe = null;
  }
}

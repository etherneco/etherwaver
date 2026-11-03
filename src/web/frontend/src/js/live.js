// Live update channel (Server-Sent Events).
//
// - reconnects automatically (EventSource retry + our own watchdog);
// - after a reconnect the backend replays missed events or sends "resync",
//   in which case every view reloads its full state;
// - while disconnected the channel reports "offline" so views mark data as
//   possibly outdated instead of presenting it as current.

import { api } from './api.js';

const EVENTS = ['hello', 'resync', 'server.status', 'server.event', 'backend.state', 'service.state',
  'config.changed', 'job.updated', 'log', 'security.pending', 'security.changed', 'session.expired'];

export class LiveChannel {
  constructor() {
    this.handlers = new Map();
    this.state = 'connecting';
    this.source = null;
    this.lastEventAt = 0;
    this.watchdog = null;
  }

  on(type, fn) {
    if (!this.handlers.has(type)) this.handlers.set(type, new Set());
    this.handlers.get(type).add(fn);
    return () => this.handlers.get(type).delete(fn);
  }

  emit(type, data) {
    for (const fn of this.handlers.get(type) || []) {
      try { fn(data); } catch (e) { console.error(e); }
    }
  }

  setState(state) {
    if (state !== this.state) { this.state = state; this.emit('channel', state); }
  }

  start() {
    this.stop();
    const src = new EventSource(api.base + 'events', { withCredentials: true });
    this.source = src;
    this.setState('connecting');
    src.onopen = () => { this.lastEventAt = Date.now(); };
    src.onerror = () => {
      // EventSource reconnects by itself (with Last-Event-ID); until the
      // stream is back, data on screen may be outdated.
      this.setState(src.readyState === EventSource.CLOSED ? 'offline' : 'reconnecting');
      if (src.readyState === EventSource.CLOSED) setTimeout(() => this.start(), 3000);
    };
    for (const type of EVENTS) {
      src.addEventListener(type, ev => {
        this.lastEventAt = Date.now();
        let payload = {};
        try { payload = JSON.parse(ev.data); } catch { return; }
        if (type === 'hello') this.setState('live');
        if (type === 'session.expired') { this.stop(); }
        this.emit(type, payload.data || {});
      });
    }
    // comments (keep-alive) do not fire events; reopen if the stream stalls
    this.watchdog = setInterval(() => {
      if (this.source && Date.now() - this.lastEventAt > 40000 && this.state === 'live') {
        this.setState('reconnecting');
        this.start();
      }
    }, 10000);
    src.addEventListener('message', () => { this.lastEventAt = Date.now(); });
  }

  touch() { this.lastEventAt = Date.now(); }

  stop() {
    if (this.source) { this.source.close(); this.source = null; }
    if (this.watchdog) { clearInterval(this.watchdog); this.watchdog = null; }
    this.setState('offline');
  }
}

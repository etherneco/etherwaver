// HTTP API client. All business rules are enforced by the backend; this
// module only transports requests and normalises errors.

const BASE = new URL('api/v1/', document.baseURI).pathname;

let csrfToken = null;
let onUnauthenticated = () => {};

export function setCsrfToken(token) { csrfToken = token; }
export function setUnauthenticatedHandler(fn) { onUnauthenticated = fn; }

export class ApiError extends Error {
  constructor(status, body, requestId) {
    const err = (body && body.error) || {};
    super(err.message || `Request failed (${status})`);
    this.status = status;
    this.code = err.code || (status === 0 ? 'network_error' : 'error');
    this.details = err.details;
    this.requestId = err.requestId || requestId || null;
  }
  get isConflict() { return this.status === 412; }
  get isUncertain() { return this.code === 'outcome_unknown'; }
  get isBackendDown() { return this.status === 503 && this.code.startsWith('backend'); }
  get fieldErrors() {
    const out = {};
    if (Array.isArray(this.details)) {
      for (const d of this.details) out[d.field] = d.message;
    }
    return out;
  }
}

function newRequestId() {
  const a = new Uint8Array(8);
  crypto.getRandomValues(a);
  return Array.from(a, b => b.toString(16).padStart(2, '0')).join('');
}

export async function request(method, path, { body, etag, raw, signal } = {}) {
  const headers = { 'Accept': 'application/json', 'X-Request-Id': newRequestId() };
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  if (method !== 'GET' && csrfToken) headers['X-CSRF-Token'] = csrfToken;
  if (etag) headers['If-Match'] = etag;
  let resp;
  try {
    resp = await fetch(BASE + path, {
      method, headers, credentials: 'same-origin', signal,
      body: body !== undefined ? JSON.stringify(body) : undefined,
    });
  } catch (e) {
    if (e.name === 'AbortError') throw e;
    throw new ApiError(0, { error: { code: 'network_error', message: 'The web panel service is not reachable.' } }, headers['X-Request-Id']);
  }
  const requestId = resp.headers.get('X-Request-Id');
  if (raw && resp.ok) return resp;
  let data = null;
  const text = await resp.text();
  if (text) {
    try { data = JSON.parse(text); } catch { data = null; }
  }
  if (!resp.ok) {
    const err = new ApiError(resp.status, data, requestId);
    if (resp.status === 401 && path !== 'session') onUnauthenticated(err);
    throw err;
  }
  if (data && typeof data === 'object') {
    Object.defineProperty(data, '_etag', { value: resp.headers.get('ETag'), enumerable: false });
  }
  return data;
}

export const api = {
  get: (p, o) => request('GET', p, o),
  post: (p, body, o = {}) => request('POST', p, { ...o, body }),
  put: (p, body, o = {}) => request('PUT', p, { ...o, body }),
  patch: (p, body, o = {}) => request('PATCH', p, { ...o, body }),
  del: (p, o) => request('DELETE', p, o),
  base: BASE,
};

// Waits for a background job to finish. Uses live events when available
// and falls back to polling, never blocking a request indefinitely.
export async function waitForJob(job, live, onProgress) {
  let current = job;
  const done = s => ['succeeded', 'failed', 'uncertain', 'cancelled'].includes(s);
  onProgress && onProgress(current);
  return new Promise(resolve => {
    let finished = false;
    const finish = j => { if (!finished) { finished = true; off(); clearInterval(timer); resolve(j); } };
    const off = live.on('job.updated', data => {
      if (data.id === job.id) { current = data; onProgress && onProgress(data); if (done(data.state)) finish(data); }
    });
    const timer = setInterval(async () => {
      try {
        const r = await api.get('jobs/' + job.id);
        current = r.job; onProgress && onProgress(r.job);
        if (done(r.job.state)) finish(r.job);
      } catch { /* keep waiting; live events may still arrive */ }
    }, 1500);
    if (done(job.state)) finish(job);
  });
}

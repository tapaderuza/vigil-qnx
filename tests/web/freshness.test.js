// Unit tests for the dashboard's freshness logic (web/index.html, between the FRESHNESS markers).
//
//     node --test tests/web
//
// The logic is extracted from the page itself, so what is tested is what ships. A simulated clock
// drives timeouts, so nothing here sleeps.
//
// SPDX-License-Identifier: MIT
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const html = fs.readFileSync(path.join(__dirname, "..", "..", "web", "index.html"), "utf8");
const m = html.match(/\/\* FRESHNESS:BEGIN[\s\S]*?\*\/([\s\S]*?)\/\* FRESHNESS:END \*\//);
assert.ok(m, "FRESHNESS block not found in web/index.html");
const lib = new Function(m[1] + "\nreturn { FRESH, newPanel, acceptSample, liveness, pollOnce };")();

const settle = () => new Promise((r) => setImmediate(r));

/** A fake clock with timers; advance() runs due timers in order and lets promises settle between them. */
function makeClock() {
  let now = 0, id = 0;
  const timers = new Map();
  return {
    now: () => now,
    setTimeout: (f, ms) => { timers.set(++id, { at: now + ms, f }); return id; },
    clearTimeout: (t) => timers.delete(t),
    async advance(ms) {
      const end = now + ms;
      for (;;) {
        await settle();
        const due = [...timers.entries()].filter(([, t]) => t.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
        if (!due) break;
        timers.delete(due[0]); now = due[1].at; due[1].f();
      }
      now = end; await settle();
    },
  };
}

function picture(seq) {
  return { seq, t_ms: seq * 100, highest: "MEDIUM", beds: [{ bed: 1, online: true, news2: { total: 5, band: "MEDIUM" } }] };
}

/** env.fetch delegates to script.current(signal), so each phase of a test can change the daemon's behaviour. */
function makeEnv(clock, script) {
  return {
    AbortController,
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    now: clock.now, wall: () => 1_700_000_000_000 + clock.now(),
    fetch: (url, opts) => script.current(opts.signal),
  };
}
const ok = (p) => Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(p) });
const httpError = (code) => Promise.resolve({ ok: false, status: code, json: () => Promise.reject(new Error("no body")) });
const hang = (signal) => new Promise((_, reject) => signal.addEventListener("abort", () => reject(new Error("aborted"))));

/** Polls every pollMs of simulated time for `ms`, returning the liveness observed after each poll. */
async function phase(ctx, ms, handler) {
  const { st, env, clock, script } = ctx;
  script.current = handler;
  const end = clock.now() + ms, seen = [];
  while (clock.now() < end) {
    const p = lib.pollOnce(st, env);
    await clock.advance(1);
    await p.catch(() => {});
    seen.push(lib.liveness(st, clock.now()));
    await clock.advance(st.cfg.pollMs);
  }
  return seen;
}
function context(cfg) {
  const clock = makeClock(), script = {}, st = lib.newPanel(cfg || {});
  return { clock, script, st, env: makeEnv(clock, script) };
}

test("normal connection: every poll with a moving sequence is fresh and the panel is live", async () => {
  const ctx = context();
  let seq = 0;
  const seen = await phase(ctx, 5000, () => ok(picture(++seq)));
  assert.ok(seen.length >= 9);
  assert.ok(seen.every((s) => s.state === "live"));
  assert.equal(ctx.st.errors, 0);
  assert.equal(ctx.st.lastError, "");
});

test("before any sample the panel is 'connecting', never live", () => {
  const st = lib.newPanel({});
  assert.deepEqual(lib.liveness(st, 12345), { state: "connecting", ageMs: null });
});

test("HTTP error: values go historic after the stale limit, keeping the time of the last valid update", async () => {
  const ctx = context({ staleMs: 3000 });
  let seq = 0;
  await phase(ctx, 2000, () => ok(picture(++seq)));
  const lastOkWall = ctx.st.lastOkWall, lastPic = ctx.st.pic;
  assert.ok(lastOkWall > 0);
  const seen = await phase(ctx, 6000, () => httpError(503));
  assert.equal(seen[0].state, "live");                          // one failed poll is not yet an outage
  assert.equal(seen[seen.length - 1].state, "stale");
  assert.ok(seen[seen.length - 1].ageMs > 3000);
  assert.equal(ctx.st.lastOkWall, lastOkWall);                  // when the last valid update arrived is kept
  assert.equal(ctx.st.pic, lastPic);                            // and so is the picture, to be shown as historic
  assert.match(ctx.st.lastError, /HTTP 503/);
});

test("a request that hangs is abandoned after the timeout, and the panel goes stale", async () => {
  const ctx = context({ staleMs: 3000, timeoutMs: 2000 });
  let seq = 0;
  await phase(ctx, 1500, () => ok(picture(++seq)));
  ctx.script.current = hang;
  const p = lib.pollOnce(ctx.st, ctx.env);
  await ctx.clock.advance(1999);
  assert.equal(ctx.st.errors, 0, "still waiting inside the timeout");
  await ctx.clock.advance(2);
  assert.equal((await p).fresh, false);
  assert.match(ctx.st.lastError, /no answer within 2 s/);
  await ctx.clock.advance(4000);
  assert.equal(lib.liveness(ctx.st, ctx.clock.now()).state, "stale");
});

test("a body that never finishes after the headers is also abandoned", async () => {
  const ctx = context({ timeoutMs: 1000 });
  ctx.script.current = (signal) => Promise.resolve({
    ok: true, status: 200,
    json: () => new Promise((_, rej) => signal.addEventListener("abort", () => rej(new Error("aborted")))),
  });
  const p = lib.pollOnce(ctx.st, ctx.env);
  await ctx.clock.advance(1100);
  assert.equal((await p).fresh, false);
  assert.match(ctx.st.lastError, /no answer within 1 s/);
});

test("a frozen sequence is not fresh, even though the server keeps answering 200", async () => {
  const ctx = context({ staleMs: 3000 });
  let seq = 0;
  await phase(ctx, 2000, () => ok(picture(++seq)));
  const frozen = picture(seq);
  const seen = await phase(ctx, 6000, () => ok(frozen));
  assert.equal(seen[seen.length - 1].state, "stale");
  assert.match(ctx.st.lastError, /same sequence/);
  assert.equal(ctx.st.restarts, 0);
});

test("recovery: the first fresh sample after an outage makes the panel live again", async () => {
  const ctx = context({ staleMs: 3000 });
  let seq = 0;
  await phase(ctx, 2000, () => ok(picture(++seq)));
  await phase(ctx, 5000, () => httpError(503));
  assert.equal(lib.liveness(ctx.st, ctx.clock.now()).state, "stale");
  const seen = await phase(ctx, 1000, () => ok(picture(++seq + 100)));
  assert.equal(seen[seen.length - 1].state, "live");
  assert.equal(ctx.st.lastError, "");
  assert.equal(ctx.st.restarts, 0);
});

test("daemon restart: counters starting again from 1 count as fresh and flag the restart", async () => {
  const ctx = context({ staleMs: 3000 });
  let seq = 5000;
  await phase(ctx, 2000, () => ok(picture(++seq)));
  await phase(ctx, 4000, () => httpError(503));                 // the daemon is down
  assert.equal(lib.liveness(ctx.st, ctx.clock.now()).state, "stale");
  let again = 0;
  const seen = await phase(ctx, 1500, () => ok(picture(++again)));
  assert.equal(seen[seen.length - 1].state, "live");
  assert.equal(ctx.st.restarts, 1);
  assert.ok(ctx.st.pic.seq < 10);
});

test("malformed answers are never fresh", () => {
  const st = lib.newPanel({});
  for (const bad of [null, {}, { seq: "1", t_ms: 2, beds: [] }, { seq: 1, t_ms: 2 }, { seq: 1, t_ms: 2, beds: {} }]) {
    assert.equal(lib.acceptSample(st, bad, 0, 0).fresh, false);
  }
  assert.equal(st.lastOkAt, null);
});

test("the boundary: exactly the stale limit is still live, one millisecond later it is not", () => {
  const st = lib.newPanel({ staleMs: 3000 });
  lib.acceptSample(st, picture(1), 1000, 5);
  assert.equal(lib.liveness(st, 4000).state, "live");
  assert.equal(lib.liveness(st, 4001).state, "stale");
});

test("a stale panel never produces a 'Stable' verdict: the page maps stale to 'No live data'", () => {
  assert.match(html, /historic \? "No live data"/);
  assert.match(html, /historic \? "HISTORIC"/);
});

import { createExecutionContext, createScheduledController, env, waitOnExecutionContext } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { afterEach, describe, expect, it, vi } from "vitest";
import example from "../examples/report-format-1.json";
import worker from "../src/index.js";
import { senderAddress, senderTag } from "../src/quota.js";

// The addresses here are apart from receiver.test.js's, and each test uses
// its own, so the rate limiter's counters of one test do not reach another.
let nextIp = 1;
function freshIp() {
  return `198.18.0.${nextIp++}`;
}

function post(body, ip = freshIp()) {
  return exports.default.fetch("https://bench.happydwarf.dev/reports", {
    method: "POST",
    headers: { "content-type": "application/json", "cf-connecting-ip": ip },
    body: typeof body === "string" ? body : JSON.stringify(body),
  });
}

function report() {
  const r = structuredClone(example);
  r.series_id = crypto.randomUUID();
  return r;
}

function utf8Length(text) {
  return new TextEncoder().encode(text).byteLength;
}

function today() {
  return new Date().toISOString().slice(0, 10);
}

async function setCounts(sender, received, bytes = 0) {
  await env.DB.prepare("INSERT INTO daily_counts (day, sender, received, bytes) VALUES (?1, ?2, ?3, ?4)")
    .bind(today(), sender, received, bytes)
    .run();
}

async function counts() {
  const { results } = await env.DB.prepare("SELECT sender, received, bytes FROM daily_counts WHERE day = ?1 ORDER BY sender")
    .bind(today())
    .all();
  return results;
}

async function storedBytes() {
  return (await env.DB.prepare("SELECT bytes FROM stored_bytes").first()).bytes;
}

afterEach(() => vi.restoreAllMocks());

describe("the sender", () => {
  it("is an IPv4 address, or the /64 of an IPv6 one", () => {
    expect(senderAddress("192.0.2.1")).toBe("192.0.2.1");
    expect(senderAddress("2001:db8:1:2:3:4:5:6")).toBe("2001:db8:1:2::/64");
    expect(senderAddress("2001:DB8:1:2::9")).toBe("2001:db8:1:2::/64");
    expect(senderAddress("2001:0db8:0001:0002:ffff::")).toBe("2001:db8:1:2::/64");
    expect(senderAddress("2001:db8::")).toBe("2001:db8:0:0::/64");
    expect(senderAddress("::ffff:192.0.2.1")).toBe("192.0.2.1");
    expect(senderAddress("64:ff9b::192.0.2.1")).toBe("64:ff9b:0:0::/64");
    expect(senderAddress("unknown")).toBe("unknown");
    expect(senderAddress("1::2::3")).toBe("1::2::3");
  });

  it("is stored as a keyed hash that changes every day", async () => {
    const tag = await senderTag("key", "2001:db8:1:2::9", "2026-11-21");
    expect(tag).toMatch(/^[0-9a-f]{32}$/);
    expect(await senderTag("key", "2001:db8:1:2:aaaa::1", "2026-11-21")).toBe(tag);
    expect(await senderTag("key", "2001:db8:1:3::9", "2026-11-21")).not.toBe(tag);
    expect(await senderTag("key", "2001:db8:1:2::9", "2026-11-22")).not.toBe(tag);
    expect(await senderTag("another key", "2001:db8:1:2::9", "2026-11-21")).not.toBe(tag);
    await expect(senderTag(undefined, "192.0.2.1", "2026-11-21")).rejects.toThrow(/QUOTA_KEY/);
  });

  it("is counted without the address", async () => {
    const ip = freshIp();
    const body = JSON.stringify(report());
    expect((await post(body, ip)).status).toBe(201);
    const tag = await senderTag(env.QUOTA_KEY, ip, today());
    expect(await counts()).toEqual([
      { sender: "", received: 1, bytes: utf8Length(body) },
      { sender: tag, received: 1, bytes: utf8Length(body) },
    ]);
    const { results } = await env.DB.prepare("SELECT * FROM daily_counts").all();
    expect(JSON.stringify(results)).not.toContain(ip);
  });
});

describe("check 4: per sender, per day", () => {
  it("allows 20 reports from one sender, then answers 429", async () => {
    const ip = freshIp();
    await setCounts(await senderTag(env.QUOTA_KEY, ip, today()), 19);
    expect((await post(report(), ip)).status).toBe(201);
    const log = vi.spyOn(console, "log");
    const refused = await post(report(), ip);
    expect(refused.status).toBe(429);
    expect((await refused.json()).message).toBe("This address has sent all the reports it can today. Try again tomorrow.");
    expect(JSON.parse(log.mock.calls[0][0])).toMatchObject({ status: 429, limit: "sender" });
    // Another sender is not affected, nor the count of all senders.
    expect((await post(report())).status).toBe(201);
    expect((await counts()).find((row) => row.sender === "").received).toBe(2);
  });

  it("allows 2 MiB from one sender, the report that would pass it refused", async () => {
    const ip = freshIp();
    const body = JSON.stringify(report());
    const limit = Number(env.SENDER_DAILY_BYTES_LIMIT);
    await setCounts(await senderTag(env.QUOTA_KEY, ip, today()), 1, limit - utf8Length(body) + 1);
    expect((await post(body, ip)).status).toBe(429);
    const exact = JSON.stringify(report());
    await env.DB.prepare("UPDATE daily_counts SET bytes = ?1 WHERE sender <> ''").bind(limit - utf8Length(exact)).run();
    expect((await post(exact, ip)).status).toBe(201);
  });

  it("shares one quota across an IPv6 /64", async () => {
    await setCounts(await senderTag(env.QUOTA_KEY, "2001:db8:5:6::1", today()), 20);
    expect((await post(report(), "2001:db8:5:6:abcd::7")).status).toBe(429);
    expect((await post(report(), "2001:db8:5:7::1")).status).toBe(201);
  });

  it("takes nothing for a repeat or a refused report", async () => {
    const ip = freshIp();
    const r = report();
    expect((await post(r, ip)).status).toBe(201);
    await env.DB.prepare("UPDATE daily_counts SET received = 20 WHERE sender <> ''").run();
    // The quota is used up, and a repeat still learns that its report arrived.
    expect((await post(r, ip)).status).toBe(200);
    const bad = report();
    bad.starts[0].time_ms = 1;
    expect((await post(bad, freshIp())).status).toBe(422);
    expect((await counts()).map((row) => row.received)).toEqual([1, 20]);
  });

  it("answers 503 without QUOTA_KEY and stores nothing", async () => {
    const r = report();
    const request = new Request("https://bench.happydwarf.dev/reports", {
      method: "POST",
      headers: { "content-type": "application/json", "cf-connecting-ip": freshIp() },
      body: JSON.stringify(r),
    });
    const response = await worker.fetch(request, { ...env, QUOTA_KEY: undefined });
    expect(response.status).toBe(503);
    expect(await env.DB.prepare("SELECT count(*) AS n FROM reports").first()).toEqual({ n: 0 });
  });
});

describe("check 4: the stored total", () => {
  it("answers 503 once the stored bytes would pass 1 GiB", async () => {
    const body = JSON.stringify(report());
    await env.DB.prepare("UPDATE stored_bytes SET bytes = ?1").bind(Number(env.TOTAL_BYTES_LIMIT) - utf8Length(body) + 1).run();
    const log = vi.spyOn(console, "log");
    const refused = await post(body);
    expect(refused.status).toBe(503);
    expect((await refused.json()).message).toBe("The receiver has no room for more reports. Try again later.");
    expect(JSON.parse(log.mock.calls[0][0])).toMatchObject({ status: 503, limit: "total" });
    // Nothing was taken from the day's counts.
    expect((await counts()).map((row) => [row.received, row.bytes])).toEqual([
      [0, 0],
      [0, 0],
    ]);
  });

  it("follows the stored reports: inserts, deletes by hand and retention", async () => {
    const first = JSON.stringify(report());
    const second = JSON.stringify(report(), null, 1);
    expect((await post(first)).status).toBe(201);
    expect((await post(second)).status).toBe(201);
    expect(await storedBytes()).toBe(utf8Length(first) + utf8Length(second));

    await env.DB.prepare("DELETE FROM reports WHERE series_id = ?1").bind(JSON.parse(first).series_id).run();
    expect(await storedBytes()).toBe(utf8Length(second));

    await env.DB.prepare("UPDATE reports SET received_day = '2000-01-01'").run();
    const controller = createScheduledController({ scheduledTime: Date.now(), cron: "17 3 * * *" });
    const ctx = createExecutionContext();
    await worker.scheduled(controller, env, ctx);
    await waitOnExecutionContext(ctx);
    expect(await storedBytes()).toBe(0);
  });

  it("counts bytes, not characters", async () => {
    const r = report();
    r.system.cpu = "Ryzen™ 7 – ünïcode";
    const body = JSON.stringify(r);
    expect((await post(body)).status).toBe(201);
    expect(await storedBytes()).toBe(utf8Length(body));
    expect(utf8Length(body)).toBeGreaterThan(body.length);
  });
});

describe("check 4: requests in flight together (N6)", () => {
  it("never passes the day's limit of all senders", async () => {
    await setCounts("", Number(env.DAILY_LIMIT) - 3);
    const responses = await Promise.all(Array.from({ length: 10 }, () => post(report())));
    const statuses = responses.map((r) => r.status).sort();
    expect(statuses).toEqual([201, 201, 201, 503, 503, 503, 503, 503, 503, 503]);
    expect((await counts()).find((row) => row.sender === "").received).toBe(Number(env.DAILY_LIMIT));
    expect(await env.DB.prepare("SELECT count(*) AS n FROM reports").first()).toEqual({ n: 3 });
  });

  it("never passes one sender's quota", async () => {
    const ip = freshIp();
    const tag = await senderTag(env.QUOTA_KEY, ip, today());
    await setCounts(tag, Number(env.SENDER_DAILY_LIMIT) - 2);
    // 5 at once, the rate limiter's allowance per minute.
    const responses = await Promise.all(Array.from({ length: 5 }, () => post(report(), ip)));
    expect(responses.map((r) => r.status).sort()).toEqual([201, 201, 429, 429, 429]);
    const rows = await counts();
    expect(rows.find((row) => row.sender === tag).received).toBe(Number(env.SENDER_DAILY_LIMIT));
    expect(rows.find((row) => row.sender === "").received).toBe(2);
  });

  it("counts a series sent twice at once once", async () => {
    const body = JSON.stringify(report());
    const responses = await Promise.all([post(body), post(body), post(body)]);
    expect(responses.map((r) => r.status).sort()).toEqual([200, 200, 201]);
    expect((await counts()).find((row) => row.sender === "")).toEqual({ sender: "", received: 1, bytes: utf8Length(body) });
  });
});

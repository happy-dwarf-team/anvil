import { describe, expect, it } from "vitest";
import example from "../examples/report-format-1.json";
import managerStopped from "./fixtures/manager-report-stopped.json";
import { acceptedFormats, checkReport, repeatsKey } from "../src/report.js";

const limits = { minStartMs: 1000, maxStartMs: 600000 };

function check(change) {
  const r = structuredClone(example);
  change(r);
  return checkReport(r, limits);
}

describe("repeatsKey", () => {
  const repeats = (text) => repeatsKey(text, JSON.parse(text));

  it("finds a key named twice in any object", () => {
    expect(repeats('{"a":1,"a":2}')).toBe(true);
    expect(repeats('{"a":{"b":1,"b":1}}')).toBe(true);
    expect(repeats('[{"x":1},{"y":[{"z":1,"z":2}]}]')).toBe(true);
    // The same key after an escape is the same key.
    expect(repeats('{"a":1,"\\u0061":2}')).toBe(true);
    expect(repeats('{"__proto__":1,"__proto__":2}')).toBe(true);
  });

  it("passes keys that differ, and colons and quotes inside strings", () => {
    expect(repeats('{"a":1,"b":{"a":2}}')).toBe(false);
    expect(repeats('[{"a":1},{"a":2}]')).toBe(false);
    expect(repeats('{"a:b":"c:d","e\\":":"\\\\:","f":[":",{"g":"\\"h:"}]}')).toBe(false);
    expect(repeats('{"__proto__":{"constructor":null}}')).toBe(false);
    expect(repeats(' "a:b" ')).toBe(false);
    expect(repeats("[1, 2, null]")).toBe(false);
    // Deeper than a recursive walk's stack.
    const deep = "[".repeat(100000) + '{"a":1}' + "]".repeat(100000);
    expect(repeats(deep)).toBe(false);
    expect(repeats(JSON.stringify(example))).toBe(false);
    expect(repeats(JSON.stringify(managerStopped, null, 2))).toBe(false);
  });
});

describe("checkReport", () => {
  it("accepts format 1 only", () => {
    expect(acceptedFormats()).toEqual([1]);
    expect(checkReport(example, limits)).toEqual({ ok: true, format: 1 });
    expect(checkReport([], limits).ok).toBe(false);
    expect(checkReport({ format: "1" }, limits).format).toBe(null);
  });

  // The Manager's own reports: anvil_manager_tests builds the example's
  // document from its series, and writes this stopped series byte for byte
  // (tests/manager_benchmark_tests.cpp).
  it("accepts the Manager's report of a stopped series", () => {
    expect(checkReport(managerStopped, limits)).toEqual({ ok: true, format: 1 });
  });

  it("ties the stop reason to the series state", () => {
    expect(check((r) => (r.series.stop_reason = "player")).ok).toBe(false);
    expect(check((r) => (r.series.state = "stopped")).ok).toBe(false);
  });

  it("ties the discard reason to validity", () => {
    expect(check((r) => (r.starts[1].discard_reason = null)).ok).toBe(false);
    expect(check((r) => (r.starts[0].discard_reason = "crash")).ok).toBe(false);
    expect(check((r) => (r.starts[0].time_ms = null)).ok).toBe(false);
  });

  it("needs the start positions in order", () => {
    const result = check((r) => (r.starts[4].position = 7));
    expect(result.message).toMatch(/starts\[4\] has position 7, expected 5/);
  });

  it("allows null for any plugin field and a participant without fields", () => {
    expect(check((r) => (r.starts[2].plugins.bellows.fields["open_warmup.mode"] = null)).ok).toBe(true);
    expect(check((r) => (r.starts[2].plugins.bellows.fields = {})).ok).toBe(true);
  });

  it("checks each kind", () => {
    const kinds = { "f.int": { kind: "int" }, "f.number": { kind: "number" }, "f.flag": { kind: "flag" } };
    const withFields = (fields) =>
      check((r) => {
        Object.assign(r.plugin_schemas.bellows, kinds);
        Object.assign(r.starts[2].plugins.bellows.fields, fields);
      });
    expect(withFields({ "f.int": 3, "f.number": 2.5, "f.flag": true }).ok).toBe(true);
    expect(withFields({ "f.int": 2.5 }).ok).toBe(false);
    expect(withFields({ "f.number": "2" }).ok).toBe(false);
    expect(withFields({ "f.flag": 1 }).ok).toBe(false);
  });

  it("refuses string values the kinds keep out", () => {
    expect(check((r) => (r.starts[2].plugins.bellows.fields["attr_cache.hits"] = "C:\\Users\\me")).ok).toBe(false);
  });

  it("needs a field schema for every participant", () => {
    const result = check((r) => delete r.plugin_schemas.bellows);
    expect(result.message).toMatch(/bellows is a benchmark participant in start 2 but has no field schema/);
  });

  it("checks field declarations", () => {
    expect(check((r) => (r.plugin_schemas.bellows["x.y"] = { kind: "enum" })).ok).toBe(false);
    expect(check((r) => (r.plugin_schemas.bellows["x.y"] = { kind: "int", options: ["a"] })).ok).toBe(false);
    expect(check((r) => (r.plugin_schemas.bellows["x.y"] = { kind: "int", unit: "seconds" })).ok).toBe(false);
    expect(check((r) => (r.plugin_schemas.bellows["X.y"] = { kind: "int" })).ok).toBe(false);
    expect(check((r) => (r.plugin_schemas.bellows["a.b.c"] = { kind: "int" })).ok).toBe(false);
  });

  it("needs an empty warm group to have no median", () => {
    const empty = (r) => {
      for (const s of r.starts) if (s.arm === "with" && s.valid) s.cold_start.class = "unclear";
      r.result.warm.with = { n: 0, median_ms: null, min_ms: null, max_ms: null, times_ms: [] };
    };
    expect(check(empty).message).toMatch(/difference is given without warm starts in both arms/);
    expect(
      check((r) => {
        empty(r);
        r.result.difference = null;
      }).ok,
    ).toBe(true);
  });

  it("takes the median of an even group as the mean of the middle two", () => {
    const result = check((r) => {
      r.starts[0].cold_start.class = "warm";
      r.result.warm.without = {
        n: 4,
        median_ms: 41250,
        min_ms: 40900,
        max_ms: 52300,
        times_ms: [52300, 40900, 41500, 41000],
      };
      r.result.difference = { ms: -7450, percent: (-7450 / 41250) * 100, ranges_overlap: false };
    });
    expect(result).toEqual({ ok: true, format: 1 });
  });

  it("checks ranges_overlap", () => {
    expect(check((r) => (r.result.difference.ranges_overlap = true)).ok).toBe(false);
  });
});

describe("the largest series", () => {
  it("has 200 starts", () => {
    const r = structuredClone(example);
    r.starts = Array.from({ length: 200 }, (_, i) => ({ ...structuredClone(example.starts[2]), position: i + 1 }));
    r.result.warm.without = { n: 0, median_ms: null, min_ms: null, max_ms: null, times_ms: [] };
    r.result.warm.with = { n: 200, median_ms: 33800, min_ms: 33800, max_ms: 33800, times_ms: Array(200).fill(33800) };
    r.result.difference = null;
    expect(checkReport(r, limits)).toEqual({ ok: true, format: 1 });
    r.starts.push({ ...structuredClone(example.starts[2]), position: 201 });
    expect(checkReport(r, limits).ok).toBe(false);
  });
});

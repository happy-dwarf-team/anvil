import { describe, expect, it } from "vitest";
import example from "../examples/report-format-1.json";
import { acceptedFormats, checkReport } from "../src/report.js";

const limits = { minStartMs: 1000, maxStartMs: 600000 };

function check(change) {
  const r = structuredClone(example);
  change(r);
  return checkReport(r, limits);
}

describe("checkReport", () => {
  it("accepts format 1 only", () => {
    expect(acceptedFormats()).toEqual([1]);
    expect(checkReport(example, limits)).toEqual({ ok: true, format: 1 });
    expect(checkReport([], limits).ok).toBe(false);
    expect(checkReport({ format: "1" }, limits).format).toBe(null);
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

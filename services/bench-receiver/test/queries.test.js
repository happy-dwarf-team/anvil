import { env } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import example from "../examples/report-format-1.json";

// The saved queries, read as text by Vite at build time.
const queries = import.meta.glob("../queries/*.sql", { query: "?raw", import: "default", eager: true });

beforeEach(async () => {
  await env.DB.prepare("INSERT INTO reports (series_id, format, received_day, body) VALUES (?1, 1, '2026-10-25', ?2)")
    .bind(example.series_id, JSON.stringify(example))
    .run();
});

describe("views", () => {
  it("give one row per start", async () => {
    const { results } = await env.DB.prepare("SELECT position, arm, class, valid FROM starts_v1 ORDER BY position").all();
    expect(results).toHaveLength(8);
    expect(results[0]).toEqual({ position: 1, arm: "without", class: "cold", valid: 1 });
  });

  it("give one row per plugin field value", async () => {
    const row = await env.DB.prepare(
      "SELECT value FROM plugin_fields_v1 WHERE plugin = 'bellows' AND field = 'attr_cache.hits' AND position = 3",
    ).first();
    expect(row.value).toBe(48210);
  });

  it("give the plugin states", async () => {
    const row = await env.DB.prepare("SELECT state, detail FROM plugin_states_v1 WHERE plugin = 'bellows' AND position = 1").first();
    expect(row).toEqual({ state: "disabled", detail: "arm" });
  });

  it("give the result", async () => {
    const row = await env.DB.prepare("SELECT difference_ms, cold_starts, drive_game FROM results_v1").first();
    expect(row).toEqual({ difference_ms: -7200, cold_starts: 1, drive_game: "nvme" });
  });
});

describe("saved queries", () => {
  it("are all there", () => {
    expect(Object.keys(queries).length).toBeGreaterThan(0);
  });

  for (const [path, sql] of Object.entries(queries)) {
    it(`${path} runs`, async () => {
      const { results } = await env.DB.prepare(sql).all();
      expect(results.length).toBeGreaterThan(0);
    });
  }
});

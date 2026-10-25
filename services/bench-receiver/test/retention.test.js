import { createExecutionContext, createScheduledController, env, waitOnExecutionContext } from "cloudflare:test";
import { describe, expect, it } from "vitest";
import worker from "../src/index.js";
import { monthsBefore } from "../src/retention.js";

async function insert(seriesId, day) {
  await env.DB.prepare("INSERT INTO reports (series_id, format, received_day, body) VALUES (?1, 1, ?2, '{}')")
    .bind(seriesId, day)
    .run();
}

describe("retention", () => {
  it("goes back whole months, clamped to a shorter month", () => {
    expect(monthsBefore(new Date("2026-10-25T03:17:00Z"), 24)).toBe("2024-10-25");
    expect(monthsBefore(new Date("2026-02-28T00:00:00Z"), 24)).toBe("2024-02-28");
    expect(monthsBefore(new Date("2026-03-31T00:00:00Z"), 1)).toBe("2026-02-28");
    expect(monthsBefore(new Date("2026-01-15T00:00:00Z"), 13)).toBe("2024-12-15");
  });

  it("deletes reports received more than 24 months ago and past daily counts", async () => {
    await insert("old", "2024-10-24");
    await insert("kept", "2024-10-25");
    await env.DB.prepare("INSERT INTO daily_counts (day, received) VALUES ('2026-10-24', 3), ('2026-10-25', 1)").run();

    const controller = createScheduledController({ scheduledTime: Date.parse("2026-10-25T03:17:00Z"), cron: "17 3 * * *" });
    const ctx = createExecutionContext();
    await worker.scheduled(controller, env, ctx);
    await waitOnExecutionContext(ctx);

    const { results: reports } = await env.DB.prepare("SELECT series_id FROM reports").all();
    expect(reports.map((r) => r.series_id)).toEqual(["kept"]);
    const { results: days } = await env.DB.prepare("SELECT day FROM daily_counts").all();
    expect(days.map((d) => d.day)).toEqual(["2026-10-25"]);
  });
});

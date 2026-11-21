import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach } from "vitest";

await applyD1Migrations(env.DB, env.TEST_MIGRATIONS);

// D1 storage is shared by the tests of a file; each test starts empty.
beforeEach(async () => {
  // The triggers bring stored_bytes back to 0 with the reports; a test may
  // have set it by hand.
  await env.DB.batch([
    env.DB.prepare("DELETE FROM reports"),
    env.DB.prepare("DELETE FROM daily_counts"),
    env.DB.prepare("UPDATE stored_bytes SET bytes = 0"),
  ]);
});

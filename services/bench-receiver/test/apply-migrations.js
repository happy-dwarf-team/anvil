import { applyD1Migrations, env } from "cloudflare:test";
import { beforeEach } from "vitest";

await applyD1Migrations(env.DB, env.TEST_MIGRATIONS);

// D1 storage is shared by the tests of a file; each test starts empty.
beforeEach(async () => {
  await env.DB.batch([env.DB.prepare("DELETE FROM reports"), env.DB.prepare("DELETE FROM daily_counts")]);
});

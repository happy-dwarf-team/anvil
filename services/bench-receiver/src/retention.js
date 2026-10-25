// Retention: reports are kept 24 months from the day of receipt
// (docs/spec/benchmark-v1.md, section 9).

// Deletes reports received before the retention period and the counts of past
// days. Returns the number of reports deleted.
export async function deleteExpired(env, now) {
  const today = now.toISOString().slice(0, 10);
  const cutoff = monthsBefore(now, Number(env.RETENTION_MONTHS));
  const [reports] = await env.DB.batch([
    env.DB.prepare("DELETE FROM reports WHERE received_day < ?1").bind(cutoff),
    env.DB.prepare("DELETE FROM daily_counts WHERE day < ?1").bind(today),
  ]);
  return reports.meta.changes;
}

// The UTC date `months` before `now`, clamped to the end of a shorter month.
export function monthsBefore(now, months) {
  const total = now.getUTCFullYear() * 12 + now.getUTCMonth() - months;
  const year = Math.floor(total / 12);
  const month = total - year * 12;
  const lastDay = new Date(Date.UTC(year, month + 1, 0)).getUTCDate();
  return new Date(Date.UTC(year, month, Math.min(now.getUTCDate(), lastDay))).toISOString().slice(0, 10);
}

// Check 4: the daily quota per sender, the daily limits of all senders and the
// cap on stored bytes (anvil#539), taken in one statement before the insert so
// that requests in flight together cannot pass them.

const encoder = new TextEncoder();

// The part of an address that one sender controls: an IPv4 address, or the
// /64 of an IPv6 address, which one connection usually gets whole.
export function senderAddress(ip) {
  if (!ip.includes(":")) return ip;
  const lower = ip.toLowerCase();
  const halves = lower.split("::");
  if (halves.length > 2) return lower;
  // A dotted IPv4 tail stands for the last two groups.
  const dotted = lower.includes(".") ? lower.slice(lower.lastIndexOf(":") + 1) : null;
  const groups = (part) => (part ? part.split(":").flatMap((g) => (g.includes(".") ? ["0", "0"] : [g])) : []);
  const head = groups(halves[0]);
  const tail = halves.length === 2 ? groups(halves[1]) : [];
  const all = halves.length === 2 ? [...head, ...Array(Math.max(0, 8 - head.length - tail.length)).fill("0"), ...tail] : head;
  if (all.length !== 8 || !all.every((g) => /^[0-9a-f]{1,4}$/.test(g))) return lower;
  const values = all.map((g) => parseInt(g, 16));
  // An IPv4-mapped address (::ffff:192.0.2.1) is that IPv4 sender.
  if (dotted !== null && values.slice(0, 5).every((v) => v === 0) && values[5] === 0xffff) return dotted;
  return values.slice(0, 4).map((v) => v.toString(16)).join(":") + "::/64";
}

async function hmac(key, message) {
  const k = await crypto.subtle.importKey("raw", key, { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return new Uint8Array(await crypto.subtle.sign("HMAC", k, encoder.encode(message)));
}

// The sender's row in daily_counts: an HMAC of the sender's address under a key
// of the day, itself an HMAC of the day under the QUOTA_KEY secret. The address
// is not stored, the same address gives another value the next day, and the
// database alone does not let anyone test addresses against the values.
export async function senderTag(secret, ip, day) {
  if (!secret) throw new Error("QUOTA_KEY is not set");
  const dayKey = await hmac(encoder.encode(secret), `bench-receiver sender ${day}`);
  const tag = await hmac(dayKey, senderAddress(ip));
  return Array.from(tag.subarray(0, 16), (b) => b.toString(16).padStart(2, "0")).join("");
}

// Takes one report of `size` bytes from the day's count of all senders and of
// this sender, both or neither, while every limit allows it. One UPDATE, so
// D1, which runs one statement at a time, cannot interleave another request's
// between the check and the count. Returns null when taken, otherwise the
// limit that refused: "sender", "day" or "total".
export async function reserve(db, { day, sender, size, limits }) {
  const [, taken] = await db.batch([
    db
      .prepare("INSERT OR IGNORE INTO daily_counts (day, sender, received, bytes) VALUES (?1, '', 0, 0), (?1, ?2, 0, 0)")
      .bind(day, sender),
    db
      .prepare(
        "UPDATE daily_counts SET received = received + 1, bytes = bytes + ?3 " +
          "WHERE day = ?1 AND sender IN ('', ?2) " +
          "AND (SELECT count(*) FROM daily_counts WHERE day = ?1 AND (" +
          "(sender = '' AND received < ?4 AND bytes + ?3 <= ?5) OR " +
          "(sender = ?2 AND received < ?6 AND bytes + ?3 <= ?7))) = 2 " +
          "AND (SELECT bytes FROM stored_bytes WHERE id = 1) + ?3 <= ?8 " +
          "RETURNING sender",
      )
      .bind(
        day,
        sender,
        size,
        limits.dailyLimit,
        limits.dailyBytesLimit,
        limits.senderDailyLimit,
        limits.senderDailyBytesLimit,
        limits.totalBytesLimit,
      ),
  ]);
  if (taken.results.length === 2) return null;

  // Refused: name the limit, the store's first.
  const [counts, stored] = await db.batch([
    db.prepare("SELECT sender, received, bytes FROM daily_counts WHERE day = ?1 AND sender IN ('', ?2)").bind(day, sender),
    db.prepare("SELECT bytes FROM stored_bytes WHERE id = 1"),
  ]);
  if ((stored.results[0]?.bytes ?? 0) + size > limits.totalBytesLimit) return "total";
  const all = counts.results.find((row) => row.sender === "");
  if (!all || all.received >= limits.dailyLimit || all.bytes + size > limits.dailyBytesLimit) return "day";
  return "sender";
}

// Gives back a report taken by reserve() that was not stored after all.
export async function release(db, { day, sender, size }) {
  await db
    .prepare("UPDATE daily_counts SET received = received - 1, bytes = bytes - ?3 WHERE day = ?1 AND sender IN ('', ?2)")
    .bind(day, sender, size)
    .run();
}

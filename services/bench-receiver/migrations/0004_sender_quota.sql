-- The daily quota per sender and the cap on stored bytes (anvil#539).
--
-- daily_counts gets a row per sender beside the row of all senders, so that
-- one UPDATE can take a report from both counts at once or from neither.
-- `sender` is '' for all senders, otherwise a keyed hash of the address whose
-- key changes every UTC day (src/quota.js); the address itself is not stored.
-- The Cron Trigger deletes past days, so a sender's row lives one day.
CREATE TABLE daily_counts_by_sender (
  day TEXT NOT NULL, -- UTC date, YYYY-MM-DD
  sender TEXT NOT NULL DEFAULT '',
  received INTEGER NOT NULL,
  bytes INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (day, sender)
) STRICT, WITHOUT ROWID;

INSERT INTO daily_counts_by_sender (day, sender, received, bytes)
SELECT day, '', received, bytes FROM daily_counts;

DROP TABLE daily_counts;

ALTER TABLE daily_counts_by_sender RENAME TO daily_counts;

-- The bytes of every stored report body, kept by the triggers below, so the
-- total cap reads one row instead of summing the table. The triggers also see
-- reports deleted by hand and by the Cron Trigger.
CREATE TABLE stored_bytes (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  bytes INTEGER NOT NULL
) STRICT;

INSERT INTO stored_bytes (id, bytes) SELECT 1, coalesce(sum(length(CAST(body AS BLOB))), 0) FROM reports;

CREATE TRIGGER reports_stored_bytes_insert AFTER INSERT ON reports
BEGIN
  UPDATE stored_bytes SET bytes = bytes + length(CAST(NEW.body AS BLOB)) WHERE id = 1;
END;

CREATE TRIGGER reports_stored_bytes_update AFTER UPDATE OF body ON reports
BEGIN
  UPDATE stored_bytes SET bytes = bytes - length(CAST(OLD.body AS BLOB)) + length(CAST(NEW.body AS BLOB)) WHERE id = 1;
END;

CREATE TRIGGER reports_stored_bytes_delete AFTER DELETE ON reports
BEGIN
  UPDATE stored_bytes SET bytes = bytes - length(CAST(OLD.body AS BLOB)) WHERE id = 1;
END;

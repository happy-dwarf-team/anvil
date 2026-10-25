-- One row per benchmark series: the report's raw text byte for byte and the
-- day of receipt (docs/spec/benchmark-v1.md, section 9). The envelope columns
-- are generated from the body, so nothing but series_id, format,
-- received_day and body is ever written.
CREATE TABLE reports (
  series_id TEXT PRIMARY KEY NOT NULL,
  format INTEGER NOT NULL,
  received_day TEXT NOT NULL, -- UTC date, YYYY-MM-DD
  body TEXT NOT NULL,
  anvil_version TEXT GENERATED ALWAYS AS (json_extract(body, '$.versions.anvil')) VIRTUAL,
  compared_plugin TEXT GENERATED ALWAYS AS (json_extract(body, '$.series.compared.id')) VIRTUAL,
  compared_version TEXT GENERATED ALWAYS AS (json_extract(body, '$.series.compared.version')) VIRTUAL,
  series_state TEXT GENERATED ALWAYS AS (json_extract(body, '$.series.state')) VIRTUAL
) STRICT;

CREATE INDEX reports_received_day ON reports (received_day);
CREATE INDEX reports_anvil_version ON reports (anvil_version);
CREATE INDEX reports_compared ON reports (compared_plugin, compared_version);
CREATE INDEX reports_series_state ON reports (series_state);

-- The number of reports received per UTC day, for the daily limit. Reading
-- one row here is cheaper than counting the day's reports on every request.
-- The Cron Trigger deletes past days.
CREATE TABLE daily_counts (
  day TEXT PRIMARY KEY NOT NULL, -- UTC date, YYYY-MM-DD
  received INTEGER NOT NULL
) STRICT;

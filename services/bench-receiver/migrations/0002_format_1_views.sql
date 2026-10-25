-- Views over format 1 reports. They read the stored body with json_each; the
-- Worker does not split reports on receipt. A later format gets views of its
-- own, so queries over old reports keep working.

-- One row per start.
CREATE VIEW starts_v1 AS
SELECT
  r.series_id,
  r.received_day,
  r.anvil_version,
  r.compared_plugin,
  r.compared_version,
  json_extract(s.value, '$.position') AS position,
  json_extract(s.value, '$.arm') AS arm,
  json_extract(s.value, '$.time_ms') AS time_ms,
  json_extract(s.value, '$.valid') AS valid,
  json_extract(s.value, '$.discard_reason') AS discard_reason,
  json_extract(s.value, '$.killed') AS killed,
  json_extract(s.value, '$.cold_start.class') AS class,
  json_extract(s.value, '$.cold_start.first_since_boot') AS first_since_boot,
  json_extract(s.value, '$.cold_start.definitions_changed') AS definitions_changed,
  json_extract(s.value, '$.cold_start.open_cost.source') AS open_cost_source,
  json_extract(s.value, '$.cold_start.open_cost.files') AS open_cost_files,
  json_extract(s.value, '$.cold_start.open_cost.ms') AS open_cost_ms,
  json_extract(s.value, '$.input_during_start') AS input_during_start,
  json_extract(s.value, '$.crash.attribution') AS crash_attribution,
  json_extract(s.value, '$.crash.plugin') AS crash_plugin
FROM reports AS r, json_each(r.body, '$.starts') AS s
WHERE r.format = 1;

-- One row per plugin in each start, with its state.
CREATE VIEW plugin_states_v1 AS
SELECT
  r.series_id,
  json_extract(s.value, '$.position') AS position,
  json_extract(s.value, '$.arm') AS arm,
  p.key AS plugin,
  json_extract(p.value, '$.state') AS state,
  json_extract(p.value, '$.participant') AS participant,
  json_extract(p.value, '$.done') AS done,
  json_extract(p.value, '$.learning') AS learning,
  coalesce(json_extract(p.value, '$.by'), json_extract(p.value, '$.reason')) AS detail
FROM reports AS r, json_each(r.body, '$.starts') AS s, json_each(s.value, '$.plugins') AS p
WHERE r.format = 1;

-- One row per plugin field value in each start.
CREATE VIEW plugin_fields_v1 AS
SELECT
  r.series_id,
  r.compared_version,
  json_extract(s.value, '$.position') AS position,
  json_extract(s.value, '$.arm') AS arm,
  json_extract(s.value, '$.valid') AS valid,
  json_extract(s.value, '$.cold_start.class') AS class,
  p.key AS plugin,
  f.key AS field,
  f.value AS value
FROM reports AS r,
  json_each(r.body, '$.starts') AS s,
  json_each(s.value, '$.plugins') AS p,
  json_each(p.value, '$.fields') AS f
WHERE r.format = 1;

-- One row per series with the derived result and the system.
CREATE VIEW results_v1 AS
SELECT
  r.series_id,
  r.received_day,
  r.anvil_version,
  r.compared_plugin,
  r.compared_version,
  r.series_state,
  json_extract(r.body, '$.series.stop_reason') AS stop_reason,
  json_array_length(r.body, '$.starts') AS starts,
  json_extract(r.body, '$.result.warm.without.n') AS warm_without_n,
  json_extract(r.body, '$.result.warm.without.median_ms') AS warm_without_median_ms,
  json_extract(r.body, '$.result.warm.with.n') AS warm_with_n,
  json_extract(r.body, '$.result.warm.with.median_ms') AS warm_with_median_ms,
  json_extract(r.body, '$.result.difference.ms') AS difference_ms,
  json_extract(r.body, '$.result.difference.percent') AS difference_percent,
  json_extract(r.body, '$.result.difference.ranges_overlap') AS ranges_overlap,
  json_array_length(r.body, '$.result.cold.starts') AS cold_starts,
  json_extract(r.body, '$.system.cpu') AS cpu,
  json_extract(r.body, '$.system.logical_cores') AS logical_cores,
  json_extract(r.body, '$.system.ram_gb') AS ram_gb,
  json_extract(r.body, '$.system.drives.game') AS drive_game,
  json_extract(r.body, '$.system.drives.user') AS drive_user,
  json_extract(r.body, '$.system.drives.workshop') AS drive_workshop,
  json_extract(r.body, '$.system.windows_build') AS windows_build,
  json_extract(r.body, '$.mods.enabled') AS mods_enabled,
  json_extract(r.body, '$.mods.files') AS mod_files,
  json_extract(r.body, '$.mods.size_mb') AS mod_size_mb
FROM reports AS r
WHERE r.format = 1;

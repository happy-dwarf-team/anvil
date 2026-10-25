-- Cold starts one by one, with their signals and open cost per file.
SELECT series_id, compared_plugin, compared_version, position, arm, time_ms,
  first_since_boot, definitions_changed, open_cost_source, open_cost_files,
  round(open_cost_ms / nullif(open_cost_files, 0), 3) AS ms_per_file
FROM starts_v1
WHERE valid = 1 AND class = 'cold'
ORDER BY compared_plugin, compared_version DESC, series_id, position;

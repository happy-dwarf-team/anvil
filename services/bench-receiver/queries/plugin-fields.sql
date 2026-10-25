-- Every plugin field over valid warm starts: how often it is set, and the range
-- of numeric values or the count of each enum option and flag.
SELECT plugin, field, compared_version, arm,
  count(*) AS starts,
  count(value) AS set_in,
  CASE WHEN typeof(min(value)) IN ('integer', 'real') THEN round(avg(value), 1) END AS avg,
  CASE WHEN typeof(min(value)) IN ('integer', 'real') THEN min(value) END AS min,
  CASE WHEN typeof(max(value)) IN ('integer', 'real') THEN max(value) END AS max,
  CASE WHEN typeof(min(value)) = 'text' THEN group_concat(DISTINCT value) END AS options_seen
FROM plugin_fields_v1
WHERE valid = 1 AND class = 'warm'
GROUP BY plugin, field, compared_version, arm
ORDER BY plugin, field, compared_version DESC, arm;

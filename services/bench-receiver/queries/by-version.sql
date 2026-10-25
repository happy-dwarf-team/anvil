-- Per compared plugin version and Anvil version: how many series, and the warm
-- difference in percent (with the plugin minus without it).
SELECT compared_plugin, compared_version, anvil_version,
  count(*) AS series,
  sum(series_state = 'complete') AS complete,
  count(difference_percent) AS with_difference,
  round(avg(difference_percent), 1) AS avg_percent,
  round(min(difference_percent), 1) AS min_percent,
  round(max(difference_percent), 1) AS max_percent,
  sum(ranges_overlap = 0 AND difference_percent > 0) AS slower_clearly,
  sum(ranges_overlap = 1) AS overlapping
FROM results_v1
GROUP BY compared_plugin, compared_version, anvil_version
ORDER BY compared_plugin, compared_version DESC, anvil_version DESC;

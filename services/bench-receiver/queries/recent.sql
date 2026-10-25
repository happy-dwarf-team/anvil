-- The 50 most recent series with their result.
SELECT series_id, received_day, anvil_version, compared_plugin, compared_version, series_state, stop_reason,
  starts, warm_without_n, warm_with_n, warm_without_median_ms, warm_with_median_ms,
  difference_ms, round(difference_percent, 1) AS difference_percent, ranges_overlap, cold_starts
FROM results_v1
ORDER BY received_day DESC, series_id
LIMIT 50;

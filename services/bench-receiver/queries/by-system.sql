-- The warm difference by the game folder's drive kind and memory: where the
-- plugin helps least.
SELECT compared_plugin, compared_version, drive_game,
  CASE WHEN ram_gb < 16 THEN '<16' WHEN ram_gb < 32 THEN '16-31' ELSE '32+' END AS ram,
  count(*) AS series,
  round(avg(difference_percent), 1) AS avg_percent,
  round(max(difference_percent), 1) AS max_percent,
  round(avg(mods_enabled), 0) AS avg_mods
FROM results_v1
WHERE difference_percent IS NOT NULL
GROUP BY compared_plugin, compared_version, drive_game, ram
ORDER BY avg_percent DESC;

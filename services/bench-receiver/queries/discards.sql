-- Why series stop and starts do not count, per compared plugin version.
SELECT compared_plugin, compared_version, 'stop' AS what, stop_reason AS reason, count(*) AS n
FROM results_v1
WHERE series_state = 'stopped'
GROUP BY compared_plugin, compared_version, stop_reason
UNION ALL
SELECT compared_plugin, compared_version, 'discard', discard_reason, count(*)
FROM starts_v1
WHERE valid = 0
GROUP BY compared_plugin, compared_version, discard_reason
UNION ALL
SELECT compared_plugin, compared_version, 'unclear', arm, count(*)
FROM starts_v1
WHERE valid = 1 AND class = 'unclear'
GROUP BY compared_plugin, compared_version, arm
ORDER BY compared_plugin, compared_version DESC, what, n DESC;

-- Reports per day of receipt, against the daily limit.
SELECT received_day, count(*) AS reports, sum(length(body)) AS bytes
FROM reports
GROUP BY received_day
ORDER BY received_day DESC
LIMIT 60;

#!/bin/bash
# Doba job runner: runs every jobs/*.job dropped on this SSD (each in the
# background, output in logs/<job>.log). Close this window to stop it.
SSD="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$SSD/jobs" "$SSD/logs"
caffeinate -dimsu -w $$ &
echo "Doba runner on $SSD — waiting for jobs (close the window to stop)."
while true; do
  for j in "$SSD/jobs/"*.job; do
    [ -e "$j" ] || continue
    name="$(basename "$j" .job)"
    mv "$j" "$SSD/jobs/$name.running"
    echo "$(date '+%F %T')  start $name"
    ( bash "$SSD/jobs/$name.running" > "$SSD/logs/$name.log" 2>&1
      rc=$?
      echo "$rc" > "$SSD/logs/$name.exit"
      mv "$SSD/jobs/$name.running" "$SSD/jobs/$name.done"
      echo "$(date '+%F %T')  end $name (exit $rc)" ) &
  done
  sleep 10
done

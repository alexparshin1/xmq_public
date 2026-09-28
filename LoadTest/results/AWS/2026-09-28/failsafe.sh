#!/usr/bin/env bash
while pgrep -f "aws-2026-09-28/[e]mqx_rerun.sh" >/dev/null || pgrep -f "aws-2026-09-28/[e]mqx_persistent.sh" >/dev/null; do sleep 60; done
sleep 3600
echo "$(date) failsafe: shutting down server and client" >> ~/aws-2026-09-28/failsafe.log
ssh -o BatchMode=yes 172.31.13.230 sudo -n shutdown -h now
sudo -n shutdown -h now

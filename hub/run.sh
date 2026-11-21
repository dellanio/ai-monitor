#!/bin/bash
# hub-stack-chan (AI Monitor) v0.2
set -e
cd "$(dirname "$0")"
[ -f config.yaml ] || { echo "copie config.example.yaml para config.yaml e ajuste"; exit 1; }
exec python3 -m hub --config config.yaml "$@"

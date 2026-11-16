#!/bin/sh
set -eu
cd "$(dirname "$0")"
printf '\nBridge 网页管理端：http://127.0.0.1:8857\n请用 Chrome 或 Edge 打开。按 Ctrl+C 停止服务。\n\n'
exec python3 -m http.server 8857 --bind 127.0.0.1 --directory web

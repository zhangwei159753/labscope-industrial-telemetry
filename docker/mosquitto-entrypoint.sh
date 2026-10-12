#!/bin/sh
set -eu

: "${MOSQUITTO_ADMIN_USER:=labscope-admin}"
: "${MOSQUITTO_ADMIN_PASSWORD:?MOSQUITTO_ADMIN_PASSWORD must be set}"

data_dir=/mosquitto/data
config_file="$data_dir/dynamic-security.json"
mkdir -p "$data_dir" /mosquitto/log

if [ ! -s "$config_file" ]; then
    mosquitto_ctrl dynsec init "$config_file" "$MOSQUITTO_ADMIN_USER" "$MOSQUITTO_ADMIN_PASSWORD"
fi

chown -R 1883:1883 "$data_dir" /mosquitto/log
chmod 0750 "$data_dir" /mosquitto/log
chmod 0640 "$config_file"
exec mosquitto -c /mosquitto/config/mosquitto.conf

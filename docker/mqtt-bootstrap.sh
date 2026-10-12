#!/bin/sh
set -eu

: "${MOSQUITTO_ADMIN_USER:=labscope-admin}"
: "${MOSQUITTO_ADMIN_PASSWORD:?MOSQUITTO_ADMIN_PASSWORD must be set}"
: "${MOSQUITTO_INGEST_USER:=labscope-ingest}"
: "${MOSQUITTO_INGEST_PASSWORD:?MOSQUITTO_INGEST_PASSWORD must be set}"

if [ "$MOSQUITTO_ADMIN_USER" = "$MOSQUITTO_INGEST_USER" ]; then
    echo "MQTT admin and ingest users must be different" >&2
    exit 1
fi

dynsec() {
    mosquitto_ctrl -h mqtt -p 1883 \
        -u "$MOSQUITTO_ADMIN_USER" -P "$MOSQUITTO_ADMIN_PASSWORD" \
        dynsec "$@"
}

role=labscope-ingest
role_info=$(dynsec getRole "$role" 2>/dev/null || true)
if [ -z "$role_info" ]; then
    dynsec createRole "$role" >/dev/null
    role_info=$(dynsec getRole "$role")
fi

ensure_acl() {
    acl_type=$1
    topic=$2
    if ! printf '%s\n' "$role_info" | grep -F "$acl_type" | grep -F "$topic" | grep -Fq 'allow'; then
        dynsec addRoleACL "$role" "$acl_type" "$topic" allow 10 >/dev/null
        role_info=$(dynsec getRole "$role")
    fi
}

ensure_acl subscribePattern 'labscope/v1/sites/+/devices/+/telemetry'
ensure_acl publishClientSend 'labscope/v1/sites/+/devices/+/acks'

client_info=$(dynsec getClient "$MOSQUITTO_INGEST_USER" 2>/dev/null || true)
if [ -z "$client_info" ]; then
    dynsec createClient "$MOSQUITTO_INGEST_USER" -p "$MOSQUITTO_INGEST_PASSWORD" >/dev/null
    dynsec addClientRole "$MOSQUITTO_INGEST_USER" "$role" 10 >/dev/null
else
    if ! printf '%s\n' "$client_info" | grep -Fq "$role"; then
        dynsec addClientRole "$MOSQUITTO_INGEST_USER" "$role" 10 >/dev/null
    fi
    dynsec setClientPassword "$MOSQUITTO_INGEST_USER" "$MOSQUITTO_INGEST_PASSWORD" >/dev/null
    dynsec enableClient "$MOSQUITTO_INGEST_USER" >/dev/null
fi

echo "Mosquitto ingest identity is ready"

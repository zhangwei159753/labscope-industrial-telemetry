#!/usr/bin/env python3
"""Provision, rotate, or revoke a device in PostgreSQL and Mosquitto dynsec."""

from mqtt_device_sim import provision_main


if __name__ == "__main__":
    raise SystemExit(provision_main())

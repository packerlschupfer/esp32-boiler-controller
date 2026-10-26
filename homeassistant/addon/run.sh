#!/usr/bin/with-contenv bashio

# Read config from Home Assistant
export MQTT_HOST=$(bashio::config 'mqtt_host')
export MQTT_PORT=$(bashio::config 'mqtt_port')
export MQTT_USER=$(bashio::config 'mqtt_user')
export MQTT_PASSWORD=$(bashio::config 'mqtt_password')
export MQTT_TOPIC_PREFIX=$(bashio::config 'mqtt_topic_prefix')

bashio::log.info "Starting Boiler Config UI..."
bashio::log.info "MQTT: ${MQTT_USER}@${MQTT_HOST}:${MQTT_PORT}"

cd /app
exec python3 server.py

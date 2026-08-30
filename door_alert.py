#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Forward Wishome door-lock events (MQTT wishome/lock/events) to the ESP32
smartdoor endpoint so it can arm/disarm the thief alarm and record events in
the smartdoor/ Firebase root.

Door WA notifications are still handled by wishome_bridge.py itself; this
service ONLY toggles the alarm state, so there is no notification overlap.
"""
import json
import time
import urllib.request

import paho.mqtt.client as mqtt

MQTT_HOST = '127.0.0.1'
MQTT_PORT = 1883
MQTT_TOPIC = 'wishome/lock/events'
ESP32_DOOR_URL = 'http://192.168.18.61/doorlock'
LOG_FILE = '/var/log/door_alert.log'


def log(msg):
    line = '[%s] %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg)
    try:
        with open(LOG_FILE, 'a') as f:
            f.write(line + '\n')
    except Exception:
        pass
    print(line, flush=True)


def forward(payload):
    try:
        body = json.dumps(payload, separators=(',', ':')).encode()
        req = urllib.request.Request(ESP32_DOOR_URL, data=body, method='POST')
        req.add_header('Content-Type', 'application/json')
        with urllib.request.urlopen(req, timeout=12) as r:
            code = r.status
        log('forward event=%s logId=%s -> HTTP %s' %
            (payload.get('event'), payload.get('logId'), code))
        return code == 200
    except Exception as e:
        log('forward FAILED event=%s: %r' % (payload.get('event'), e))
        return False


def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode('utf-8'))
    except Exception as e:
        log('bad payload %r: %r' % (msg.payload[:200], e))
        return
    forward(payload)


def main():
    log('door_alert starting, MQTT=%s:%s topic=%s -> %s' %
        (MQTT_HOST, MQTT_PORT, MQTT_TOPIC, ESP32_DOOR_URL))
    client = mqtt.Client(client_id='door_alert')
    client.on_message = on_message
    client.reconnect_delay_set(1, 30)
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.subscribe(MQTT_TOPIC, qos=1)
    log('subscribed %s' % MQTT_TOPIC)
    client.loop_forever()


if __name__ == '__main__':
    main()

/*
 * vim:ts=4:sw=4:expandtab
 */
#ifndef PUBSUB_MQTT_H
#define PUBSUB_MQTT_H

#define PROTOCOL_NAME "MQIsdp"

int handle_connect(struct Client *client, int msg_length);
#endif

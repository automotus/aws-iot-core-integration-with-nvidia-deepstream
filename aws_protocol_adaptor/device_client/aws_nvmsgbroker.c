/*
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

#include <glib.h>
#include <gst/gst.h>
#include <gmodule.h>
#include <sys/syscall.h>
#include "aws_config_parser.h"
#include "nvds_msgapi.h"
#include "aws_nvmsgbroker.h"
#include <openssl/sha.h>

#define SHA256_FILE_BUFLEN 32768
#define SHA256_STRLEN SHA256_DIGEST_LENGTH * 2 + 1

NvDsMsgApiHandle (*nvds_msgapi_connect_ptr)(char *connection_str, nvds_msgapi_connect_cb_t connect_cb, char *config_path);
NvDsMsgApiErrorType (*nvds_msgapi_send_ptr)(NvDsMsgApiHandle conn, char *topic, const uint8_t *payload, size_t nbuf);
NvDsMsgApiErrorType (*nvds_msgapi_disconnect_ptr)(NvDsMsgApiHandle h_ptr);
NvDsMsgApiErrorType (*nvds_msgapi_connection_signature_ptr)(char *connection_str, char *config_path, char *output_str, int max_len);
static GMutex thread_mutex;
static GQueue *work_queue;
static struct timespec last_send_time_stamp; // this is to make sure we send or yield frequent enough so we do not get disconnected.
static nvds_msgapi_connect_cb_t disconnect_cb; // disconnect handler provided by connect thread
static nvds_msgapi_subscribe_request_cb_t nvds_cb; // msgapi subscribe callback handler
static char *subscribed_topics[MAX_SUBSCRIPTIONS]; // to store the subscribed topics in order to be used during unsubscribe operation
static size_t num_subscriptions = 0; // number of registered subscriptions

/* ************************************************************************* */
// Connect function def
/* ************************************************************************* */

static void
disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data)
{
	IOT_WARN("MQTT Disconnect");
	IoT_Error_t rc = FAILURE;
	if (NULL == pClient)
	{
		return;
	}
	IOT_UNUSED(data);
	if (aws_iot_is_autoreconnect_enabled(pClient))
	{
		IOT_INFO("Auto Reconnect is enabled, Reconnecting attempt will start now");
	}
	else
	{
		IOT_WARN("Auto Reconnect not enabled. Starting manual reconnect...");
		rc = aws_iot_mqtt_attempt_reconnect(pClient);
		if (NETWORK_RECONNECTED == rc)
		{
			IOT_WARN("Manual Reconnect Successful");
		}
		else
		{
			IOT_WARN("Manual Reconnect Failed - %d", rc);
			if (disconnect_cb != NULL)
			{
				disconnect_cb((NvDsMsgApiHandle)pClient, NVDS_MSGAPI_EVT_DISCONNECT);
			}
		}
	}
}

NvDsMsgApiHandle nvds_msgapi_connect(char *connection_str, nvds_msgapi_connect_cb_t connect_cb, char *config_path)
{
	disconnect_cb = connect_cb;
	if (config_path == NULL)
	{
		IOT_ERROR("Essential args missing for function nvds_msgapi_connect\n");
		return NULL;
	}

	// param init
	g_mutex_init(&thread_mutex);
	work_queue = g_queue_new();
	IoT_Error_t rc = FAILURE;
	AWS_IoT_Client *client = g_malloc(sizeof(AWS_IoT_Client));
	IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
	IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

	// try to connect to iot server
	IOT_INFO("\nAWS IoT SDK Version %d.%d.%d-%s\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);
	parse_config_file(&mqttInitParams, &connectParams, (gchar *)config_path);
	mqttInitParams.enableAutoReconnect = false; // We enable this later below
	mqttInitParams.disconnectHandler = disconnectCallbackHandler;
	mqttInitParams.disconnectHandlerData = NULL;
	connectParams.keepAliveIntervalInSec = 600;
	connectParams.isCleanSession = true;
	connectParams.MQTTVersion = MQTT_3_1_1;
	connectParams.clientIDLen = (uint16_t)strlen(connectParams.pClientID);
	connectParams.isWillMsgPresent = false;

	IOT_DEBUG("rootCA %s", mqttInitParams.pRootCALocation);
	IOT_DEBUG("clientCRT %s", mqttInitParams.pDeviceCertLocation);
	IOT_DEBUG("clientKey %s", mqttInitParams.pDevicePrivateKeyLocation);

	rc = aws_iot_mqtt_init(client, &mqttInitParams);
	if (SUCCESS != rc)
	{
		IOT_ERROR("aws_iot_mqtt_init returned error : %d ", rc);
		return NULL;
	}

	IOT_INFO("Connecting...");
	rc = aws_iot_mqtt_connect(client, &connectParams);
	if (SUCCESS != rc)
	{
		IOT_ERROR("Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
		if (disconnect_cb != NULL)
			connect_cb((NvDsMsgApiHandle)client, NVDS_MSGAPI_EVT_DISCONNECT);
		return NULL;
	}
	disconnect_cb = connect_cb;
	clock_gettime(CLOCK_REALTIME, &last_send_time_stamp);
	// auto-reconnect? how?
	IOT_INFO("Successfully connected");
	return (NvDsMsgApiHandle)client;
}

/* ************************************************************************* */
// Disconnect function def
/* ************************************************************************* */

NvDsMsgApiErrorType nvds_msgapi_disconnect(NvDsMsgApiHandle h_ptr)
{
	if ((h_ptr == NULL))
	{
		IOT_ERROR("Essential args missing for function nvds_msgapi_disconnect\n");
		return NVDS_MSGAPI_ERR;
	}
	IoT_Error_t rc = FAILURE;
	AWS_IoT_Client *client = (AWS_IoT_Client *)h_ptr;
	for (int i = 0; i < num_subscriptions; i++)
	{
		rc = aws_iot_mqtt_unsubscribe(client, subscribed_topics[i], strlen(subscribed_topics[i]));
		if (SUCCESS != rc)
		{
			IOT_ERROR("Unable to unsubscribe, error: %d\n", rc);
		}
	}
	IOT_INFO("Successfully unsubscribed");

	rc = aws_iot_mqtt_disconnect(client);
	if (SUCCESS != rc)
	{
		IOT_ERROR("Unable to disconnect, error: %d\n", rc);
		return NVDS_MSGAPI_ERR;
	}
	IOT_INFO("Successfully disconnected");
	g_free(client);
	return NVDS_MSGAPI_OK;
}

/* ************************************************************************* */
// Send function def
/* ************************************************************************* */

static IoT_Error_t _mqtt_msg_send(AWS_IoT_Client *client, char *topic, const uint8_t *payload, size_t nbuf)
{
	IoT_Error_t rc = FAILURE;
	IoT_Publish_Message_Params paramsQOS0;
	paramsQOS0.qos = QOS0;
	paramsQOS0.payload = (void *)payload;
	paramsQOS0.isRetained = 0;
	paramsQOS0.payloadLen = nbuf;
	g_mutex_lock(&thread_mutex);
	rc = aws_iot_mqtt_publish(client, topic, strlen(topic), &paramsQOS0);
	g_mutex_unlock(&thread_mutex);
	return rc;
}

NvDsMsgApiErrorType nvds_msgapi_send(NvDsMsgApiHandle conn, char *topic, const uint8_t *payload, size_t nbuf)
{
	if ((conn == NULL) || (topic == NULL) || (payload == NULL) || (nbuf == 0))
	{
		IOT_ERROR("Essential args missing for function nvds_msgapi_send\n");
		return NVDS_MSGAPI_ERR;
	}
	AWS_IoT_Client *client = (AWS_IoT_Client *)conn;
	IoT_Error_t rc = _mqtt_msg_send(client, topic, payload, nbuf);
	if (SUCCESS != rc)
	{
		IOT_ERROR("Unable to publish, error: %d\n", rc);
		return NVDS_MSGAPI_ERR;
	}
	IOT_INFO("Successfully sent sync message");
	return NVDS_MSGAPI_OK;
}

/* ************************************************************************* */
// Async: Send function def
// Therefore, we are using a mutex to guard aws_iot_mqtt_publish in nvds_msgapi_send_async.
// Once this beta version below becomes official, we will release the one without mutex.
/* ************************************************************************* */

typedef struct Works
{
	NvDsMsgApiHandle h_ptr;
	size_t payload_size;
	nvds_msgapi_send_cb_t call_back_handler;
	void *user_ptr;
	char topic[AWS_IOT_MQTT_TOPIC_BUF_LEN];
	char payload[AWS_IOT_MQTT_TX_BUF_LEN];
} Work;

// flag connection status
// add comment to keep connection always on otherwise: NVDS_MSGAPI_ERR
// not demonstrating subscriptions
NvDsMsgApiErrorType nvds_msgapi_send_async(NvDsMsgApiHandle h_ptr, char *topic, const uint8_t *payload, size_t nbuf, nvds_msgapi_send_cb_t send_callback, void *user_ptr)
{
	if ((h_ptr == NULL) || (topic == NULL) || (payload == NULL) || (nbuf == 0))
	{
		IOT_ERROR("Essential args missing for function nvds_msgapi_send: %d, %d, %d, %d\n", (h_ptr == NULL), (topic == NULL), (payload == NULL), (nbuf == 0));
		return NVDS_MSGAPI_ERR;
	}
	Work *work_node = g_malloc(sizeof(Work));
	if (work_node == NULL)
	{
		IOT_ERROR("Malloc failed.");
		return NVDS_MSGAPI_ERR;
	}
	work_node->h_ptr = h_ptr;
	work_node->payload_size = nbuf;
	work_node->call_back_handler = send_callback;
	work_node->user_ptr = user_ptr;
	if ((strlen(topic) > (AWS_IOT_MQTT_TOPIC_BUF_LEN - 1)) || (nbuf > AWS_IOT_MQTT_TX_BUF_LEN))
	{
		IOT_ERROR("Topic or payload buff size too small.");
		return NVDS_MSGAPI_ERR;
	}
	memset(work_node->topic, 0, sizeof(work_node->topic));
	memset(work_node->payload, 0, sizeof(work_node->payload));
	memcpy(work_node->topic, topic, strlen(topic));
	memcpy(work_node->payload, payload, nbuf);
	g_queue_push_tail(work_queue, work_node);
	return NVDS_MSGAPI_OK;
}

void nvds_cb_wrapped(AWS_IoT_Client *pClient, char *pTopicName, uint16_t topicNameLen, IoT_Publish_Message_Params *pParams, void *pClientData)
{
	IOT_INFO("Subscribe callback");
	nvds_cb(NVDS_MSGAPI_OK, pParams->payload, pParams->payloadLen, pTopicName, pClientData);
}

NvDsMsgApiErrorType nvds_msgapi_subscribe(NvDsMsgApiHandle h_ptr, char **topics, int num_topics, nvds_msgapi_subscribe_request_cb_t cb, void *user_ctx)
{
	IOT_INFO("Subscribe called\n");
	if ((h_ptr == NULL) || (topics == NULL) || (num_topics <= 0))
	{
		IOT_ERROR("Essential args missing for function nvds_msgapi_subscribe: %d, %d, %d\n", (h_ptr == NULL), (topics == NULL), (num_topics == 0));
		return NVDS_MSGAPI_ERR;
	}
	if (!cb)
	{
		IOT_ERROR("Callback function for nvds_msgapi_subscribe cannot be NULL\n");
		return NVDS_MSGAPI_ERR;
	}
	nvds_cb = cb;
	IoT_Error_t rc = FAILURE;
	AWS_IoT_Client *client = (AWS_IoT_Client *)h_ptr;
	for (int i = 0; i < num_topics; i++)
	{
		rc = aws_iot_mqtt_subscribe(client, topics[i], strlen(topics[i]), QOS0, nvds_cb_wrapped, user_ctx);
		if (SUCCESS != rc)
		{
			IOT_ERROR("Unable to subscribe, error: %d\n", rc);
			return NVDS_MSGAPI_ERR;
		}
		subscribed_topics[i] = topics[i];
		num_subscriptions++;
	}
	IOT_INFO("Successfully subscribed");
	return NVDS_MSGAPI_OK;
}

/* ************************************************************************* */
// Do Work function def
/* ************************************************************************* */

void nvds_msgapi_do_work(NvDsMsgApiHandle h_ptr)
{
	// decide whether to yield in this run
	IoT_Error_t rc = FAILURE;
	bool need_to_yield = false;
	struct timespec current_time_stamp;
	clock_gettime(CLOCK_REALTIME, &current_time_stamp);
	uint time_diff = current_time_stamp.tv_sec - last_send_time_stamp.tv_sec;
	if (time_diff > AWS_IOT_MAX_SEND_INTERVAL_SEC)
	{
		need_to_yield = true;
	}
	IOT_DEBUG("Current queue length: %d\n", g_queue_get_length(work_queue));
	if (g_queue_is_empty(work_queue))
	{
		IOT_INFO("Work queue empty.");
		if (need_to_yield)
		{
			IOT_INFO("IoT yielding in order to not be disconnected.");
			g_mutex_lock(&thread_mutex);
			rc = aws_iot_mqtt_yield((AWS_IoT_Client *)h_ptr, AWS_IOT_CLIENT_YIELD_WAIT_TIME);
			if (rc != SUCCESS && disconnect_cb != NULL)
			{
				disconnect_cb(h_ptr, NVDS_MSGAPI_EVT_DISCONNECT);
			}
			last_send_time_stamp = current_time_stamp;
			g_mutex_unlock(&thread_mutex);
		}
		return;
	}
	while (!g_queue_is_empty(work_queue))
	{
		Work *work_node = (Work *)g_queue_pop_head(work_queue);
		AWS_IoT_Client *client = (AWS_IoT_Client *)work_node->h_ptr;
		rc = _mqtt_msg_send(client, work_node->topic, work_node->payload, work_node->payload_size);
		if (SUCCESS != rc)
		{
			IOT_ERROR("Unable to publish, error: %d\n", rc);
			if (work_node->call_back_handler != NULL)
			{
				work_node->call_back_handler(work_node->user_ptr, NVDS_MSGAPI_ERR);
			}
			g_free(work_node);
			return;
		}
		if (work_node->call_back_handler != NULL)
		{
			IOT_INFO("Pointer callback.");
			work_node->call_back_handler(work_node->user_ptr, NVDS_MSGAPI_OK);
		}
		g_free(work_node);
	}
	last_send_time_stamp = current_time_stamp;
	return;
}

char *nvds_msgapi_getversion()
{
	return NVDS_MSGAPI_VERSION;
}

char *nvds_msgapi_get_protocol_name()
{
	return NVDS_MSGAPI_PROTOCOL;
}

bool is_valid_connection_str(char *conn_str)
{
	char *burl = "", *bport = "";
	if (connection_str == NULL)
	{
		IOT_ERROR("connection string cant be NULL");
		return false;
	}

	int i = 0;

	char *token = strtok(conn_str, ";");
	char *data[2];

	while (token)
	{
		data[i++] = token;
		token = strtok(NULL, ";");
	}
	for (i = 0; i < 2; i++)
	{
		printf("%s\n", data[i]);
	}
	burl = data[0];
	bport = data[1];

	if (burl == "" || bport == "")
	{
		IOT_ERROR("connection string is invalid. hostname or port is empty\n");
		return false;
	}
	return true;
}

int cfg_file_sha256(SHA256_CTX *sha256, char *cfg_path) 
{
	FILE *file;
	unsigned char buffer[SHA256_FILE_BUFLEN];
	int bytes_read = 0;
	int read_err = 0;

	if (!(file = fopen(cfg_path, "rb"))) 
	{
		IOT_ERROR("Failed to open configuration file %s\n", cfg_path);
		return -1;
	}

	while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)))
	{
		SHA256_Update(sha256, buffer, bytes_read);
	}

	read_err = ferror(file);
	fclose(file);

	if (read_err != 0) 
	{
		IOT_ERROR("Reading configuration file failed with error code %d\n", read_err);
		return read_err;
	}

	return 0;
}

int generate_sha256_hash(char *output_str, char *conn_str, char *cfg_path)
{
	SHA256_CTX sha256;
	unsigned char hashval[SHA256_DIGEST_LENGTH];

	SHA256_Init(&sha256);

	SHA256_Update(&sha256, conn_str, strlen(conn_str));

	if (cfg_file_sha256(&sha256, cfg_path) != 0)
	{
		IOT_ERROR("Failed to generate config file hash\n");
		return -1;
	}

	SHA256_Final(hashval, &sha256);
	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
	{
		sprintf(output_str + (i * 2), "%02x", hashval[i]);
	}

	return 0;
}

NvDsMsgApiErrorType nvds_msgapi_connection_signature(char *broker_str, char *cfg, char *output_str, int max_len)
{
	// Value of output_str must be empty string if operation is unsuccessful
	strcpy(output_str, ""); 

	if (broker_str == NULL || cfg == NULL)
	{
		IOT_ERROR("Must specify broker_str and cfg\n");
		return NVDS_MSGAPI_ERR;
	}

	if (max_len < SHA256_STRLEN) 
	{
		IOT_ERROR("Insufficient output string length. Need %d, got %d", SHA256_STRLEN, max_len);
		return NVDS_MSGAPI_ERR;
	}

	if (!is_valid_connection_str(broker_str))
	{
		return NVDS_MSGAPI_ERR;
	}

	if (generate_sha256_hash(output_str, broker_str, cfg) != 0)
	{
		return NVDS_MSGAPI_ERR;
	}

	return NVDS_MSGAPI_OK;
}

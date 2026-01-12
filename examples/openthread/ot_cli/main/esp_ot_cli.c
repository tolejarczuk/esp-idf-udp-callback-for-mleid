/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * OpenThread Command Line Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_types.h"
#include "nvs_flash.h"
#include "openthread/cli.h"
#include "openthread/instance.h"
#include "openthread/logging.h"
#include "openthread/tasklet.h"
#include "openthread/thread.h"
#include "openthread/udp.h"
#include "openthread/ip6.h"

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
#include "ot_led_strip.h"
#endif

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
#include "esp_ot_cli_extension.h"
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION

#define TAG "ot_esp_cli"

#define UDP_PORT 12345

static otUdpSocket sUdpSocket;
static bool sUdpServerInitialized = false;

static void udp_receive_callback(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo)
{
    otInstance *instance = (otInstance *)aContext;
    char buffer[128];
    uint16_t length = otMessageGetLength(aMessage);
    uint16_t offset = otMessageGetOffset(aMessage);

    if (length - offset > sizeof(buffer) - 1)
    {
        length = sizeof(buffer) - 1 + offset;
    }

    otMessageRead(aMessage, offset, buffer, length - offset);
    buffer[length - offset] = '\0';

    ESP_LOGI(TAG, "Received UDP message: %s", buffer);

    // Handle GET_MLEID command
    if (strcmp(buffer, "GET_MLEID") == 0)
    {
        const otIp6Address *mlEid = otThreadGetMeshLocalEid(instance);
        char response[64];
        char ml_eid_str[40];
        otIp6AddressToString(mlEid, ml_eid_str, sizeof(ml_eid_str));
        sprintf(response, "MLEID:%s", ml_eid_str);

        ESP_LOGI(TAG, "Sending response: %s", response);

        // Send UDP response back to sender
        otMessage *responseMessage = otUdpNewMessage(instance, NULL);
        if (responseMessage != NULL)
        {
            otMessageAppend(responseMessage, response, strlen(response));

            otMessageInfo messageInfo = *aMessageInfo;
            memcpy(&messageInfo.mPeerAddr, &aMessageInfo->mPeerAddr, sizeof(otIp6Address));
            messageInfo.mPeerPort = aMessageInfo->mPeerPort;

            otError error = otUdpSend(instance, &sUdpSocket, responseMessage, &messageInfo);
            if (error != OT_ERROR_NONE)
            {
                ESP_LOGE(TAG, "Failed to send UDP response: %d", error);
                otMessageFree(responseMessage);
            }
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create UDP response message");
        }
    }
}

static void setup_udp_server(otInstance *instance)
{
    if (sUdpServerInitialized)
    {
        return;
    }

    otSockAddr sockaddr;
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = UDP_PORT;

    ESP_LOGI(TAG, "Setting up UDP server on port %d...", UDP_PORT);

    otError error = otUdpOpen(instance, &sUdpSocket, udp_receive_callback, instance);
    if (error != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "Failed to open UDP socket: %d", error);
        return;
    }

    error = otUdpBind(instance, &sUdpSocket, &sockaddr, OT_NETIF_THREAD_HOST);
    if (error != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "Failed to bind UDP socket on port %d: error=%d", UDP_PORT, error);
        otUdpClose(instance, &sUdpSocket);
        return;
    }

    sUdpServerInitialized = true;
    ESP_LOGI(TAG, "UDP server successfully listening on port %d", UDP_PORT);
}

static void ot_state_changed_callback(otChangedFlags aFlags, void *aContext)
{
    otInstance *instance = (otInstance *)aContext;

    ESP_LOGI(TAG, "State changed, flags: 0x%08lx", aFlags);

    if (aFlags & OT_CHANGED_THREAD_ROLE)
    {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        const char *roleStr = "Unknown";
        switch (role)
        {
        case OT_DEVICE_ROLE_DISABLED:
            roleStr = "Disabled";
            break;
        case OT_DEVICE_ROLE_DETACHED:
            roleStr = "Detached";
            break;
        case OT_DEVICE_ROLE_CHILD:
            roleStr = "Child";
            break;
        case OT_DEVICE_ROLE_ROUTER:
            roleStr = "Router";
            break;
        case OT_DEVICE_ROLE_LEADER:
            roleStr = "Leader";
            break;
        }
        ESP_LOGI(TAG, "Thread role changed to: %s (%d)", roleStr, role);

        // Set up UDP server once we have a valid role (child, router, or leader)
        if (role >= OT_DEVICE_ROLE_CHILD && !sUdpServerInitialized)
        {
            setup_udp_server(instance);
        }
    }
}

static esp_netif_t *init_openthread_netif(const esp_openthread_platform_config_t *config)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(config)));

    return netif;
}

static void ot_task_worker(void *aContext)
{
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    // Initialize the OpenThread stack
    ESP_ERROR_CHECK(esp_openthread_init(&config));

#if CONFIG_OPENTHREAD_STATE_INDICATOR_ENABLE
    ESP_ERROR_CHECK(esp_openthread_state_indicator_init(esp_openthread_get_instance()));
#endif

#if CONFIG_OPENTHREAD_LOG_LEVEL_DYNAMIC
    // The OpenThread log level directly matches ESP log level
    (void)otLoggingSetLevel(CONFIG_LOG_DEFAULT_LEVEL);
#endif
    // Initialize the OpenThread cli
#if CONFIG_OPENTHREAD_CLI
    esp_openthread_cli_init();
#endif

    esp_netif_t *openthread_netif;
    // Initialize the esp_netif bindings
    openthread_netif = init_openthread_netif(&config);
    esp_netif_set_default_netif(openthread_netif);

#if CONFIG_OPENTHREAD_CLI_ESP_EXTENSION
    esp_cli_custom_command_init();
#endif // CONFIG_OPENTHREAD_CLI_ESP_EXTENSION

    // Register state change callback for UDP server initialization
    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    otSetStateChangedCallback(instance, ot_state_changed_callback, instance);

    // Log current Thread state
    otDeviceRole role = otThreadGetDeviceRole(instance);
    bool isEnabled = otThreadGetDeviceRole(instance) != OT_DEVICE_ROLE_DISABLED;
    ESP_LOGI(TAG, "Initial Thread state - Role: %d, Enabled: %d", role, isEnabled);

    // Check if we have a stored dataset
    otOperationalDatasetTlvs activeDataset;
    otError error = otDatasetGetActiveTlvs(instance, &activeDataset);
    if (error == OT_ERROR_NONE)
    {
        ESP_LOGI(TAG, "Active dataset found, enabling Thread...");

        // Enable IPv6 and Thread
        error = otIp6SetEnabled(instance, true);
        ESP_LOGI(TAG, "otIp6SetEnabled result: %d", error);

        error = otThreadSetEnabled(instance, true);
        ESP_LOGI(TAG, "otThreadSetEnabled result: %d", error);
    }
    else
    {
        ESP_LOGW(TAG, "No active dataset found (error: %d). Use CLI to configure network.", error);
    }

    esp_openthread_lock_release();

    // Run the main loop
#if CONFIG_OPENTHREAD_CLI
    esp_openthread_cli_create_task();
#endif
#if CONFIG_OPENTHREAD_AUTO_START
    otOperationalDatasetTlvs dataset;
    error = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);
    ESP_ERROR_CHECK(esp_openthread_auto_start((error == OT_ERROR_NONE) ? &dataset : NULL));
#endif
    esp_openthread_launch_mainloop();

    // Clean up
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);

    esp_vfs_eventfd_unregister();
    vTaskDelete(NULL);
}

void app_main(void)
{
    // Used eventfds:
    // * netif
    // * ot task queue
    // * radio driver
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    xTaskCreate(ot_task_worker, "ot_cli_main", 10240, xTaskGetCurrentTaskHandle(), 5, NULL);
}

/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_tls.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
// ADC
#include "driver/adc.h"
#include "esp_adc_cal.h"

#define DEFAULT_VREF 1100 // Use adc2_vref_to_gpio() to get the proper number
#define NO_OF_SAMPLES 64  // Multisampling

#include "esp_http_client.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048
static const char *TAG = "HTTP_CLIENT";

#define WEBSERVER_URL "https://ihdlofkic7ueawamy5py7capk40nrkwk.lambda-url.us-east-1.on.aws/"
#define SERVER_POLLING_RATE 30000 // Rate in ms to send data to the server

// ADC Channels
#if CONFIG_IDF_TARGET_ESP32
#define ADC1_EXAMPLE_CHAN0 ADC1_CHANNEL_6
static const char *TAG_CH[10] = {"ADC1_CH6"};
#else
#define ADC1_EXAMPLE_CHAN0 ADC1_CHANNEL_2
static const char *TAG_CH[10] = {"ADC1_CH2"};
#endif

// ADC Attenuation
#define ADC_EXAMPLE_ATTEN ADC_ATTEN_DB_11

#define OFFSET 1000 // Reading error from soil moisture monitor
#define VAL_TO_PERCENT(x) ((1 - ((x - OFFSET) / (4096.0 - OFFSET))) * 100)

static int adc_raw[10];
static float moisture_send;

static void get_data(void *pvParameters)
{
    for (;;)
    {
        adc_raw[0] = adc1_get_raw(ADC1_EXAMPLE_CHAN0);
        ESP_LOGI(TAG_CH[0], "soil moisture: %f", VAL_TO_PERCENT(adc_raw[0]));
        moisture_send = VAL_TO_PERCENT(adc_raw[0]);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* Root cert for howsmyssl.com, taken from howsmyssl_com_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const char howsmyssl_com_root_cert_pem_start[] asm("_binary_howsmyssl_com_root_cert_pem_start");
extern const char howsmyssl_com_root_cert_pem_end[] asm("_binary_howsmyssl_com_root_cert_pem_end");

extern const char postman_root_cert_pem_start[] asm("_binary_postman_root_cert_pem_start");
extern const char postman_root_cert_pem_end[] asm("_binary_postman_root_cert_pem_end");

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer; // Buffer to store response of http request from event handler
    static int output_len;      // Stores number of bytes read
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        /*
         *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
         *  However, event handler can also be used in case chunked encoding is used.
         */
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            // If user_data buffer is configured, copy the response into the buffer
            if (evt->user_data)
            {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            }
            else
            {
                if (output_buffer == NULL)
                {
                    output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
            // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
            // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        if (output_buffer != NULL)
        {
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    }
    return ESP_OK;
}
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
static void https_with_url(void)
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    esp_http_client_config_t config = {
        .url = WEBSERVER_URL,
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer, // Pass address of local buffer to get response
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    char post_data[100];
    uint32_t time_send = esp_log_timestamp();
    uint8_t mac_addr[8];
    esp_efuse_mac_get_default(mac_addr);
    char profile_send[18];
    snprintf(profile_send, sizeof(profile_send), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    snprintf(post_data, sizeof(post_data), "{\"timestamp\":\"%i\",\"moisture\":\"%f\",\"profile\":\"%s\"}", time_send, moisture_send, profile_send);
    // snprintf(post_data, sizeof(post_data), "{timestamp:%i,moisture:%f,profile:%s}", time_send, moisture_send, profile_send);
    printf("%s\n",post_data);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    // ESP_LOG_BUFFER_CHAR(TAG, local_response_buffer, strlen(local_response_buffer));
    printf("%s\n",local_response_buffer);
    esp_http_client_cleanup(client);
}
#endif // CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
uint8_t tickIterations = 5;
static void http_test_task(void *pvParameters)
{
    for (;;)
    {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        https_with_url();
#endif

        // const TickType_t xDelay = 5000 / portTICK_PERIOD_MS; // block for 5 seconds

        vTaskDelay(SERVER_POLLING_RATE / portTICK_PERIOD_MS); // block for 30 seconds
        // tickIterations--;
        // if (tickIterations == 0) {
        // ESP_LOGI(TAG, "Finish http example");
        // vTaskDelete(NULL);
        // }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Connected to AP, begin sending data");

    ESP_ERROR_CHECK(adc1_config_width(ADC_WIDTH_BIT_DEFAULT));
    ESP_ERROR_CHECK(
        adc1_config_channel_atten(ADC1_EXAMPLE_CHAN0, ADC_EXAMPLE_ATTEN));
    xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);
    xTaskCreate(&get_data, "Getting data from ADC", 8192, NULL, 5, NULL);
}

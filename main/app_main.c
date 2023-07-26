#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
// #include "protocol_examples_common.h"
#include "mqtt_client.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include <time.h> //random numbers
#include "esp_timer.h"

#define led1 15
#define led2 2
#define led3 4

#define UART_NUM UART_NUM_0
#define BUF_SIZE 1024 
#define TASK_MEMORY 1024 * 5
#define PATTERN_CHR_NUM (1) /*!< Set the number of consecutive and identical characters received by receiver which defines a UART pattern*/

/* MQTT */
#define CONFIG_BROKER_URL "mqtt://mqtt.eclipseprojects.io"

/* WiFi */
#define EXAMPLE_ESP_WIFI_SSID "Samsung1"
#define EXAMPLE_ESP_WIFI_PASS "12345678"
#define EXAMPLE_ESP_MAXIMUM_RETRY 5
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static QueueHandle_t uart_queue;
static QueueHandle_t mqtt_queue;
QueueHandle_t ecg007_queue;
static esp_timer_handle_t mqtt_timer;
static esp_timer_handle_t uart_timer;
static int64_t mqtt_send_time;
static int64_t uart_send_time;
int64_t mqtt_receive_time = 0;
int64_t uart_receive_time = 0;
int64_t uart_fill_time = 0;
int64_t elapsed_time = 0;
int64_t uart_elapsed_time = 0;
esp_err_t config_leds(void);
// void ecg007SplitData(uint8_t startByte, uint8_t dataByte, char *output, uint8_t outputSize, uint8_t *counter);
void sendEcg007Data(uint8_t *data, uint32_t data_size);
void sendPM6750Data(uint8_t *data, uint32_t data_size);
void hexStringToByteArray(const char *hexString, unsigned char *byteArray);
/* TASKS */
static void uart_task(void *pvParameters);

static esp_mqtt_client_handle_t client;

/**
 * @brief Event structure used in UART event queue
 */
typedef struct
{
    uint8_t *data; /*!< UART event type */
    int size;      /*!< UART data size for UART_DATA event*/
} mqtt_data_t;

// char* example_arr;
char *data_parsed;
uint8_t counter_example = 0;

bool mqtt_sended = true;
// int counter = 0;

// char data_to_send[1000]; // data que se acumulara para enviar data por MQTT

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0)
    {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/* struct ECG007
{
    uint8_t waves_header;
    uint8_t rtop_header;
    uint8_t breath_header;
    double status_header;
    uint64_t waves; // 7 bytes of each channel
    uint8_t breath;
    uint64_t stats; // 7 bytes [ppm, breathe rate,sensor state, temp1 int,temp1 dec, temp2 int, temp2 dec ]
    char string_member[25];
};

struct ECG007 ecg007={
    0xFB,
    0xFC,
    0xFD,
    0xFE,
    0x00000000,
    0x00,
    0x00000000,
}; */
// static const char *TAG = "UART";
void hexStringToByteArray(const char *hexString, unsigned char *byteArray)
{
    size_t length = strlen(hexString);
    size_t i;

    for (i = 0; i < length / 2; i++)
    {
        char hex[3] = {hexString[i * 2], hexString[i * 2 + 1], '\0'};
        byteArray[i] = (unsigned char)strtol(hex, NULL, 16);
    }
}

// Tarea del consumidor
static void mqtt_send_task(void *pvParameters)
{
    mqtt_data_t data_event;
    int free_space1 = 0;
    int free_space2 = 0;
    bool flag = false;
    while (1)
    {
        // Leer el valor de la cola
        if (xQueueReceive(mqtt_queue, &data_event, portMAX_DELAY))
        {
            free_space1 = uxQueueSpacesAvailable(uart_queue);
            free_space2 = uxQueueSpacesAvailable(mqtt_queue);
            // ESP_LOGI(TAG, "Espacio disponible en queue: %d", free_space2);
            // printf("Consumidor: Valor recibido de la cola: %02x.\n", received_value);
            // ESP_LOGI(TAG, "tamano: %d", sizeof(data_event->size));
            //esp_timer_start_once(mqtt_timer, 0);
            mqtt_send_time = esp_timer_get_time();
            mqtt_sended = false;
            esp_mqtt_client_publish(client, "/sensor/pm6750", (const char *)(data_event.data), data_event.size, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
           /*  while (!mqtt_sended)
            {
                vTaskDelay(pdMS_TO_TICKS(10));
            } */
            free(data_event.data);
            // Detener el temporizador y obtener el tiempo actual
          //  esp_timer_stop(mqtt_timer);
            mqtt_receive_time = esp_timer_get_time();
            // Calcular el tiempo transcurrido entre el envío y la recepción
            elapsed_time = mqtt_receive_time - mqtt_send_time;
            ESP_LOGI(TAG, "Tiempo transcurrido: %lld ms %d %d", elapsed_time / 1000, free_space1, free_space2);
        };
        vTaskDelay(pdMS_TO_TICKS(10)); // Esperar 100 milisegundos antes de verificar nuevamente
    }
}

static void uart_task(void *pvParameters)
{
    uart_event_t event;
    size_t buffered_size;
    int readed_size = 0;
    mqtt_data_t data_event;
    uint8_t *data = (uint8_t *)malloc(1024);
    uint8_t *temp_data = (uint8_t *)malloc(1024);
    int total_size = 0; // Tamaño acumulado de los datos recibidos
    bool temp_flag=false;
    while (1)
    {
        /* if (true)
        { */
        if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY))
        {
            bzero(data, 1024);
            switch (event.type)
            { // IMPORTANTE!!!!!
            // Event of UART receving data
            /*We'd better handler data event fast, there would be much more data events than
            other types of events. If we take too much time on data event, the queue might
            be full.*/
            case UART_DATA: // cuando se recibe data en el puerto serial
                readed_size = uart_read_bytes(UART_NUM, data, event.size, portMAX_DELAY);
                if (total_size + readed_size <= 1000) // si el buffer temp no se ha llenado
                {
                    if(!temp_flag){
                      uart_send_time = esp_timer_get_time(); 
                      temp_flag=true; 
                    }
                    memcpy(temp_data + total_size, data, readed_size);
                    total_size += readed_size;
                }
                else
                { // buffer lleno

                    //  if (readed_size > 0)
                    //{
                    //  xQueueSend(mqtt_queue, &data, portMAX_DELAY);
                    // esp_mqtt_client_publish(client, "/sensor/pm6750", (const char *)data, readed_size, 0, 0);
                    
                    
                    uart_receive_time = esp_timer_get_time();
                    uart_elapsed_time = uart_receive_time - uart_send_time;

                    ESP_LOGI(TAG, "Buffer de envío completo. Enviando a cola... Tiempo de llenado: %lld total size: %d", uart_elapsed_time / 1000, total_size);
                    data_event.data = (uint8_t *)malloc(total_size);
                    memcpy(data_event.data, temp_data, total_size);
                    data_event.size = total_size;
                    xQueueSend(mqtt_queue, &data_event, portMAX_DELAY);
                    total_size = 0;

                    //    sendEcg007Data(data,event.size);
                    // sendPM6750Data(data, event.size);
                    // uart_write_bytes(UART_NUM, (const char *)data, event.size);
                    // ESP_LOGI(TAG, "Recv str: %08x \n", (char *)data);
                    //   uart_flush(UART_NUM); // limpieza del buffer de entrada para evitar overflow
                    //}
                    temp_flag=false;
                }

                break;
            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "hw fifo overflow"); ////////////////////////////////
                // If fifo overflow happened, you should consider adding flow control for your application.
                // The ISR has already reset the rx FIFO,
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(UART_NUM);
                //  xQueueReset(uart_queue);
                break;
            // Event of UART ring buffer full
            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "ring buffer full"); ////////////////////!!!!!!!!!!!!!!!!!!!!!!
                // If buffer full happened, you should consider increasing your buffer size
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(UART_NUM);
                // xQueueReset(uart_queue);
                break;
            // Event of UART RX break detected
            case UART_BREAK:
                ESP_LOGI(TAG, "uart rx break");
                break;
            // Event of UART parity check error
            case UART_PARITY_ERR:
                ESP_LOGI(TAG, "uart parity error");
                break;
            // Event of UART frame error
            case UART_FRAME_ERR:
                ESP_LOGI(TAG, "uart frame error");
                break;
            // UART_PATTERN_DET
            case UART_PATTERN_DET: // patrones para comandos https://github.com/espressif/esp-idf/blob/d00e7b5af897cc5fafe51fae19c57f0313b81edf/examples/peripherals/uart/uart_events/main/uart_events_example_main.c
                                   /*   uart_get_buffered_data_len(UART_NUM, &buffered_size);
                                     int pos = uart_pattern_pop_pos(UART_NUM);
                                     ESP_LOGI(TAG, "[UART PATTERN DETECTED] pos: %d, buffered size: %d", pos, buffered_size);
                                     if (pos == -1)
                                     {
                                         // There used to be a UART_PATTERN_DET event, but the pattern position queue is full so that it can not
                                         // record the position. We should set a larger queue size.
                                         // As an example, we directly flush the rx buffer here.
                                         uart_flush_input(UART_NUM);
                                     }
                                     else
                                     {
                                         uart_read_bytes(UART_NUM, data, pos, 100 / portTICK_PERIOD_MS);
                                         uint8_t pat[PATTERN_CHR_NUM + 1];
                                         memset(pat, 0, sizeof(pat));
                                         uart_read_bytes(UART_NUM, pat, PATTERN_CHR_NUM, 100 / portTICK_PERIOD_MS);
                                         ESP_LOGI(TAG, "read data: %02x", data);
                                         ESP_LOGI(TAG, "read pat : %02x", pat);
                                         // esp_mqtt_client_publish(client, "/sensor/pm6750", (const char *)data, 0, 0, 0);
                                     } */
                break;

            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }
        else
        {
            ESP_LOGE(TAG, "Error de recepcion de dato");
        }
    }
    free(data);
    data = NULL;
    vTaskDelete(NULL);
}

static void echo_task(void *arg)
{

    /*  uart_config_t uart_config = {
     .baud_rate = 115200, // 38400 ecg007  115200 pm6750
     .data_bits = UART_DATA_8_BITS,
     .parity = UART_PARITY_DISABLE,
     .stop_bits = UART_STOP_BITS_1,
     .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
     .source_clk = UART_SCLK_DEFAULT};

 ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
 ESP_ERROR_CHECK(uart_set_pin(UART_NUM, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
 ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0)); */
    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *)malloc(BUF_SIZE);

    while (1)
    {
        //  uart_read_bytes(UART_NUM, data, event.size, portMAX_DELAY);
        // Read data from the UART
        int len = uart_read_bytes(UART_NUM, data, 100, 20 / portTICK_PERIOD_MS);
        // Write data back to the UART
        //  uart_write_bytes(ECHO_UART_PORT_NUM, (const char *)data, len);
        if (len)
        {
            //  data[len] = '\0';
            ESP_LOGI(TAG, "len: %i", len);
            // sendPM6750Data(data, len);
            esp_mqtt_client_publish(client, "/sensor/pm6750", (const char *)data, 0, 0, 0);
        }
    }
}

static void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200, // 38400 ecg007  115200 pm6750
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT};

    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, 1024, 1024, 100, &uart_queue, 0));
    // Set uart pattern detect function.
    // uart_enable_pattern_det_baud_intr(UART_NUM, 0x55, 1, 9, 0, 0);
    // Reset the pattern queue length to record at most 20 pattern positions.
    // uart_pattern_queue_reset(UART_NUM, 20);
    xTaskCreate(uart_task, "uart_task", TASK_MEMORY, NULL, 5, NULL);
    // ESP_LOGI(TAG, "Init uart completed");
    //  xTaskCreate(echo_task, "uart_echo_task", TASK_MEMORY, NULL, 10, NULL);
}

esp_err_t config_leds(void)
{
    gpio_reset_pin(led1);
    gpio_reset_pin(led2);
    gpio_reset_pin(led3);
    gpio_set_direction(led1, GPIO_MODE_OUTPUT);
    gpio_set_direction(led2, GPIO_MODE_OUTPUT);
    gpio_set_direction(led3, GPIO_MODE_OUTPUT);
    return ESP_OK;
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    // esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        /*   msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
          ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id); */

        msg_id = esp_mqtt_client_subscribe(client, "/esp32", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        /*         msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
                ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

                msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
                ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id); */
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        // ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        mqtt_sended = true;
        // Calcular el tiempo transcurrido entre el envío y la recepción
        break;
    case MQTT_EVENT_DATA:
        //  ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        unsigned char *byteArray = malloc(event->data_len);
        hexStringToByteArray(event->data, byteArray);
        uart_write_bytes(UART_NUM, byteArray, (event->data_len) / 2);
        /*        for (size_t i = 0; i < (event->data_len) / 2; i++)
               {
                   printf("%02X ", byteArray[i]);
               }
               printf("\n");
        */
        free(byteArray);

        /* printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data); */

        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_BROKER_URL,
        .buffer.size = BUF_SIZE * 5};
    client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /*  ESP_ERROR_CHECK(esp_netif_init());

     ESP_ERROR_CHECK(esp_event_loop_create_default()); */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void app_main(void)
{
    // data_parsed = (char *)malloc(BUF_SIZE);
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    config_leds();
    init_uart();
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    // configure wifi
    wifi_init_sta();
    // init mqtt
    mqtt_app_start();

    mqtt_queue = xQueueCreate(1000, 100);
    xTaskCreatePinnedToCore(mqtt_send_task, "mqtt_send_task", BUF_SIZE * 4, NULL, 15, NULL, 1);
    // Iniciar el temporizador para medir el tiempo de envío
    esp_timer_create_args_t mqtt_timer_args = {
        .callback = NULL,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mqtt_timer"};
    esp_timer_create(&mqtt_timer_args, &mqtt_timer);

    esp_timer_create_args_t uart_timer_args = {
        .callback = NULL,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "uart_timer"};
    esp_timer_create(&uart_timer_args, &uart_timer);

    srand(time(NULL));

    // char data[32];
    /*   while (1)
      {
          sprintf(data, "%d", rand());
          esp_mqtt_client_publish(client, "/topic/qos0", data, strlen(data), 0, 0);
          vTaskDelay(pdMS_TO_TICKS(1000));
      }*/
}

void sendEcg007Data(uint8_t *data, uint32_t data_size)
{
    for (size_t i = 0; i < data_size; i++)
    {
        sprintf(&data_parsed[i * 2], "%02x", data[i]);
    }
    esp_mqtt_client_publish(client, "/sensor/ecg007", data_parsed, data_size, 0, 0);
}

void sendPM6750Data(uint8_t *data, uint32_t data_size)
{
    /*    for (size_t i = 0; i < data_size; i++)
       {
           sprintf(&data_parsed[i * 2], "%02x", data[i]);
       } */

    /*  ESP_LOGI(TAG, "Recv str: %s \n", (char *)data_parsed);
     esp_mqtt_client_publish(client, "/sensor/pm6750", data_parsed, 0, 0, 0); */
    ESP_LOGI(TAG, "Recv str: %s \n", (char *)data);
    esp_mqtt_client_publish(client, "/sensor/pm6750", &data, 0, 0, 0);
    // esp_mqtt_client_enqueue(client, "/sensor/pm6750", data_parsed, 0, 0, 0, true);
    vTaskDelay(pdMS_TO_TICKS(1));
}

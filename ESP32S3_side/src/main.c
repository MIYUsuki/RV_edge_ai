#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "cJSON.h"


static const char *TAG = "RADAR_MQTT";
static const char *TAG_GPIO = "REQ_GPIO";
static uint32_t last_mqtt_time = 0;
static uint32_t last_tts_time = 0;

//改为自己的WiFi名和密码
#define WIFI_SSID      "K60"
#define WIFI_PASSWORD  "159139family"
#define MAXIMUM_RETRY  5

// 定义 FreeRTOS 事件标志位
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

// UART 引脚与配置 (根据S3实际接线修改)
#define RADAR_UART_NUM     UART_NUM_1
#define RADAR_TXD_PIN      17
#define RADAR_RXD_PIN      18
#define RADAR_RX_BUF_SIZE  256
// --- TTS 模块配置 (SNR9816TTS) ---
// 波特率默认 115200
#define TTS_UART_NUM       UART_NUM_2
#define TTS_TXD_PIN        43   // 接TTS模块的RX
#define TTS_RXD_PIN        44   // 接TTS模块的TX

// --- 配置 SPI 引脚 ---
// 以下引脚仅为示例，请根据你的 S3 硬件连接进行修改
#define GPIO_MOSI 11
#define GPIO_MISO 13
#define GPIO_SCLK 12
#define GPIO_CS   10
// 使用 SPI2 主机外设作为从机
#define RCV_HOST    SPI2_HOST

//检测luckfoxpico的输出高低电平
#define SENSOR_INPUT_PIN  GPIO_NUM_4

// 定义接收/发送缓冲区大小 (例如 1024 字节)
#define BUF_SIZE 1024

// 本地 MQTT 服务器配置
//需根据实际情况修改
#define LOCAL_MQTT_BROKER  "ws://broker.emqx.io:8083/mqtt" 
#define MQTT_TOPIC         "sensor/radar"

// 全局 MQTT 客户端句柄
esp_mqtt_client_handle_t mqtt_client = NULL;

// Wi-Fi 事件处理回调函数
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Wi-Fi 启动后，尝试连接
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 连接断开后尝试重连
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            // 达到最大重试次数，设置失败标志位
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // 成功获取 IP 地址
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        // 设置成功标志位
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    // 1. 初始化底层 TCP/IP 堆栈
    ESP_ERROR_CHECK(esp_netif_init());

    // 2. 创建默认的事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. 初始化 Wi-Fi，使用默认配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. 注册事件处理函数
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // 5. 配置 Wi-Fi 参数 (STA 模式, SSID, 密码)
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            // 默认情况下如果密码不匹配，连接可能会挂起，可以设置 WPA2
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    
    // 6. 启动 Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // 等待连接成功或失败的标志位
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

/*
    MQTT related definition
*/

// MQTT 事件回调函数
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected to Local Broker!");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected.");
            break;
        default:
            break;
    }
}

void parse_mqtt_message(const char *topic, int topic_len, const char *data, int data_len)
{
    // 1. 安全处理：将非 \0 结尾的内存段拷贝为标准的 C 字符串
    // strndup 会自动分配内存并在末尾补 '\0'，使用完必须 free
    char *topic_str = strndup(topic, topic_len);
    char *data_str = strndup(data, data_len);

    if (topic_str == NULL || data_str == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed for topic or data");
        if(topic_str) free(topic_str);
        if(data_str) free(data_str);
        return;
    }

    ESP_LOGI(TAG, "Received message on Topic: %s", topic_str);

    // 2. 使用 cJSON 解析 Payload
    // 假设发来的数据格式为: {"action" :full_power}
    cJSON *root = cJSON_Parse(data_str);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON payload. Raw data: %s", data_str);
    } else {
        // 提取字段
        cJSON *action = cJSON_GetObjectItem(root, "action");
        cJSON *move = cJSON_GetObjectItem(root, "status");


        // 安全校验并使用数据
        if (cJSON_IsString(action) && (action->valuestring != NULL)) {
            ESP_LOGI(TAG, "Parsed Device: %s", action->valuestring);
        }
        
        if (cJSON_IsString(move) && (move->valuestring != NULL)) {
            ESP_LOGI(TAG, "Parsed Status: %s", move->valuestring);
            /*
            if(move->valuerstring==full_power)
                exit ulp;
            else if(move->valuestring==low_cost)
                enter ulp
            */
        }

        // 3. 释放 cJSON 对象，防止内存泄漏 (非常重要)
        cJSON_Delete(root);
    }

    // 4. 释放前面分配的字符串内存
    free(topic_str);
    free(data_str);
}
// 启动 MQTT 客户端
static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = LOCAL_MQTT_BROKER,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

/*
    hardware initialize part
*/

// 初始化雷达 UART
static void radar_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 256000,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(RADAR_UART_NUM, RADAR_RX_BUF_SIZE, 0, 0, NULL, 0);
    uart_param_config(RADAR_UART_NUM, &uart_config);
    uart_set_pin(RADAR_UART_NUM, RADAR_TXD_PIN, RADAR_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

// 雷达数据解析与 MQTT 发送任务
static void radar_parse_task(void *arg) {
    uint8_t data[RADAR_RX_BUF_SIZE];
    uint8_t frame_buf[32];
    int frame_index = 0;
    
    char mqtt_payload[64];

    while (1) {
        // 从 UART 读取数据
        int len = uart_read_bytes(RADAR_UART_NUM, data, RADAR_RX_BUF_SIZE, 20 / portTICK_PERIOD_MS);
        
        for (int i = 0; i < len; i++) {
            // 状态机：寻找帧头 F4 F3 F2 F1
            if (frame_index == 0 && data[i] != 0xF4) continue;
            if (frame_index == 1 && data[i] != 0xF3) { frame_index = 0; continue; }
            if (frame_index == 2 && data[i] != 0xF2) { frame_index = 0; continue; }
            if (frame_index == 3 && data[i] != 0xF1) { frame_index = 0; continue; }
            
            frame_buf[frame_index++] = data[i];
            
            // 正常模式下一帧固定为 23 字节
            if (frame_index == 23) {
                // 校验帧尾 F8 F7 F6 F5
                if (frame_buf[19] == 0xF8 && frame_buf[20] == 0xF7 && 
                    frame_buf[21] == 0xF6 && frame_buf[22] == 0xF5) {
                    
                    // 提取目标状态 (Byte 8)
                    uint8_t target_state = frame_buf[8];
                    
                    // 提取运动距离 (Byte 9, 10 小端序)
                    uint16_t moving_dist = frame_buf[9] | (frame_buf[10] << 8);
                    
                    // 提取静止距离 (Byte 12, 13 小端序)
                    uint16_t static_dist = frame_buf[12] | (frame_buf[13] << 8);

                    // 极致精简：仅当有目标时才上报，或者状态发生变化时上报
                    // 此处演示全量精简上报，格式：{"s":状态, "m":运动距离cm, "st":静止距离cm}
                    snprintf(mqtt_payload, sizeof(mqtt_payload), 
                             "{\"s\":%d,\"m\":%d,\"st\":%d}", 
                             target_state, moving_dist, static_dist);
                             
                    // 通过 MQTT 发布，QoS=0 追求极致低延迟
                    if (mqtt_client != NULL) {
                        esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, mqtt_payload, 0, 0, 0);
                    }
                    
                    // ESP_LOGI(TAG, "Sent: %s", mqtt_payload);
                }
                
                // 帧处理完毕，重置索引迎接下一帧
                frame_index = 0;
            }
        }
    }
}

// --- SNR9816TTS UART 初始化 ---
static void tts_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,                // 模块默认波特率 [cite: 175]
        .data_bits = UART_DATA_8_BITS,      
        .parity    = UART_PARITY_DISABLE,   
        .stop_bits = UART_STOP_BITS_1,      
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(TTS_UART_NUM, 256, 0, 0, NULL, 0);
    uart_param_config(TTS_UART_NUM, &uart_config);
    uart_set_pin(TTS_UART_NUM, TTS_TXD_PIN, TTS_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}
// --- SNR9816TTS 语音播报核心函数 ---
void tts_play_text(const char *text) {
    if (text == NULL) return;
    
    size_t text_len = strlen(text);
    if (text_len > 4000) text_len = 4000; // 单次合成不超过4K字节 [cite: 177]

    // 数据长度 = 命令字(1) + 编码参数(1) + 文本长度 [cite: 178]
    uint16_t data_len = 2 + text_len; 
    
    // 总帧长 = 帧头(1) + 长度(2) + 数据长度 [cite: 178]
    uint8_t *frame = malloc(3 + data_len);
    if (frame == NULL) return;

    frame[0] = 0xFD;                     // 帧头 [cite: 178]
    frame[1] = (data_len >> 8) & 0xFF;   // 数据长度高8位 [cite: 178]
    frame[2] = data_len & 0xFF;          // 数据长度低8位 [cite: 178]
    frame[3] = 0x01;                     // 命令字：合成播放 [cite: 178]
    frame[4] = 0x04;                     // 编码参数：0x04代表UTF-8 [cite: 178]
    
    memcpy(&frame[5], text, text_len);   // 填入UTF-8文本

    uart_write_bytes(TTS_UART_NUM, frame, 3 + data_len);
    ESP_LOGI(TAG, "Sent %d bytes to TTS module.", text_len);
    
    free(frame);
}
/**
 * @brief GPIO 初始化配置
 */
static void gpio_input_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,         // 轮询模式，不使用硬件中断
        .mode = GPIO_MODE_INPUT,                // 设置为输入模式
        .pin_bit_mask = (1ULL << SENSOR_INPUT_PIN), // 选择引脚
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   // 启用下拉电阻 (默认保持低电平，防悬空误触)
        .pull_up_en = GPIO_PULLUP_DISABLE,      // 禁用上拉
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG_GPIO, "GPIO %d initialized as input.", SENSOR_INPUT_PIN);
}

/**
 * @brief 引脚电平轮询与 MQTT 发送任务
 */
static void gpio_detect_task(void *arg) {
    // 记录上一次的电平状态，初始假定为低电平 (0)
    int last_state = 0; 
    
    while (1) {
        // 读取当前引脚电平 (1 为高电平，0 为低电平)
        int current_state = gpio_get_level(SENSOR_INPUT_PIN);
        //软件限制 您没事吧 的播放频率
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;

        // 核心逻辑：只有当上一次是 0，这一次是 1 时，才认为是"触发了高电平"
        if (current_state == 1 && last_state == 0) {
            ESP_LOGI(TAG_GPIO, "检测到高电平! 正在发送 MQTT 消息...");
            
            // 确保 MQTT 客户端不为空且已连接再发送
            if (mqtt_client != NULL) {
                const char *topic = "sensor/alert";
                const char *payload = "{\"status\": \"fall_detected\"}";
                // 【限制 语音播报】：拉低时播报，5秒最多1次，意味着间隔至少 5000 毫秒
                if (current_time - last_tts_time >= 5000) {
                    tts_play_text("您没事吧");
                    last_tts_time = current_time; // 更新上一次播报的时间
                }
                
                // 发送 QoS=1 的消息保证到达
                // 推荐修改：接收返回值
                int msg_id = esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 0);
                if (msg_id != -1) {
                    ESP_LOGI(TAG_GPIO, "消息发送成功, msg_id=%d", msg_id);
                } else {
                    ESP_LOGE(TAG_GPIO, "消息发送失败!");
                }
            } else {
                ESP_LOGW(TAG_GPIO, "MQTT 客户端未就绪，放弃发送。");
            }
        }

        // 更新上一次的状态
        last_state = current_state;

        // 延时 50ms (包含两个作用：1. 释放CPU给其他任务 2. 软件按键消抖)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/**
 * @brief 启动检测任务的入口函数
 */
void start_gpio_detect(void) {
    gpio_input_init();
    // 创建一个 FreeRTOS 任务专门用于检测
    // 栈空间给 2048 足够，优先级设为 3 (可根据实际需求调整)
    xTaskCreate(gpio_detect_task, "gpio_detect_task", 2048, NULL, 3, NULL);
}



void app_main(void) {
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    
    // 连接成功后可以继续其他的网络任务...
    
    ESP_LOGI(TAG, "Initializing Radar UART...");
    radar_uart_init();
    tts_uart_init();
    gpio_input_init();
    
    ESP_LOGI(TAG, "Starting MQTT Client...");
    mqtt_app_start();
    static uint32_t last_mqtt_time = 0;
    static uint32_t last_tts_time = 0;
    
    ESP_LOGI(TAG, "Starting Radar Parsing Task...");
    // 分配 4KB 栈空间，优先级设为较高 (5)
    xTaskCreate(radar_parse_task, "radar_parse_task", 4096, NULL, 5, NULL);
    
    start_gpio_detect();
}
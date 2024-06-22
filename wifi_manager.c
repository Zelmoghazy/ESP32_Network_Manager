#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <esp_http_server.h>
#include <sys/param.h>

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include <mqtt_client.h>

#define S_IP        "192.168.1.1"     
#define GATEWAY     "192.168.1.1"    
#define NETMASK     "255.255.255.0"

#define WIFI_TAG        "WIFI"
#define TAG             "WEBSERVER"

httpd_handle_t  webserver;

static RTC_DATA_ATTR char GOT_IP = false;
static RTC_DATA_ATTR char __SSID[32];
static RTC_DATA_ATTR char __PWD[64];
static RTC_DATA_ATTR char __MQTT[64];
static RTC_DATA_ATTR char __WIFI;
static RTC_DATA_ATTR char __ETHERNET;

bool SOFTAP = 0;

#define ESP_WIFI_SSID      "ESP_WIFI"
#define ESP_WIFI_PASS      "123456789"
#define ESP_WIFI_CHANNEL   1
#define MAX_STA_CONN       4

static int s_retry_num = 0;
#define MAXIMUM_RETRY  20

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static httpd_handle_t start_webserver(void);

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void wifi_sta_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) 
    {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(WIFI_TAG,"connect to the AP fail");
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) 
    {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
        httpd_stop(webserver);
    }
}

void wifi_init_sta()
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_sta_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_sta_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, __SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, __PWD, sizeof(wifi_config.sta.password));

    /* Disable Power Saving */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     ** number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) 
     */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, 
     ** hence we can test which event actually happened. 
     */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s password:%s",
                 wifi_config.sta.ssid, wifi_config.sta.password);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(WIFI_TAG, "Failed to connect to SSID:%s, password:%s",
                 wifi_config.sta.ssid, wifi_config.sta.password);
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }
}

void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    assert(netif);

    if (esp_netif_dhcps_stop(netif) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop dhcp client");
        return;
    }
    esp_netif_ip_info_t info_t;
    memset(&info_t, 0, sizeof(esp_netif_ip_info_t));
    ipaddr_aton((const char *)S_IP, &info_t.ip.addr);
    ipaddr_aton((const char *)GATEWAY, &info_t.gw.addr);
    ipaddr_aton((const char *)NETMASK, &info_t.netmask.addr);
    if(esp_netif_set_ip_info(netif, &info_t) != ESP_OK){
        ESP_LOGE(TAG, "Failed to set ip info");
    }
    esp_netif_dhcps_start(netif);


    ESP_ERROR_CHECK(esp_netif_attach_wifi_ap(netif));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_ap_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                    .required = true,
            },
        },
    };

    if (strlen(ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             ESP_WIFI_SSID, ESP_WIFI_PASS, ESP_WIFI_CHANNEL);

    httpd_stop(webserver);

    webserver = start_webserver();
}

bool get_wifi_credentials(void) 
{
    size_t length = 0;

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    err = nvs_get_str(nvs_handle, "ssid", NULL, &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;

    err = nvs_get_str(nvs_handle, "ssid", __SSID, &length);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Credentials do not exist
        ESP_LOGI(TAG, "Wi-Fi credentials doesnt exist.");
        nvs_close(nvs_handle);
        return false;

    }

    length = 0;
    err = nvs_get_str(nvs_handle, "password", NULL, &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;
    // password maybe nothing if open, add check maybe later
    err = nvs_get_str(nvs_handle, "password", __PWD, &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to get password! (%s)",esp_err_to_name(err));
        return false;
    }

    nvs_close(nvs_handle);
    return true;
}

bool get_mqtt_credentials(void) 
{
    size_t length = 0;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("mqtt", NVS_READONLY, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return false;
    }

    err = nvs_get_str(nvs_handle, "uri", NULL, &length);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return false;

    err = nvs_get_str(nvs_handle, "uri", __MQTT,&length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Credentials do not exist
        ESP_LOGI(TAG, "MQTT credentials doesnt exist.");
        nvs_close(nvs_handle);
        return false;

    }
    nvs_close(nvs_handle);
    return true;
}

void store_wifi_credentials(void) 
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }
    
    err = nvs_set_str(nvs_handle, "ssid", __SSID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SSID!");
    }

    err = nvs_set_str(nvs_handle, "password", __PWD);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set password!");
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit updates!");
    }

    ESP_LOGI(TAG, "Stored Wi-Fi credentials: SSID: %s, Password: %s", __SSID, __PWD);

    nvs_close(nvs_handle);
}

void store_mqtt_credentials() 
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("mqtt", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs_handle, "uri", __MQTT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set uri!");
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit updates!");
    }

    ESP_LOGI(TAG, "Stored mqtt credentials: uri: %s", __MQTT);

    nvs_close(nvs_handle);
}


void network_manager(void)
{
    if(__ETHERNET){
        // Initialize ethernet
    }else{
        if(get_wifi_credentials()) // no saved credentials
        {
            wifi_init_sta();

        }else{
            SOFTAP |= 1;
        }
    }
}

static esp_err_t servePage_get_handler(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, 
    "<!DOCTYPE html>\
    <html>\
    <head>\
        <title>ESP32 Network Configuration</title>\
        <style>\
            body {\
                display: flex;\
                justify-content: center;\
                align-items: center;\
                height: 100vh;\
                background-color: #f0f0f0;\
                font-family: Arial, sans-serif;\
            }\
            #network-form-container {\
                background-color: #fff;\
                padding: 20px;\
                border-radius: 10px;\
                box-shadow: 0 0 10px rgba(0, 0, 0, 0.1);\
                width: 300px;\
                text-align: center;\
            }\
            h1 {\
                font-size: 24px;\
                margin-bottom: 20px;\
            }\
            label {\
                display: block;\
                margin-top: 10px;\
                font-weight: bold;\
            }\
            input[type=\"text\"],\
            input[type=\"password\"] {\
                width: calc(100\% - 20px);\
                padding: 10px;\
                margin-top: 5px;\
                border: 1px solid #ccc;\
                border-radius: 5px;\
            }\
            input[type=\"checkbox\"] {\
                margin-top: 10px;\
            }\
            button {\
                margin-top: 20px;\
                padding: 10px 20px;\
                background-color: #007BFF;\
                color: white;\
                border: none;\
                border-radius: 5px;\
                cursor: pointer;\
            }\
            button:hover {\
                background-color: #0056b3;\
            }\
        </style>\
        <script>\
            function toggleCheckbox(checkbox) {\
                var ethernetCheckbox = document.getElementById(\"ethernet-checkbox\");\
                var wifiCheckbox = document.getElementById(\"wifi-checkbox\");\
                if (checkbox.id === \"ethernet-checkbox\" && checkbox.checked) {\
                    wifiCheckbox.checked = false;\
                } else if (checkbox.id === \"wifi-checkbox\" && checkbox.checked) {\
                    ethernetCheckbox.checked = false;\
                } else if (!ethernetCheckbox.checked && !wifiCheckbox.checked) {\
                    wifiCheckbox.checked = true;\
                }\
                var username = document.getElementById(\"username\");\
                var password = document.getElementById(\"password\");\
                if (checkbox.id === \"ethernet-checkbox\") {\
                    username.disabled = checkbox.checked;\
                    password.disabled = checkbox.checked;\
                    username.value = \"\";\
                    password.value = \"\";\
                }else{\
                    username.disabled = false;\
                    password.disabled = false;\
                }\
            }\
            function sendFormData() {\
                var username = document.getElementById(\"username\").value;\
                var password = document.getElementById(\"password\").value;\
                var mqttHost = document.getElementById(\"mqtt-host\").value;\
                var ethernet = document.getElementById(\"ethernet-checkbox\").checked;\
                var wifi = document.getElementById(\"wifi-checkbox\").checked;\
                var xhr = new XMLHttpRequest();\
                xhr.open(\"POST\", \"/network-config\", true);\
                xhr.setRequestHeader(\"Content-Type\", \"application/json\");\
                xhr.send(JSON.stringify({\
                    \"username\": username,\
                    \"password\": password,\
                    \"mqttHost\": mqttHost,\
                    \"ethernet\": ethernet,\
                    \"wifi\": wifi\
                }));\
            }\
        </script>\
    </head>\
    <body>\
        <div id=\"network-form-container\">\
            <h1>ESP32 Network Configuration</h1>\
            <form id=\"network-form\">\
                <label for=\"username\">Username:</label>\
                <input type=\"text\" id=\"username\" name=\"username\"><br>\
                <label for=\"password\">Password:</label>\
                <input type=\"password\" id=\"password\" name=\"password\"><br>\
                <label for=\"mqtt-host\">MQTT Host:</label>\
                <input type=\"text\" id=\"mqtt-host\" name=\"mqtt-host\"><br>\
                <label for=\"ethernet-checkbox\">Ethernet</label>\
                <input type=\"checkbox\" id=\"ethernet-checkbox\" name=\"ethernet-checkbox\" onclick=\"toggleCheckbox(this)\"><br>\
                <label for=\"wifi-checkbox\">Wi-Fi</label>\
                <input type=\"checkbox\" id=\"wifi-checkbox\" name=\"wifi-checkbox\" onclick=\"toggleCheckbox(this)\" checked><br>\
                <button type=\"button\" onclick=\"sendFormData()\">Submit</button>\
            </form>\
        </div>\
    </body>\
    </html>\
");

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t servePage = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = servePage_get_handler,
    .user_ctx = NULL
};

static esp_err_t network_config_post_handler(httpd_req_t *req)
{
    char buf[300];
    int ret, remaining = req->content_len;

    while (remaining > 0)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (ret == 0)
            {
                ESP_LOGI(TAG, "No content received please try again ...");
            }
            else if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {

                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }
        remaining -= ret;
    }

    cJSON *json = cJSON_Parse(buf);

    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return ESP_FAIL;
    }

    cJSON *username = cJSON_GetObjectItem(json, "username");
    cJSON *password = cJSON_GetObjectItem(json, "password");
    cJSON *mqttHost = cJSON_GetObjectItem(json, "mqttHost");
    cJSON *ethernet = cJSON_GetObjectItem(json, "ethernet");
    cJSON *wifi     = cJSON_GetObjectItem(json, "wifi");

    if (cJSON_IsString(username) && cJSON_IsString(password) && cJSON_IsString(mqttHost) && cJSON_IsBool(ethernet) && cJSON_IsBool(wifi)) {
        ESP_LOGI(TAG, "Username: %s", username->valuestring);
        ESP_LOGI(TAG, "Password: %s", password->valuestring);
        ESP_LOGI(TAG, "MQTT Host: %s", mqttHost->valuestring);
        ESP_LOGI(TAG, "Ethernet: %s", ethernet->valueint ? "true" : "false");
        ESP_LOGI(TAG, "Wi-Fi: %s", wifi->valueint ? "true" : "false");

        sprintf(__SSID, "%s",   username->valuestring);
        sprintf(__PWD, "%s",    password->valuestring);
        sprintf(__MQTT, "%s",   mqttHost->valuestring);
        __WIFI     =            wifi->valueint;
        __ETHERNET =            ethernet->valueint;

        store_wifi_credentials();
        store_mqtt_credentials();
    }

    cJSON_Delete(json);
    
    httpd_resp_send_chunk(req, NULL, 0);
    esp_sleep_enable_timer_wakeup(100000);
    esp_deep_sleep_start();
    return ESP_OK;
}

static const httpd_uri_t network_config_uri = {
    .uri = "/network-config",
    .method = HTTP_POST,
    .handler = network_config_post_handler,
    .user_ctx = "TEST"
};


static esp_err_t index_get_handler(httpd_req_t *req) {
    extern const unsigned char index_html_start[] asm("_binary_index_html_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_end");
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

static httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_get_handler,
    .user_ctx = NULL
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &servePage);
        httpd_register_uri_handler(server, &network_config_uri);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}


static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) 
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, "timestamp", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            esp_mqtt_client_publish(client,"response",event->data,event->data_len,0,0);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
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
    if(!get_mqtt_credentials()){
        SOFTAP |= 1;
        return;
    }
    esp_mqtt_client_config_t mqtt_cfg = {};

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_set_uri(client, __MQTT);
    
    if(client == NULL){
        ESP_LOGE(TAG, "MQTT Client creation failed.\n");
    }else{
        ESP_LOGI(TAG, "MQTT Client successfuly created.\n");
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    /* Starts MQTT client with already created client handle. */
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    network_manager();
    mqtt_app_start();
    if(SOFTAP){
        wifi_init_softap();
    }
}

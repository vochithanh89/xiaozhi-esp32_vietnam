#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "zhengchen_lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "power_manager.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <wifi_station.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "mcp_server.h"
#include <qrcode.h>
#include "system_info.h"

#define TAG "ZHENGCHEN_1_54TFT_WIFI"

class ZHENGCHEN_1_54TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    ZHENGCHEN_LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_9);
        power_manager_->OnTemperatureChanged([this](float chip_temp) {
            display_->UpdateHighTempWarning(chip_temp);
        });

        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                ESP_LOGI("PowerManager", "Charging started");
            } else {
                power_save_timer_->SetEnabled(true);
                ESP_LOGI("PowerManager", "Charging stopped");
            }
        });
    
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_2);
        rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_2, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        // 设置开机按钮的长按事件（直接进入配网模式）
        boot_button_.OnLongPress([this]() {
            // 唤醒电源保存定时器
            power_save_timer_->WakeUp();
            // 获取应用程序实例
            auto& app = Application::GetInstance();
            
            // 进入配网模式
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            
            // 重置WiFi配置以确保进入配网模式
            ResetWifiConfiguration();
        });

        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_up_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_down_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));

        display_ = new ZHENGCHEN_LcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->SetupHighTempWarningPopup();
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.network.ip2qrcode",
            "Print the QR code of the IP address connected to WiFi network.\n"
            "Use this tool when user asks about network connection, IP address and print QR code.\n"
            "Returns the new IP address, SSID, and connection status. Also displays IP address as QR code on LCD screen.",
            PropertyList(),
            [](const PropertyList &properties) -> ReturnValue
            {
                auto &wifi_station = WifiStation::GetInstance();
                auto &board = Board::GetInstance();

                ESP_LOGI(TAG, "Getting network status for IP address tool");
                cJSON *json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "connected", wifi_station.IsConnected());

                if (wifi_station.IsConnected())
                {
                    std::string ip_address = wifi_station.GetIpAddress();
                    cJSON_AddStringToObject(json, "ip_address", ip_address.c_str());
                    cJSON_AddStringToObject(json, "ssid", wifi_station.GetSsid().c_str());
                    cJSON_AddNumberToObject(json, "rssi", wifi_station.GetRssi());
                    cJSON_AddNumberToObject(json, "channel", wifi_station.GetChannel());
                    cJSON_AddStringToObject(json, "mac_address", SystemInfo::GetMacAddress().c_str());
                    cJSON_AddStringToObject(json, "status", "connected");

                    // Generate and display QR code for IP address
                    auto display = board.GetDisplay();
                    if (display)
                    {
                        ESP_LOGI(TAG, "Generating QR code for IP address: %s", ip_address.c_str());
                        if (display->QRCodeIsSupported())
                        {
                            ip_address += "";
                            display->SetIpAddress(ip_address);
                            // Capture display pointer for callback
                            static Display *s_display = display;
                            esp_qrcode_config_t qrcode_cfg = {
                                .display_func = [](esp_qrcode_handle_t qrcode)
                                {
                                    if (s_display && qrcode)
                                    {
                                        s_display->DisplayQRCode(qrcode, nullptr);
                                    }
                                },
                                .max_qrcode_version = 10,
                                .qrcode_ecc_level = ESP_QRCODE_ECC_MED};

                            // Create URL format for QR code
                            std::string qr_text = "http://" + ip_address;
                            esp_err_t err = esp_qrcode_generate(&qrcode_cfg, qr_text.c_str());
                            if (err == ESP_OK)
                            {
                                ESP_LOGI(TAG, "QR code generated and displayed for IP: %s", ip_address.c_str());
                                cJSON_AddBoolToObject(json, "qrcode_displayed", true);
                            }
                            else
                            {
                                ESP_LOGE(TAG, "Failed to generate QR code for IP address");
                                cJSON_AddBoolToObject(json, "qrcode_displayed", false);
                            }
                        }
                        else
                        {
                            display->SetChatMessage("assistant", ip_address.c_str());
                            vTaskDelay(pdMS_TO_TICKS(5000));
                            ESP_LOGW(TAG, "Display does not support QR code");
                            cJSON_AddBoolToObject(json, "qrcode_displayed", false);
                        }
                    }
                    else
                    {
                        cJSON_AddStringToObject(json, "status", "disconnected");
                        cJSON_AddStringToObject(json, "message", "Device is not connected to WiFi");
                    }
                }

                return json;
            });

        mcp_server.AddTool("external.worker.health",
            "Kiểm tra trạng thái Cloudflare MCP Worker.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto &board = Board::GetInstance();
                auto http = board.GetNetwork()->CreateHttp(5);
                http->SetHeader("Accept", "application/json");
                if (!http->Open("GET", "https://ai.micsoftvn.workers.dev/health")) {
                    throw std::runtime_error("Failed to open MCP worker health endpoint");
                }

                int status_code = http->GetStatusCode();
                std::string body = http->ReadAll();
                http->Close();

                if (status_code != 200) {
                    throw std::runtime_error("MCP worker health returned status " + std::to_string(status_code));
                }
                if (body.empty()) {
                    body = "{\"status\":\"unknown\"}";
                }
                return body;
            });

        mcp_server.AddTool("external.worker.news",
            "Lấy tin tức mới nhất thông qua Cloudflare MCP Worker.",
            PropertyList({ Property("limit", kPropertyTypeInteger, 5, 1, 10) }),
            [](const PropertyList& properties) -> ReturnValue {
                int limit = properties["limit"].value<int>();
                auto &board = Board::GetInstance();
                auto http = board.GetNetwork()->CreateHttp(5);
                http->SetHeader("Content-Type", "application/json");
                http->SetHeader("Accept", "application/json");
                if (!http->Open("POST", "https://ai.micsoftvn.workers.dev/mcp")) {
                    throw std::runtime_error("Failed to open MCP worker news endpoint");
                }

                cJSON* payload = cJSON_CreateObject();
                cJSON_AddStringToObject(payload, "tool", "vn_news");
                cJSON* input = cJSON_CreateObject();
                cJSON_AddNumberToObject(input, "limit", limit);
                cJSON_AddItemToObject(payload, "input", input);

                char* payload_str = cJSON_PrintUnformatted(payload);
                http->Write(payload_str, strlen(payload_str));
                http->Write("", 0);
                cJSON_free(payload_str);
                cJSON_Delete(payload);

                int status_code = http->GetStatusCode();
                std::string body = http->ReadAll();
                http->Close();

                if (status_code != 200) {
                    throw std::runtime_error("MCP worker news returned status " + std::to_string(status_code));
                }

                cJSON* response = cJSON_Parse(body.c_str());
                if (!response) {
                    return body;
                }
                std::string result;
                auto error = cJSON_GetObjectItem(response, "error");
                if (cJSON_IsString(error)) {
                    result = error->valuestring;
                } else {
                    auto result_item = cJSON_GetObjectItem(response, "result");
                    if (cJSON_IsString(result_item)) {
                        result = result_item->valuestring;
                    } else if (result_item != nullptr) {
                        char* json_str = cJSON_PrintUnformatted(result_item);
                        if (json_str) {
                            result.assign(json_str);
                            cJSON_free(json_str);
                        }
                    }
                }
                cJSON_Delete(response);

                if (result.empty()) {
                    return "{}";
                }
                return result;
            });

        mcp_server.AddTool("external.vnexpress.latest",
            "Fetch the latest headlines from VNExpress RSS feed.",
            PropertyList({ Property("limit", kPropertyTypeInteger, 5, 1, 20) }),
            [](const PropertyList& properties) -> ReturnValue {
                auto &board = Board::GetInstance();
                int limit = properties["limit"].value<int>();
                auto http = board.GetNetwork()->CreateHttp(5);
                const std::string url = "https://vnexpress.net/rss/tin-moi-nhat.rss";
                http->SetHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64)");
                http->SetHeader("Accept", "application/rss+xml, application/xml;q=0.9, */*;q=0.8");
                http->SetHeader("Accept-Encoding", "identity");
                http->SetHeader("Connection", "close");
                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string body = http->ReadAll();
                http->Close();

                auto strip_cdata = [](std::string text) {
                    const std::string begin = "<![CDATA[";
                    const std::string end = "]]>";
                    auto start = text.find(begin);
                    if (start != std::string::npos) {
                        text = text.substr(start + begin.size());
                        auto finish = text.find(end);
                        if (finish != std::string::npos) {
                            text = text.substr(0, finish);
                        }
                    }
                    return text;
                };

                auto trim = [](std::string text) {
                    auto not_space = [](char ch) { return !std::isspace(static_cast<unsigned char>(ch)); };
                    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
                    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
                    return text;
                };

                cJSON* array = cJSON_CreateArray();
                size_t pos = 0;
                while (cJSON_GetArraySize(array) < limit) {
                    size_t item_start = body.find("<item>", pos);
                    if (item_start == std::string::npos) {
                        break;
                    }
                    size_t item_end = body.find("</item>", item_start);
                    if (item_end == std::string::npos) {
                        break;
                    }
                    std::string item = body.substr(item_start, item_end - item_start);
                    pos = item_end + 7;

                    auto extract = [&](const char* tag) -> std::string {
                        std::string open = std::string("<") + tag + ">";
                        std::string close = std::string("</") + tag + ">";
                        size_t start = item.find(open);
                        if (start == std::string::npos) {
                            return "";
                        }
                        start += open.size();
                        size_t end = item.find(close, start);
                        if (end == std::string::npos) {
                            return "";
                        }
                        return item.substr(start, end - start);
                    };

                    std::string title = strip_cdata(trim(extract("title")));
                    std::string link = trim(extract("link"));
                    if (title.empty() || link.empty()) {
                        continue;
                    }
                    cJSON* news = cJSON_CreateObject();
                    cJSON_AddStringToObject(news, "title", title.c_str());
                    cJSON_AddStringToObject(news, "link", link.c_str());
                    cJSON_AddItemToArray(array, news);
                }

                char* json_str = cJSON_PrintUnformatted(array);
                std::string result(json_str ? json_str : "[]");
                if (json_str) {
                    cJSON_free(json_str);
                }
                cJSON_Delete(array);
                return result;
            });

        mcp_server.AddTool("external.duckduckgo.search",
            "Search DuckDuckGo for the given query and return quick results.",
            PropertyList({
                Property("query", kPropertyTypeString),
                Property("limit", kPropertyTypeInteger, 5, 1, 10)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url_encode = [](const std::string& value) -> std::string {
                    const char hex[] = "0123456789ABCDEF";
                    std::string encoded;
                    encoded.reserve(value.size() * 3);
                    for (unsigned char c : value) {
                        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                            encoded.push_back(static_cast<char>(c));
                        } else if (c == ' ') {
                            encoded.push_back('+');
                        } else {
                            encoded.push_back('%');
                            encoded.push_back(hex[(c >> 4) & 0x0F]);
                            encoded.push_back(hex[c & 0x0F]);
                        }
                    }
                    return encoded;
                };

                auto query = properties["query"].value<std::string>();
                int limit = properties["limit"].value<int>();

                auto &board = Board::GetInstance();
                auto http = board.GetNetwork()->CreateHttp(5);
                std::string url = "https://api.duckduckgo.com/?q=" + url_encode(query) + "&format=json&no_html=1&skip_disambig=1";
                http->SetHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64)");
                http->SetHeader("Accept", "application/json");
                http->SetHeader("Accept-Encoding", "identity");
                http->SetHeader("Connection", "close");
                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open DuckDuckGo API");
                }
                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("DuckDuckGo API returned status " + std::to_string(http->GetStatusCode()));
                }
                std::string body = http->ReadAll();
                http->Close();

                cJSON* root = cJSON_Parse(body.c_str());
                if (!root) {
                    throw std::runtime_error("Failed to parse DuckDuckGo response");
                }

                std::vector<std::pair<std::string, std::string>> results;

                auto abstract_text = cJSON_GetObjectItemCaseSensitive(root, "AbstractText");
                auto abstract_url = cJSON_GetObjectItemCaseSensitive(root, "AbstractURL");
                if (cJSON_IsString(abstract_text) && abstract_text->valuestring && *abstract_text->valuestring) {
                    std::string text = abstract_text->valuestring;
                    std::string url_value = (cJSON_IsString(abstract_url) && abstract_url->valuestring) ? abstract_url->valuestring : "";
                    results.emplace_back(std::move(text), std::move(url_value));
                }

                auto append_related = [&](cJSON* node) {
                    if (!cJSON_IsObject(node)) {
                        return;
                    }
                    auto text = cJSON_GetObjectItemCaseSensitive(node, "Text");
                    auto first_url = cJSON_GetObjectItemCaseSensitive(node, "FirstURL");
                    if (cJSON_IsString(text) && text->valuestring && cJSON_IsString(first_url) && first_url->valuestring) {
                        results.emplace_back(text->valuestring, first_url->valuestring);
                    }
                };

                auto related = cJSON_GetObjectItemCaseSensitive(root, "RelatedTopics");
                if (cJSON_IsArray(related)) {
                    cJSON* item = nullptr;
                    cJSON_ArrayForEach(item, related) {
                        if (cJSON_IsObject(item)) {
                            auto topics = cJSON_GetObjectItemCaseSensitive(item, "Topics");
                            if (cJSON_IsArray(topics)) {
                                cJSON* topic = nullptr;
                                cJSON_ArrayForEach(topic, topics) {
                                    if (results.size() >= static_cast<size_t>(limit)) {
                                        break;
                                    }
                                    append_related(topic);
                                    if (results.size() >= static_cast<size_t>(limit)) {
                                        break;
                                    }
                                }
                            } else {
                                append_related(item);
                            }
                        }
                        if (results.size() >= static_cast<size_t>(limit)) {
                            break;
                        }
                    }
                }

                cJSON* arr = cJSON_CreateArray();
                size_t count = std::min(results.size(), static_cast<size_t>(limit));
                for (size_t i = 0; i < count; ++i) {
                    cJSON* entry = cJSON_CreateObject();
                    cJSON_AddStringToObject(entry, "text", results[i].first.c_str());
                    cJSON_AddStringToObject(entry, "url", results[i].second.c_str());
                    cJSON_AddItemToArray(arr, entry);
                }

                char* json_str = cJSON_PrintUnformatted(arr);
                std::string result(json_str ? json_str : "[]");
                if (json_str) {
                    cJSON_free(json_str);
                }
                cJSON_Delete(arr);
                cJSON_Delete(root);
                return result;
            });

        mcp_server.AddTool("external.vietcombank.usd_rate",
            "Fetch the latest Vietcombank USD exchange rate (buy/transfer/sell).",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto &board = Board::GetInstance();
                auto http = board.GetNetwork()->CreateHttp(5);
                const std::string url = "https://portal.vietcombank.com.vn/Usercontrols/TVPortal.TyGia/pXML.aspx?b=10";
                http->SetHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64)");
                http->SetHeader("Referer", "https://portal.vietcombank.com.vn/");
                http->SetHeader("Accept", "application/xml, text/xml;q=0.9, */*;q=0.8");
                http->SetHeader("Accept-Encoding", "identity");
                http->SetHeader("Connection", "close");
                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open Vietcombank exchange rate API");
                }
                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Vietcombank API returned status " + std::to_string(http->GetStatusCode()));
                }
                std::string body = http->ReadAll();
                http->Close();

                auto find_between = [](const std::string& text, const std::string& begin, const std::string& end) -> std::string {
                    size_t start = text.find(begin);
                    if (start == std::string::npos) {
                        return "";
                    }
                    start += begin.size();
                    size_t finish = text.find(end, start);
                    if (finish == std::string::npos) {
                        return "";
                    }
                    return text.substr(start, finish - start);
                };

                std::string datetime = find_between(body, "<DateTime>", "</DateTime>");

                size_t usd_pos = body.find("CurrencyCode=\"USD\"");
                if (usd_pos == std::string::npos) {
                    throw std::runtime_error("USD rate not found in Vietcombank response");
                }
                size_t tag_start = body.rfind('<', usd_pos);
                size_t tag_end = body.find("/>", usd_pos);
                if (tag_start == std::string::npos || tag_end == std::string::npos) {
                    throw std::runtime_error("Unable to parse USD rate entry");
                }
                std::string tag = body.substr(tag_start, tag_end - tag_start);

                auto get_attr = [&](const std::string& name) -> std::string {
                    std::string key = name + "=\"";
                    size_t start = tag.find(key);
                    if (start == std::string::npos) {
                        return "";
                    }
                    start += key.size();
                    size_t finish = tag.find('"', start);
                    if (finish == std::string::npos) {
                        return "";
                    }
                    return tag.substr(start, finish - start);
                };

                std::string buy = get_attr("Buy");
                std::string transfer = get_attr("Transfer");
                std::string sell = get_attr("Sell");
                std::string name = get_attr("CurrencyName");

                if (buy.empty() && transfer.empty() && sell.empty()) {
                    throw std::runtime_error("Missing USD exchange values in response");
                }

                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "currency_code", "USD");
                if (!name.empty()) {
                    cJSON_AddStringToObject(root, "currency_name", name.c_str());
                }
                if (!datetime.empty()) {
                    cJSON_AddStringToObject(root, "timestamp", datetime.c_str());
                }
                if (!buy.empty()) {
                    cJSON_AddStringToObject(root, "buy", buy.c_str());
                }
                if (!transfer.empty()) {
                    cJSON_AddStringToObject(root, "transfer", transfer.c_str());
                }
                if (!sell.empty()) {
                    cJSON_AddStringToObject(root, "sell", sell.c_str());
                }

                char* json_str = cJSON_PrintUnformatted(root);
                std::string result(json_str ? json_str : "{}");
                if (json_str) {
                    cJSON_free(json_str);
                }
                cJSON_Delete(root);
                return result;
            });
    }

public:
    ZHENGCHEN_1_54TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();  
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    // 获取音频编解码器
    virtual AudioCodec* GetAudioCodec() override {
        // 静态实例化NoAudioCodecSimplex类
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        // 返回音频编解码器
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
        return true;
    }

    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual void SetPowerSaveMode(bool enabled) override {
        if (!enabled) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveMode(enabled);
    }
};

DECLARE_BOARD(ZHENGCHEN_1_54TFT_WIFI);

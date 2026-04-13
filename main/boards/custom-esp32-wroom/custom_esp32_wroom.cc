#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "led/neopixel_led.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_adc/adc_oneshot.h>
#include "assets/lang_config.h"

#define TAG "CustomESP32WROOM"

LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);

class CustomESP32WROOM : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    Button asr_button_;

    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    gpio_num_t fan_gpio_;
    
    // 火焰传感器相关
    bool flame_detected_ = false;
    TaskHandle_t flame_monitor_task_ = nullptr;
    
    // ADC 相关
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_channel_t flame_adc_channel_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 400 * 1000,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            {&font_puhui_14_1, &font_awesome_14_1});
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        asr_button_.OnClick([this]() {
            std::string wake_word="元巡";
            Application::GetInstance().WakeWordInvoke(wake_word);
        });

        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });
    }
    
    void InitializeFlameSensor() {
        // 初始化火焰传感器数字输入引脚
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << FLAME_SENSOR_D_GPIO);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io_conf);
        
        // 初始化 ADC 用于模拟输入（使用 ADC2，GPIO15 对应 ADC2_CHANNEL_3）
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_2,
        };
        esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize ADC2: %s", esp_err_to_name(err));
            return;
        }
        
        // 配置 ADC 通道（GPIO15 对应 ADC2_CHANNEL_3）
        flame_adc_channel_ = ADC_CHANNEL_3;
        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_oneshot_config_channel(adc_handle_, flame_adc_channel_, &chan_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "Flame sensor initialized successfully");
    }
    
    void InitializeWaterPump() {
        // 初始化水泵控制引脚
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = (1ULL << WATER_PUMP_GPIO_1) | (1ULL << WATER_PUMP_GPIO_2);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);
        
        // 初始关闭水泵
        gpio_set_level(WATER_PUMP_GPIO_1, 0);
        gpio_set_level(WATER_PUMP_GPIO_2, 0);
    }
    
    void SetWaterPumpState(bool state) {
        gpio_set_level(WATER_PUMP_GPIO_1, state ? 1 : 0);
        gpio_set_level(WATER_PUMP_GPIO_2, state ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "Water pump state set to: %s", state ? "on" : "off");
    }
    
    static void FlameMonitorTask(void* arg) {
        CustomESP32WROOM* board = static_cast<CustomESP32WROOM*>(arg);
        int adc_value = 0;
        
        while (true) {
            // 读取数字输出（低电平表示检测到火焰）
            int digital_value = gpio_get_level(FLAME_SENSOR_D_GPIO);
            bool current_flame_detected = (digital_value == 0);
            
            if (current_flame_detected && !board->flame_detected_) {
                // 刚检测到火焰
                board->flame_detected_ = true;
                
                // 读取模拟值用于报警信息
                if (board->adc_handle_ != nullptr) {
                    esp_err_t err = adc_oneshot_read(board->adc_handle_, board->flame_adc_channel_, &adc_value);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to read ADC: %s", esp_err_to_name(err));
                        adc_value = 0;
                    }
                }
                
                ESP_LOGE(TAG, "警告！检测到火焰，执行火场应急备案。ADC值: %d", adc_value);
                ESP_LOGI(TAG, "Flame sensor - Digital: %d, Analog: %d", digital_value, adc_value);
                
                // 启动水泵
                board->SetWaterPumpState(true);
                
                // 在显示屏上显示警告
                auto display = board->GetDisplay();
                if (display) {
                    display->SetStatus("火警警告！");
                    display->SetEmotion("fearful");
                    display->SetChatMessage("system", "检测到火焰！启动水泵灭火！");
                }
                
                // 进入火警模式，实时监测模拟值
                while (board->flame_detected_) {
                    // 读取数字输出
                    digital_value = gpio_get_level(FLAME_SENSOR_D_GPIO);
                    current_flame_detected = (digital_value == 0);
                    
                    if (!current_flame_detected) {
                        // 火焰消失
                        board->flame_detected_ = false;
                        ESP_LOGI(TAG, "火焰已熄灭，停止水泵");
                        
                        // 停止水泵
                        board->SetWaterPumpState(false);
                        
                        // 恢复正常显示
                        auto display = board->GetDisplay();
                        if (display) {
                            display->SetStatus(Lang::Strings::STANDBY);
                            display->SetEmotion("neutral");
                            display->SetChatMessage("system", "");
                        }
                        break;
                    }
                    
                    // 实时读取模拟值
                    if (board->adc_handle_ != nullptr) {
                        esp_err_t err = adc_oneshot_read(board->adc_handle_, board->flame_adc_channel_, &adc_value);
                        if (err == ESP_OK) {
                            // 每 200ms 输出一次模拟值
                            static int count = 0;
                            if (count % 2 == 0) {
                                ESP_LOGI(TAG, "Fire detected - ADC value: %d", adc_value);
                            }
                            count++;
                        }
                    }
                    
                    // 实时监测，200ms 一次
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
            
            // 正常模式，每 500ms 检测一次数字引脚
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

public:
    CustomESP32WROOM() : boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO), asr_button_(ASR_BUTTON_GPIO)
    {
        // 初始化风扇GPIO
        fan_gpio_ = GPIO_NUM_5;
        gpio_set_direction(fan_gpio_, GPIO_MODE_OUTPUT);
        gpio_set_level(fan_gpio_, 0); // 初始关闭风扇

        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        
        // 初始化水泵
        InitializeWaterPump();
        
        // 初始化火焰传感器
        InitializeFlameSensor();
        
        // 只有在 ADC 初始化成功后才创建火焰监测任务
        if (adc_handle_ != nullptr) {
            xTaskCreate(FlameMonitorTask, "flame_monitor", 4096, this, 5, &flame_monitor_task_);
            ESP_LOGI(TAG, "Flame monitor task started");
        } else {
            ESP_LOGW(TAG, "Flame sensor ADC not initialized, skipping flame monitor task");
        }
    }

    virtual AudioCodec* GetAudioCodec() override 
    {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    Led* GetLed() override {
        static NeopixelLed led(GPIO_NUM_13, 12);
        return &led;
    }

    std::string GetDeviceStatusJson() override {
        std::string json = WifiBoard::GetDeviceStatusJson();
        // 添加风扇状态
        bool fan_state = gpio_get_level(fan_gpio_) == 1;
        // 添加水泵状态
        bool pump_state = gpio_get_level(WATER_PUMP_GPIO_1) == 1;
        size_t pos = json.find_last_of('}');
        if (pos != std::string::npos) {
            std::string status_json = std::string(",\"fan\":{\"state\":\"") + (fan_state ? "on" : "off") + "\"}";
            status_json += std::string(",\"water_pump\":{\"state\":\"") + (pump_state ? "on" : "off") + "\"}";
            status_json += std::string(",\"flame_sensor\":{\"detected\":\"") + (flame_detected_ ? "true" : "false") + "\"}";
            json.insert(pos, status_json);
        }
        return json;
    }

    // 控制风扇
    void SetFanState(bool state) override {
        gpio_set_level(fan_gpio_, state ? 1 : 0);
        // 添加短暂延迟确保GPIO操作完成
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "Fan state set to: %s", state ? "on" : "off");
        00
    }

    // 控制RGB灯
    void SetRgbColor(int r, int g, int b) override {
        auto led = GetLed();
        if (led) {
            static_cast<NeopixelLed*>(led)->SetColor(r, g, b);
            // 添加短暂延迟确保RMT传输完成
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGI(TAG, "RGB color set to: %d, %d, %d", r, g, b);
        }
    }

    void TurnOffRgb() override {
        auto led = GetLed();
        if (led) {
            static_cast<NeopixelLed*>(led)->TurnOff();
            // 添加短暂延迟确保RMT传输完成
            vTaskDelay(pdMS_TO_TICKS(50));
            ESP_LOGI(TAG, "RGB turned off");
        }
    }
};

DECLARE_BOARD(CustomESP32WROOM);

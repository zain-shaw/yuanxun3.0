#include "neopixel_led.h"
#include <driver/rmt_tx.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <stdlib.h>

#define TAG "NeopixelLed"

// WS2812B timing constants (in nanoseconds)
#define T0H 400   // 0.4us
#define T1H 800   // 0.8us
#define T0L 850   // 0.85us
#define T1L 450   // 0.45us
#define RESET 50000 // 50us

static rmt_channel_handle_t led_chan = NULL;
static rmt_encoder_handle_t led_encoder = NULL;

NeopixelLed::NeopixelLed(int pin, int count) : pin_(pin), count_(count), current_r_(0), current_g_(0), current_b_(0), is_on_(false) {
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = (gpio_num_t)pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10MHz resolution, 1 tick = 0.1us
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    // Create a simple copy encoder
    rmt_copy_encoder_config_t encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &led_encoder));

    // Enable the channel
    ESP_ERROR_CHECK(rmt_enable(led_chan));
}

NeopixelLed::~NeopixelLed() {
    if (led_encoder) {
        rmt_del_encoder(led_encoder);
        led_encoder = NULL;
    }
    if (led_chan) {
        rmt_del_channel(led_chan);
        led_chan = NULL;
    }
}

void NeopixelLed::OnStateChanged() {
    // 状态变化时的处理
}

void NeopixelLed::SetColor(int r, int g, int b) {
    // 保存当前颜色
    current_r_ = r;
    current_g_ = g;
    current_b_ = b;
    is_on_ = (r != 0 || g != 0 || b != 0);

    // 准备RMT数据
    size_t size = count_ * 24; // 每个LED需要24位
    rmt_symbol_word_t* symbols = (rmt_symbol_word_t*)malloc(size * sizeof(rmt_symbol_word_t));
    if (!symbols) {
        ESP_LOGE(TAG, "Failed to allocate memory for RMT symbols");
        return;
    }

    // WS2812B的颜色顺序是GRB
    uint32_t color = (g << 16) | (r << 8) | b;

    // 为每个LED设置颜色
    for (int i = 0; i < count_; i++) {
        for (int j = 0; j < 24; j++) {
            if (color & (1 << (23 - j))) {
                // 1 bit: T1H high, T1L low
                symbols[i * 24 + j] = {
                    .duration0 = T1H / 100, // 0.8us / 0.1us = 8 ticks
                    .level0 = 1,
                    .duration1 = T1L / 100, // 0.45us / 0.1us = 4.5 ticks
                    .level1 = 0,
                };
            } else {
                // 0 bit: T0H high, T0L low
                symbols[i * 24 + j] = {
                    .duration0 = T0H / 100, // 0.4us / 0.1us = 4 ticks
                    .level0 = 1,
                    .duration1 = T0L / 100, // 0.85us / 0.1us = 8.5 ticks
                    .level1 = 0,
                };
            }
        }
    }

    // 发送数据
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    esp_err_t ret = rmt_transmit(led_chan, led_encoder, symbols, size * sizeof(rmt_symbol_word_t), &tx_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to transmit RMT data");
    }
    
    // 等待传输完成
    rmt_tx_wait_all_done(led_chan, pdMS_TO_TICKS(100));
    
    free(symbols);

    ESP_LOGI(TAG, "RGB color set to: R=%d, G=%d, B=%d", r, g, b);
}

void NeopixelLed::TurnOff() {
    SetColor(0, 0, 0);
    is_on_ = false;
    ESP_LOGI(TAG, "RGB turned off");
}

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_ili9341.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "adc_battery_monitor.h"
#include "application.h"
#include "button.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "display/lcd_display.h"
#include "led/single_led.h"
#include "wifi_board.h"

#define TAG "LCDWikiES3C28P"

// Minimal FT6336G reader (register compatible with the FT5x06 family).
class Ft6336Touch {
public:
    bool Init(i2c_master_bus_handle_t bus, uint8_t addr) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
        };
        return i2c_master_bus_add_device(bus, &cfg, &dev_) == ESP_OK;
    }

    bool Read(bool &touched, uint16_t &x, uint16_t &y) {
        touched = false;
        x = y = 0;
        if (!dev_) {
            return false;
        }
        uint8_t reg = 0x02;
        uint8_t buf[5];
        if (i2c_master_transmit_receive(dev_, &reg, 1, buf, 5, 50) != ESP_OK) {
            return false;
        }
        uint8_t points = buf[0] & 0x0F;
        if (points == 0) {
            return true;
        }
        touched = true;
        x = ((buf[1] & 0x0F) << 8) | buf[2];
        y = ((buf[3] & 0x0F) << 8) | buf[4];
        return true;
    }

private:
    i2c_master_dev_handle_t dev_ = nullptr;
};

class LCDWikiES3C28P : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay *display_ = nullptr;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    AdcBatteryMonitor *battery_monitor_ = nullptr;
    Ft6336Touch touch_;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = AUDIO_CODEC_I2C_NUM,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeBatteryMonitor() {
        battery_monitor_ = new AdcBatteryMonitor(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL,
                                                BATTERY_UPPER_RESISTOR, BATTERY_LOWER_RESISTOR,
                                                GPIO_NUM_NC);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = DISPLAY_SCK_PIN;
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = DISPLAY_MISO_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    static void TouchTask(void *arg) {
        auto *self = static_cast<LCDWikiES3C28P *>(arg);
        auto &app = Application::GetInstance();
        uint32_t last_tap = 0;
        uint32_t down_start = 0;
        bool down = false;

        while (true) {
            bool touched;
            uint16_t x, y;
            self->touch_.Read(touched, x, y);
            uint32_t now = esp_timer_get_time() / 1000;

            if (touched && !down) {
                down = true;
                down_start = now;
            }
            if (!touched && down) {
                down = false;
                uint32_t press = now - down_start;
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    continue;
                }
                if (press > 3000) {
                    self->EnterWifiConfigMode();
                } else if (now - last_tap < 300) {
                    app.StartListening();
                    last_tap = 0;
                } else {
                    app.ToggleChatState();
                    last_tap = now;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    void InitializeTouch() {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << TOUCH_RST_PIN,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(TOUCH_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(TOUCH_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(120));

        gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << TOUCH_INT_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&int_cfg);

        if (!touch_.Init(i2c_bus_, TOUCH_I2C_ADDR)) {
            ESP_LOGW(TAG, "Touch controller init failed, running without touch");
            return;
        }
        xTaskCreatePinnedToCore(TouchTask, "touch_task", 4096, this, 5, nullptr, 1);
        ESP_LOGI(TAG, "Touch panel ready (tap = talk, double tap = listen, long press = config)");
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    LCDWikiES3C28P() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeBatteryMonitor();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTouch();
        InitializeButtons();
        GetBacklight()->SetBrightness(100);
    }

    virtual Led *GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec *GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            i2c_bus_, AUDIO_CODEC_I2C_NUM, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true, true);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual Backlight *GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        charging = battery_monitor_->IsCharging();
        discharging = battery_monitor_->IsDischarging();
        level = battery_monitor_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(LCDWikiES3C28P);

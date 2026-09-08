#include <driver/rtc_io.h>
#include <driver/uart.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <wifi_station.h>

#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "dual_network_board.h"
#include "led/single_led.h"
#include "mcp_server.h"

#include "ai_vox3_audio_codec.h"

#define TAG "AIVOX3"

class AIVOX3 : public DualNetworkBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    i2c_master_bus_handle_t codec_i2c_bus_;
    LcdDisplay* display_;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (GetNetworkType() == NetworkType::WIFI) {
                if (app.GetDeviceState() == kDeviceStateStarting ||
                    app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                    // cast to WifiBoard
                    auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                    wifi_board.EnterWifiConfigMode();
                }
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting ||
                app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                SwitchNetworkType();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeTools() {
        static bool uart_initialized = false;
        if (!uart_initialized) {
            uart_config_t uart_config = {
                .baud_rate = 115200,
                .data_bits = UART_DATA_8_BITS,
                .parity = UART_PARITY_DISABLE,
                .stop_bits = UART_STOP_BITS_1,
                .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            };
            ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
            ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, GPIO_NUM_5, GPIO_NUM_6,
                                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
            ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, 256, 0, 0, NULL, 0));
            uart_initialized = true;
            ESP_LOGI(TAG, "PH2.0 UART初始化: TX=GPIO5, RX=GPIO6, 波特率=115200");
        }

        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.uart.send",
                           "Send data through UART serial port. Use this to control external devices connected via serial port.\n"
                           "Examples:\n"
                           "- To turn on LED: 'turn on led'\n",
                           PropertyList({Property("data", kPropertyTypeString)}),
                           [](const PropertyList &properties) -> ReturnValue
                           {
                               std::string data = properties["data"].value<std::string>();

                               if (!data.empty() && data.back() != '\n')
                               {
                                   data += "\n";
                               }
                               if (uart_write_bytes(UART_NUM_2, data.c_str(), data.length()) == data.length())
                               {
                                   ESP_LOGI(TAG, "UART data sent successfully: %s", data.c_str());
                                   return true;
                               }
                               ESP_LOGI(TAG, "Failed to send UART data: %s", data.c_str());
                               return false;
                           });

        mcp_server.AddTool("self.uart.query",
                           "Send a command to the external device via UART serial port, then wait for and return the device's reply.\n"
                           "Use this tool to READ sensor values or query status from the external Arduino. The Arduino replies with plain text.\n"
                           "Examples of commands the Arduino understands:\n"
                           "- 'read temperature' -> replies like 'Temperature: 25.50 C'\n"
                           "- 'read humidity' -> replies like 'Humidity: 65.00 %'\n"
                           "- 'read light' / 'read rain' / 'read gas' -> replies with an integer value\n",
                           PropertyList({Property("data", kPropertyTypeString)}),
                           [](const PropertyList &properties) -> ReturnValue
                           {
                               std::string data = properties["data"].value<std::string>();
                               if (!data.empty() && data.back() != '\n')
                               {
                                   data += "\n";
                               }

                               // 清空上次遗留的数据，避免读到旧的回复
                               uart_flush_input(UART_NUM_2);

                               if (uart_write_bytes(UART_NUM_2, data.c_str(), data.length()) != data.length())
                               {
                                   ESP_LOGE(TAG, "Failed to send UART query: %s", data.c_str());
                                   return std::string("ERROR: uart send failed");
                               }

                               // 等待 Arduino 回复：累计读取，直到 200ms 内无新数据（最长等 3 秒）
                               std::string reply;
                               char buf[128];
                               const int64_t kSilenceUs = 200 * 1000;       // 200ms 静默即认为回复结束
                               const int64_t kMaxWaitUs = 1500 * 1000;      // 兜底上限 1.5 秒
                               int64_t last_data_us = esp_timer_get_time();
                               const int64_t start_us = last_data_us;
                               while ((esp_timer_get_time() - last_data_us) < kSilenceUs &&
                                      (esp_timer_get_time() - start_us) < kMaxWaitUs)
                               {
                                   int len = uart_read_bytes(UART_NUM_2, buf, sizeof(buf) - 1, pdMS_TO_TICKS(50));
                                   if (len > 0)
                                   {
                                       buf[len] = '\0';
                                       reply += buf;
                                       last_data_us = esp_timer_get_time();
                                   }
                               }

                               // 去掉 Arduino 的回显行（Received: ...）和调试行（[DBG] ...），只保留有效内容
                               std::string cleaned;
                               size_t pos = 0;
                               while (pos <= reply.size())
                               {
                                   size_t eol = reply.find('\n', pos);
                                   if (eol == std::string::npos)
                                       eol = reply.size();
                                   std::string line = reply.substr(pos, eol - pos);
                                   pos = eol + 1;

                                   // 去掉行首行尾空白
                                   size_t b = line.find_first_not_of(" \t\r");
                                   size_t e = line.find_last_not_of(" \t\r");
                                   if (b == std::string::npos)
                                       continue; // 空行跳过
                                   line = line.substr(b, e - b + 1);

                                   if (line.rfind("Received:", 0) == 0)
                                       continue; // 回显行跳过
                                   if (line.rfind("[DBG]", 0) == 0)
                                       continue; // 调试行跳过
                                   if (!cleaned.empty())
                                       cleaned += "\n";
                                   cleaned += line;
                               }

                               if (cleaned.empty())
                                   return std::string("NO_REPLY");
                               return cleaned;
                           });
    }

public:
    AIVOX3()
        : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, DEFAULT_4G_NETWORK),
          boot_button_(BOOT_BUTTON_GPIO),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO),
          volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static AIVOX3AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

};

DECLARE_BOARD(AIVOX3);

#ifndef _AMBIENT_LED_STRIP_H_
#define _AMBIENT_LED_STRIP_H_

#include <cstdint>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <led_strip.h>
#include <mutex>

/**
 * @brief 灯带效果枚举
 */
enum class AmbientLedStripEffect {
    NONE = 0,           // 无效果（静态显示）
    SOLID_COLOR,        // 单色填充
    FLOW,               // 流水灯
    RAINBOW,            // 彩虹效果
    BREATHE             // 呼吸灯
};

#define DEFAULT_BRIGHTNESS 127
#define DEFAULT_MAX_LEDS 10
// #define DEFAULT_RESOLUTION_HZ 10 * 1000 * 1000
#define DEFAULT_RESOLUTION_HZ 30 * 1000 * 1000   //灯珠数量增加该值应该调大(同时注意连接线材不要过长)

/**
 * @brief WS2812灯带控制类
 * 
 * 使用ESP32 RMT实现WS2812灯带控制
 * 支持多种显示效果：单色、流水灯、彩虹、呼吸灯
 */
class AmbientLedStrip {
public:

    AmbientLedStrip(gpio_num_t gpio_num, 
             uint32_t max_leds = DEFAULT_MAX_LEDS,
             uint32_t resolution_hz = DEFAULT_RESOLUTION_HZ);

    ~AmbientLedStrip();

    esp_err_t Init();

    uint32_t GetMaxLeds() const;

    esp_err_t SetSolidColor(uint8_t r, uint8_t g, uint8_t b);

    esp_err_t SetPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

    esp_err_t Clear();

    // apply current buffer to led strip
    esp_err_t Refresh();

    /**
     * @brief 启动流水灯效果
     * @param r 红色值（0-255）
     * @param g 绿色值（0-255）
     * @param b 蓝色值（0-255）
     * @param interval_ms 流水速度（毫秒）
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t StartFlow(uint8_t r, uint8_t g, uint8_t b, uint32_t interval_ms);

    /**
     * @brief 启动彩虹效果
     * @param interval_ms 颜色变化速度（毫秒）
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t StartRainbow(uint32_t interval_ms);

    /**
     * @brief 启动呼吸灯效果
     * @param r 红色值（0-255）
     * @param g 绿色值（0-255）
     * @param b 蓝色值（0-255）
     * @param interval_ms 呼吸速度（毫秒）
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t StartBreathe(uint8_t r, uint8_t g, uint8_t b, uint32_t interval_ms=30);

    /**
     * @brief 停止当前效果
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t StopEffect();

    /**
     * @brief 获取当前效果类型
     * @return 当前效果枚举
     */
    AmbientLedStripEffect GetEffect() const { return current_effect_; }

    /**
     * @brief 设置全局亮度
     * @param brightness 亮度值（0-100，0为最暗，100为最亮）
     * @return ESP_OK表示成功，其他值表示失败
     */
    esp_err_t SetBrightness(uint8_t brightness);

    /**
     * @brief 获取当前亮度
     * @return 当前亮度值（0-100）
     */
    uint8_t GetBrightness() const;

    void  SetBrightnessLimit(bool enable);

private:
    gpio_num_t gpio_num_;                   // GPIO引脚号
    uint32_t max_leds_;                     // LED数量
    uint32_t resolution_hz_;                // RMT分辨率
    led_strip_handle_t led_strip_;          // LED灯带句柄
    bool initialized_;                      // 初始化标志
    AmbientLedStripEffect current_effect_;         // 当前效果类型
    esp_timer_handle_t effect_timer_;       // 效果定时器句柄

    // 效果相关参数
    uint8_t effect_r_, effect_g_, effect_b_;  // 效果颜色
    uint32_t effect_interval_ms_;             // 效果间隔时间
    uint32_t effect_step_;                    // 效果步进值
    uint8_t brightness_;                      // 全局亮度（0-255）
    bool  brightness_limit_enabled_ = false;  //  是否开启亮度限制
    uint8_t brightness_max_;                  // 最大亮度值,用于限制电流,灯珠数量增加该值应该调小(0-255)

    // 当前单色效果的基础颜色（未应用亮度前的原始RGB），用于在亮度变化时实时重绘
    uint8_t solid_base_r_;
    uint8_t solid_base_g_;
    uint8_t solid_base_b_;

    // 亮度读写互斥锁，保护 brightness_ 在多任务和定时器回调中的并发访问
    mutable std::mutex brightness_mutex_;


    // RMT refresh/clear 互斥锁：底层 RMT 通道在 enable→transmit→disable 期间不允许并发，
    // 否则会触发 "channel not in init state"。串行化所有 refresh/clear 调用。
    mutable std::mutex refresh_mutex_;

    /**
     * @brief 效果更新定时器回调
     * @param arg 参数指针（this指针）
     */
    static void EffectTimerCallback(void* arg);

    /**
     * @brief 更新流水灯效果
     */
    void UpdateFlowEffect();

    /**
     * @brief 更新彩虹效果
     */
    void UpdateRainbowEffect();

    /**
     * @brief 更新呼吸灯效果
     */
    void UpdateBreatheEffect();

    /**
     * @brief HSV转RGB
     * @param h 色相（0-360）
     * @param s 饱和度（0-255）
     * @param v 明度（0-255）
     * @param r 输出的红色值
     * @param g 输出的绿色值
     * @param b 输出的蓝色值
     */
    void HsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b);

    /**
     * @brief 应用亮度系数到RGB颜色值
     * @param r 输入的红色值（0-255）
     * @param g 输入的绿色值（0-255）
     * @param b 输入的蓝色值（0-255）
     * @param out_r 输出的红色值（应用亮度后）
     * @param out_g 输出的绿色值（应用亮度后）
     * @param out_b 输出的蓝色值（应用亮度后）
     */
    void ApplyBrightness(uint8_t r, uint8_t g, uint8_t b, uint8_t* out_r, uint8_t* out_g, uint8_t* out_b) const;
};

#endif  // _AMBIENT_LED_STRIP_H_
#include "ambient_led_strip.h"
#include <algorithm>
#include <cmath>
#include <esp_log.h>
#include <esp_timer.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAG "AmbientLedStrip"

// 亮度查找表，用于非线性呼吸效果
// 增加几个100,适当延长呼吸时间
static const uint8_t kBreatheBrightnessLut[] = {
    0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6,
    6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11, 12, 12, 13, 13, 14, 15,
    15, 16, 17, 17, 18, 19, 19, 20, 21, 22, 23, 23, 24, 25, 26, 27, 28, 29, 30,
    31, 32, 34, 35, 36, 37, 39, 40, 41, 43, 44, 46, 48, 49, 51, 53, 54, 56, 58,
    60, 62, 64, 66, 68, 71, 73, 75, 78, 80, 83, 85, 88, 91, 94, 97, 100,
    100,100,100,100,100
};

AmbientLedStrip::AmbientLedStrip(gpio_num_t gpio_num, uint32_t max_leds, uint32_t resolution_hz)
    : gpio_num_(gpio_num)
    , max_leds_(max_leds)
    , resolution_hz_(resolution_hz)
    , led_strip_(nullptr)
    , initialized_(false)
    , current_effect_(AmbientLedStripEffect::NONE)
    , effect_timer_(nullptr)
    , effect_r_(0)
    , effect_g_(0)
    , effect_b_(0)
    , effect_interval_ms_(0)
    , effect_step_(0)
    , brightness_(DEFAULT_BRIGHTNESS)
    , solid_base_r_(0)
    , solid_base_g_(0)
    , solid_base_b_(0)
{
    // brightness_mutex_ 为 std::mutex 类型，无需显式初始化
}

AmbientLedStrip::~AmbientLedStrip() {
    StopEffect();
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
        led_strip_ = nullptr;
    }
    if (effect_timer_ != nullptr) {
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
    }
}

esp_err_t AmbientLedStrip::Init() {
    if (initialized_) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio_num_,
        .max_leds = max_leds_,
        .led_model = LED_MODEL_WS2812,
        .flags = {
            .invert_out = false
        }
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution_hz_,
        .flags = {
            .with_dma = false
        }
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip device");
        return ret;
    }

    Clear();
    Refresh();
    // set brightness max
    int max_brightness = 255 - (max_leds_ * 3) - 50;
    if(max_brightness < 0) {
        max_brightness = 10; //默认最小值
    }
    brightness_max_ = static_cast<uint8_t>(max_brightness);

    initialized_ = true;
    ESP_LOGI(TAG, "LED strip initialized on GPIO %d with %ld LEDs", gpio_num_, (long)max_leds_);
    return ESP_OK;
}

esp_err_t AmbientLedStrip::SetSolidColor(uint8_t r, uint8_t g, uint8_t b) {
    if (!initialized_ || led_strip_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    StopEffect();
    current_effect_ = AmbientLedStripEffect::SOLID_COLOR;

    // 记录当前单色效果的基础颜色，便于后续亮度变化时实时重绘
    solid_base_r_ = r;
    solid_base_g_ = g;
    solid_base_b_ = b;

    uint8_t out_r, out_g, out_b;
    ApplyBrightness(r, g, b, &out_r, &out_g, &out_b);
    for (uint8_t i = 0; i < max_leds_; i++) {
        esp_err_t ret = led_strip_set_pixel(led_strip_, i, out_r, out_g, out_b);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return Refresh();
}

uint32_t AmbientLedStrip::GetMaxLeds() const {
    return max_leds_;
}

esp_err_t AmbientLedStrip::SetPixel(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (!initialized_ || led_strip_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index >= max_leds_) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t out_r, out_g, out_b;
    ApplyBrightness(r, g, b, &out_r, &out_g, &out_b);
    int ret = led_strip_set_pixel(led_strip_, index, out_r, out_g, out_b);
    if (ret != ESP_OK) {
        return ret;
    }
    return Refresh();
}

esp_err_t AmbientLedStrip::Clear() {
    if (!initialized_ || led_strip_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    StopEffect();
    std::lock_guard<std::mutex> lock(refresh_mutex_);
    return led_strip_clear(led_strip_);
}

esp_err_t AmbientLedStrip::Refresh() {
    if (!initialized_ || led_strip_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    std::lock_guard<std::mutex> lock(refresh_mutex_);
    return led_strip_refresh(led_strip_);
}

esp_err_t AmbientLedStrip::StartFlow(uint8_t r, uint8_t g, uint8_t b, uint32_t interval_ms) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    StopEffect();

    current_effect_ = AmbientLedStripEffect::FLOW;
    effect_r_ = r;
    effect_g_ = g;
    effect_b_ = b;
    effect_interval_ms_ = interval_ms;
    effect_step_ = 0;

    for (uint32_t i = 0; i < max_leds_; i++) {
        led_strip_set_pixel(led_strip_, i, 0, 0, 0);
    }

    const esp_timer_create_args_t timer_args = {
        .callback = EffectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_strip_flow",
        .skip_unhandled_events = false
    };

    esp_err_t ret = esp_timer_create(&timer_args, &effect_timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create flow timer");
        return ret;
    }

    // immediately execute once
    UpdateFlowEffect();

    ret = esp_timer_start_periodic(effect_timer_, interval_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start flow timer");
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
        return ret;
    }

    ESP_LOGI(TAG, "Flow effect started");
    return ESP_OK;
}

esp_err_t AmbientLedStrip::StartRainbow(uint32_t interval_ms) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    StopEffect();

    current_effect_ = AmbientLedStripEffect::RAINBOW;
    effect_interval_ms_ = interval_ms;
    effect_step_ = 0;

    const esp_timer_create_args_t timer_args = {
        .callback = EffectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_strip_rainbow",
        .skip_unhandled_events = false
    };

    esp_err_t ret = esp_timer_create(&timer_args, &effect_timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create rainbow timer");
        return ret;
    }

    // immediately execute once
    UpdateRainbowEffect();

    // 启动周期性定时器
    ret = esp_timer_start_periodic(effect_timer_, interval_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start rainbow timer");
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
        return ret;
    }

    ESP_LOGI(TAG, "Rainbow effect started");
    return ESP_OK;
}

// interval_ms: 效果更新定时器周期(20-50ms最佳)
esp_err_t AmbientLedStrip::StartBreathe(uint8_t r, uint8_t g, uint8_t b, uint32_t interval_ms) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    StopEffect();

    current_effect_ = AmbientLedStripEffect::BREATHE;
    effect_r_ = r;
    effect_g_ = g;
    effect_b_ = b;
    effect_interval_ms_ = interval_ms;
    effect_step_ = 0;

    const esp_timer_create_args_t timer_args = {
        .callback = EffectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_strip_breathe",
        .skip_unhandled_events = false
    };

    esp_err_t ret = esp_timer_create(&timer_args, &effect_timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create breathe timer");
        return ret;
    }

    // immediately execute once
    UpdateBreatheEffect();

    ret = esp_timer_start_periodic(effect_timer_, interval_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start breathe timer");
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
        return ret;
    }

    ESP_LOGI(TAG, "Breathe effect started");
    return ESP_OK;
}

// interval_ms: 单次亮/灭持续时间，完整闪烁周期为 2*interval_ms
esp_err_t AmbientLedStrip::StartBlink(uint8_t r, uint8_t g, uint8_t b, uint32_t interval_ms) {
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    StopEffect();

    current_effect_ = AmbientLedStripEffect::BLINK;
    effect_r_ = r;
    effect_g_ = g;
    effect_b_ = b;
    effect_interval_ms_ = interval_ms;
    effect_step_ = 0;

    const esp_timer_create_args_t timer_args = {
        .callback = EffectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "led_strip_blink",
        .skip_unhandled_events = false
    };

    esp_err_t ret = esp_timer_create(&timer_args, &effect_timer_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create blink timer");
        return ret;
    }

    // immediately execute once
    UpdateBlinkEffect();

    ret = esp_timer_start_periodic(effect_timer_, interval_ms * 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start blink timer");
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
        return ret;
    }

    ESP_LOGI(TAG, "Blink effect started");
    return ESP_OK;
}

esp_err_t AmbientLedStrip::StopEffect() {
    if (effect_timer_ != nullptr) {
        esp_timer_stop(effect_timer_);
        esp_timer_delete(effect_timer_);
        effect_timer_ = nullptr;
    }
    current_effect_ = AmbientLedStripEffect::NONE;
    {
        std::lock_guard<std::mutex> lock(refresh_mutex_);
    }
    return ESP_OK;
}

void AmbientLedStrip::EffectTimerCallback(void* arg) {
    AmbientLedStrip* strip = static_cast<AmbientLedStrip*>(arg);
    if (strip == nullptr) {
        return;
    }

    switch (strip->current_effect_) {
        case AmbientLedStripEffect::FLOW:
            strip->UpdateFlowEffect();
            break;
        case AmbientLedStripEffect::RAINBOW:
            strip->UpdateRainbowEffect();
            break;
        case AmbientLedStripEffect::BREATHE:
            strip->UpdateBreatheEffect();
            break;
        case AmbientLedStripEffect::BLINK:
            strip->UpdateBlinkEffect();
            break;
        default:
            break;
    }
}

void AmbientLedStrip::UpdateFlowEffect() {
    for (uint32_t i = 0; i < max_leds_; i++) {
        led_strip_set_pixel(led_strip_, i, 0, 0, 0);
    }

    uint8_t out_r, out_g, out_b;
    ApplyBrightness(effect_r_, effect_g_, effect_b_, &out_r, &out_g, &out_b);

    uint8_t current_led = effect_step_ % max_leds_;
    led_strip_set_pixel(led_strip_, current_led, out_r, out_g, out_b);

    Refresh();

    effect_step_ = (effect_step_ + 1) % max_leds_;
}

void AmbientLedStrip::UpdateRainbowEffect() {
    // 为每个LED计算不同的色相值
    for (uint8_t i = 0; i < max_leds_; i++) {
        // 计算色相：每个LED偏移不同角度，加上整体旋转
        uint16_t hue = (effect_step_ * 360 / max_leds_ + i * 360 / max_leds_) % 360;
        uint8_t s = 255;  // 饱和度
        uint8_t v = 128;  // 明度

        uint8_t r, g, b;
        HsvToRgb(hue, s, v, &r, &g, &b);
        
        // 应用亮度系数
        uint8_t out_r, out_g, out_b;
        ApplyBrightness(r, g, b, &out_r, &out_g, &out_b);
        led_strip_set_pixel(led_strip_, i, out_r, out_g, out_b);
    }

    Refresh();

    // 更新步进值（控制彩虹旋转速度）
    effect_step_ = (effect_step_ + 1) % (360 * max_leds_);
}

void AmbientLedStrip::UpdateBreatheEffect() {
    // 查表法呼吸灯：使用预定义亮度查找表实现非线性明暗变化
    const uint32_t lut_size = sizeof(kBreatheBrightnessLut) / sizeof(kBreatheBrightnessLut[0]); // 100
    const uint32_t steps_per_cycle = lut_size * 2;  // 一次完整呼吸包含淡入和淡出两个阶段

    uint32_t step_in_cycle = effect_step_ % steps_per_cycle;

    // 先上升再下降（0 -> lut_size-1 -> 0）的三角波索引
    uint32_t lut_index;
    if (step_in_cycle < lut_size) {
        // 淡入阶段：0 -> lut_size-1
        lut_index = step_in_cycle;
    } else {
        // 淡出阶段：lut_size-1 -> 0
        lut_index = steps_per_cycle - 1 - step_in_cycle;
    }

    // 将查找表中的 0-100 亮度比例应用到基础颜色上
    float brightness_ratio = kBreatheBrightnessLut[lut_index] / 100.0f;
    uint8_t r = static_cast<uint8_t>(static_cast<float>(effect_r_) * brightness_ratio);
    uint8_t g = static_cast<uint8_t>(static_cast<float>(effect_g_) * brightness_ratio);
    uint8_t b = static_cast<uint8_t>(static_cast<float>(effect_b_) * brightness_ratio);

    uint8_t out_r, out_g, out_b;
    ApplyBrightness(r, g, b, &out_r, &out_g, &out_b);

    for (uint32_t i = 0; i < max_leds_; i++) {
        led_strip_set_pixel(led_strip_, i, out_r, out_g, out_b);
    }
    Refresh();

    effect_step_++;
}

void AmbientLedStrip::UpdateBlinkEffect() {
    // 偶数步点亮，奇数步熄灭，形成亮灭交替的闪烁效果
    if (effect_step_ % 2 == 0) {
        uint8_t out_r, out_g, out_b;
        ApplyBrightness(effect_r_, effect_g_, effect_b_, &out_r, &out_g, &out_b);
        for (uint32_t i = 0; i < max_leds_; i++) {
            led_strip_set_pixel(led_strip_, i, out_r, out_g, out_b);
        }
        Refresh();
    } else {
        for (uint32_t i = 0; i < max_leds_; i++) {
            led_strip_set_pixel(led_strip_, i, 0, 0, 0);
        }
        Refresh();
    }

    effect_step_++;
}

void AmbientLedStrip::HsvToRgb(uint16_t h, uint8_t s, uint8_t v, uint8_t* r, uint8_t* g, uint8_t* b) {
    uint8_t region, remainder, p, q, t;

    if (s == 0) {
        *r = *g = *b = v;
        return;
    }

    region = h / 43;
    remainder = (h - (region * 43)) * 6;

    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0:
            *r = v; *g = t; *b = p;
            break;
        case 1:
            *r = q; *g = v; *b = p;
            break;
        case 2:
            *r = p; *g = v; *b = t;
            break;
        case 3:
            *r = p; *g = q; *b = v;
            break;
        case 4:
            *r = t; *g = p; *b = v;
            break;
        default:
            *r = v; *g = p; *b = q;
            break;
    }
}

esp_err_t AmbientLedStrip::SetBrightness(uint8_t brightness) {
    if (brightness > 100) {
        brightness = 100;
    }
    // 使用互斥锁保护亮度设置，避免与定时器回调并发访问
    std::lock_guard<std::mutex> lock(brightness_mutex_);
    brightness_ = (brightness * 255) / 100;

    if(brightness_limit_enabled_ && brightness_ > brightness_max_) {
        brightness_ = brightness_max_;
    }
    // 如果当前是单色效果，则根据新的亮度系数实时刷新整条灯带
    if (current_effect_ == AmbientLedStripEffect::SOLID_COLOR && led_strip_ != nullptr) {
        uint8_t out_r, out_g, out_b;
        out_r = (solid_base_r_ * brightness_) / 255;
        out_g = (solid_base_g_ * brightness_) / 255;
        out_b = (solid_base_b_ * brightness_) / 255;
        for (uint32_t led_index = 0; led_index < max_leds_; ++led_index) {
            led_strip_set_pixel(led_strip_, led_index, out_r, out_g, out_b);
        }
        return Refresh();
    }

    // 其他效果（流水/彩虹/呼吸）在下一次定时器回调时会读取新的亮度值并自动生效
    return ESP_OK;
}

uint8_t AmbientLedStrip::GetBrightness() const {
    return (brightness_ * 100) / 255;
}

void AmbientLedStrip::ApplyBrightness(uint8_t r, uint8_t g, uint8_t b, uint8_t* out_r, uint8_t* out_g, uint8_t* out_b) const {
    // 读取当前亮度时加锁，保证与 SetBrightness 并发安全
    std::lock_guard<std::mutex> lock(brightness_mutex_);
    uint8_t current_brightness = brightness_;
    *out_r = (r * current_brightness) / 255;
    *out_g = (g * current_brightness) / 255;
    *out_b = (b * current_brightness) / 255;
}

void AmbientLedStrip::SetBrightnessLimit(bool enable) {
    brightness_limit_enabled_ = enable;
}
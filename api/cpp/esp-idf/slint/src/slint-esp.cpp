// Copyright © SixtyFPS GmbH <info@slint.dev>
// SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-Slint-Royalty-free-2.0 OR LicenseRef-Slint-Software-3.0

#include <deque>
#include <mutex>
#include "slint-esp.h"
#include "slint-platform.h"
#include "esp_lcd_panel_ops.h"
#if __has_include("soc/soc_caps.h")
#    include "soc/soc_caps.h"
#endif
#if SOC_LCD_RGB_SUPPORTED && ESP_IDF_VERSION_MAJOR >= 5
#    include "esp_lcd_panel_rgb.h"
#endif
#include "esp_log.h"

static const char *TAG = "slint_platform";

using RepaintBufferType = slint::platform::SoftwareRenderer::RepaintBufferType;

struct EspPlatform : public slint::platform::Platform
{
    EspPlatform(const SlintPlatformConfiguration &config)
        : size(config.size),
          panel_handle(config.panel),
          touch_handle(config.touch),
          buffer1(config.buffer1),
          buffer2(config.buffer2),
          color_swap_16(config.color_swap_16),
          rotation(config.rotation)
    {
#if !defined(SLINT_FEATURE_EXPERIMENTAL)
        if (!buffer1 && !buffer2) {
            ESP_LOGI(TAG,
                     "FATAL: Line-by-line rendering requested for use with slint-esp is "
                     "experimental and not enabled in this build.");
        }
#endif
    }

    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override;

    std::chrono::milliseconds duration_since_start() override;
    void run_event_loop() override;
    void quit_event_loop() override;
    void run_in_event_loop(Task) override;

private:
    slint::PhysicalSize size;
    esp_lcd_panel_handle_t panel_handle;
    esp_lcd_touch_handle_t touch_handle;
    std::optional<std::span<slint::platform::Rgb565Pixel>> buffer1;
    std::optional<std::span<slint::platform::Rgb565Pixel>> buffer2;
    bool color_swap_16;
    slint::platform::SoftwareRenderer::RenderingRotation rotation;
    class EspWindowAdapter *m_window = nullptr;

    // Need to be static because we can't pass user data to the touch interrupt callback
    static TaskHandle_t task;
    std::mutex queue_mutex;
    std::deque<slint::platform::Platform::Task> queue; // protected by queue_mutex
    bool quit = false; // protected by queue_mutex
};

class EspWindowAdapter : public slint::platform::WindowAdapter
{
public:
    slint::platform::SoftwareRenderer m_renderer;
    bool needs_redraw = true;
    const slint::PhysicalSize m_size;

    explicit EspWindowAdapter(RepaintBufferType buffer_type, slint::PhysicalSize size)
        : m_renderer(buffer_type), m_size(size)
    {
    }

    slint::platform::AbstractRenderer &renderer() override { return m_renderer; }

    slint::PhysicalSize size() override { return m_size; }

    void request_redraw() override { needs_redraw = true; }
};

std::unique_ptr<slint::platform::WindowAdapter> EspPlatform::create_window_adapter()
{
    if (m_window != nullptr) {
        ESP_LOGI(TAG, "FATAL: create_window_adapter called multiple times");
        return nullptr;
    }

    auto buffer_type =
            buffer2 ? RepaintBufferType::SwappedBuffers : RepaintBufferType::ReusedBuffer;
    auto window = std::make_unique<EspWindowAdapter>(buffer_type, size);
    m_window = window.get();
    m_window->m_renderer.set_rendering_rotation(rotation);
    return window;
}

std::chrono::milliseconds EspPlatform::duration_since_start()
{
    auto ticks = xTaskGetTickCount();
    return std::chrono::milliseconds(pdTICKS_TO_MS(ticks));
}

#if SOC_LCD_RGB_SUPPORTED && ESP_IDF_VERSION_MAJOR >= 5
static SemaphoreHandle_t sem_vsync_end;
static SemaphoreHandle_t sem_gui_ready;

extern "C" bool on_vsync_event(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t *edata, void *)
{
    BaseType_t high_task_awoken = pdFALSE;
    if (xSemaphoreTakeFromISR(sem_gui_ready, &high_task_awoken) == pdTRUE) {
        xSemaphoreGiveFromISR(sem_vsync_end, &high_task_awoken);
    }
    return high_task_awoken == pdTRUE;
}
#endif

void EspPlatform::run_event_loop()
{
    task = xTaskGetCurrentTaskHandle();

    esp_lcd_panel_disp_on_off(panel_handle, true);

    TickType_t max_ticks_to_wait = portMAX_DELAY;

    if (touch_handle) {
        if (esp_lcd_touch_register_interrupt_callback(
                    touch_handle, [](auto) { vTaskNotifyGiveFromISR(task, nullptr); })
            != ESP_OK) {

            // No touch interrupt assigned or supported? Fall back to polling like esp_lvgl_port.
            // LVGL polls in 5ms intervals, but FreeRTOS tick interval is 10ms, so go for that
            max_ticks_to_wait = pdMS_TO_TICKS(10);
        }
    }
#if SOC_LCD_RGB_SUPPORTED && ESP_IDF_VERSION_MAJOR >= 5
    if (buffer2) {
        sem_vsync_end = xSemaphoreCreateBinary();
        sem_gui_ready = xSemaphoreCreateBinary();
        esp_lcd_rgb_panel_event_callbacks_t cbs = {};
        cbs.on_vsync = on_vsync_event;
        esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, this);
    }
#endif

    int last_touch_x = 0;
    int last_touch_y = 0;
    bool touch_down = false;

    while (true) {
        slint::platform::update_timers_and_animations();

        std::optional<slint::platform::Platform::Task> event;
        {
            std::unique_lock lock(queue_mutex);
            if (queue.empty()) {
                if (quit) {
                    quit = false;
                    break;
                }
            } else {
                event = std::move(queue.front());
                queue.pop_front();
            }
        }
        if (event) {
            std::move(*event).run();
            event.reset();
            continue;
        }

        if (m_window) {

            if (touch_handle) {
                uint16_t touchpad_x[1] = { 0 };
                uint16_t touchpad_y[1] = { 0 };
                uint8_t touchpad_cnt = 0;

                /* Read touch controller data */
                esp_lcd_touch_read_data(touch_handle);

                /* Get coordinates */
                bool touchpad_pressed = esp_lcd_touch_get_coordinates(
                        touch_handle, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

                if (touchpad_pressed && touchpad_cnt > 0) {
                    // ESP_LOGI(TAG, "x: %i, y: %i", touchpad_x[0], touchpad_y[0]);
                    last_touch_x = touchpad_x[0];
                    last_touch_y = touchpad_y[0];
                    m_window->window().dispatch_pointer_move_event(
                            slint::LogicalPosition({ float(last_touch_x), float(last_touch_y) }));
                    if (!touch_down) {
                        m_window->window().dispatch_pointer_press_event(
                                slint::LogicalPosition(
                                        { float(last_touch_x), float(last_touch_y) }),
                                slint::PointerEventButton::Left);
                    }
                    touch_down = true;
                } else if (touch_down) {
                    m_window->window().dispatch_pointer_release_event(
                            slint::LogicalPosition({ float(last_touch_x), float(last_touch_y) }),
                            slint::PointerEventButton::Left);
                    m_window->window().dispatch_pointer_exit_event();
                    touch_down = false;
                }
            }

            if (std::exchange(m_window->needs_redraw, false)) {
                auto rotated =
                        rotation == slint::platform::SoftwareRenderer::RenderingRotation::Rotate90
                        || rotation
                                == slint::platform::SoftwareRenderer::RenderingRotation::Rotate270;
                if (buffer1) {
                    auto region = m_window->m_renderer.render(buffer1.value(),
                                                              rotated ? size.height : size.width);

                    if (color_swap_16) {
                        for (auto [o, s] : region.rectangles()) {
                            for (int y = o.y; y < o.y + s.height; y++) {
                                for (int x = o.x; x < o.x + s.width; x++) {
                                    // Swap endianess to big endian
                                    auto px = reinterpret_cast<uint16_t *>(
                                            &buffer1.value()[y * size.width + x]);
                                    *px = (*px << 8) | (*px >> 8);
                                }
                            }
                        }
                    }

                    if (buffer2) {
                        auto s = region.bounding_box_size();
                        if (s.width > 0 && s.height > 0) {
#if SOC_LCD_RGB_SUPPORTED && ESP_IDF_VERSION_MAJOR >= 5
                            xSemaphoreGive(sem_gui_ready);
                            xSemaphoreTake(sem_vsync_end, portMAX_DELAY);
#endif

                            // Assuming that using double buffer means that the buffer comes from
                            // the driver and we need to pass the exact pointer.
                            // https://github.com/espressif/esp-idf/blob/53ff7d43dbff642d831a937b066ea0735a6aca24/components/esp_lcd/src/esp_lcd_panel_rgb.c#L681
                            esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, size.width, size.height,
                                                      buffer1->data());

                            std::swap(buffer1, buffer2);
                        }
                    } else {
                        for (auto [o, s] : region.rectangles()) {
                            for (int y = o.y; y < o.y + s.height; y++) {
                                esp_lcd_panel_draw_bitmap(panel_handle, o.x, y, o.x + s.width,
                                                          y + 1,
                                                          buffer1->data() + y * size.width + o.x);
                            }
                        }
                    }
#ifdef SLINT_FEATURE_EXPERIMENTAL
                } else {
                    std::unique_ptr<slint::platform::Rgb565Pixel, void (*)(void *)> lb(
                            static_cast<slint::platform::Rgb565Pixel *>(
                                    heap_caps_malloc((rotated ? size.height : size.width)
                                                             * sizeof(slint::platform::Rgb565Pixel),
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                            heap_caps_free);
                    m_window->m_renderer.render_by_line([this, &lb](std::size_t line_y,
                                                                    std::size_t line_start,
                                                                    std::size_t line_end,
                                                                    auto &&render_fn) {
                        std::span<slint::platform::Rgb565Pixel> view { lb.get(),
                                                                       line_end - line_start };
                        render_fn(view);
                        if (color_swap_16) {
                            // Swap endianess to big endian
                            std::for_each(view.begin(), view.end(), [](auto &rgbpix) {
                                auto px = reinterpret_cast<uint16_t *>(&rgbpix);
                                *px = (*px << 8) | (*px >> 8);
                            });
                        }
                        esp_lcd_panel_draw_bitmap(panel_handle, line_start, line_y, line_end,
                                                  line_y + 1, lb.get());
                    });
#endif
                }
            }

            if (m_window->window().has_active_animations()) {
                continue;
            }
        }

        TickType_t ticks_to_wait = max_ticks_to_wait;
        if (auto wait_time = slint::platform::duration_until_next_timer_update()) {
            ticks_to_wait = std::min(ticks_to_wait, pdMS_TO_TICKS(wait_time->count()));
        }

        ulTaskNotifyTake(/*reset to zero*/ pdTRUE, ticks_to_wait);
    }

    vTaskDelete(NULL);
}

void EspPlatform::quit_event_loop()
{
    {
        const std::unique_lock lock(queue_mutex);
        quit = true;
    }
    vTaskNotifyGiveFromISR(task, nullptr);
}

void EspPlatform::run_in_event_loop(slint::platform::Platform::Task event)
{
    {
        const std::unique_lock lock(queue_mutex);
        queue.push_back(std::move(event));
    }
    vTaskNotifyGiveFromISR(task, nullptr);
}

TaskHandle_t EspPlatform::task = {};

void slint_esp_init(slint::PhysicalSize size, esp_lcd_panel_handle_t panel,
                    std::optional<esp_lcd_touch_handle_t> touch,
                    std::span<slint::platform::Rgb565Pixel> buffer1,
                    std::optional<std::span<slint::platform::Rgb565Pixel>> buffer2)
{

    SlintPlatformConfiguration config {
        .size = size,
        .panel = panel,
        .touch = touch ? *touch : nullptr,
        .buffer1 = buffer1,
        .buffer2 = buffer2,
        // For compatibility with earlier versions of Slint, we compute the value of
        // color_swap_16 the way it was implemented in Slint (slint-esp) <= 1.6.0:
        .color_swap_16 = buffer2.has_value()
    };
    slint_esp_init(config);
}

#ifdef SLINT_FEATURE_EXPERIMENTAL
void slint_esp_init(slint::PhysicalSize size, esp_lcd_panel_handle_t panel,
                    std::optional<esp_lcd_touch_handle_t> touch,
                    slint::platform::SoftwareRenderer::RenderingRotation rotation)
{
    SlintPlatformConfiguration config { .size = size,
                                        .panel = panel,
                                        .touch = touch ? *touch : nullptr,
                                        .buffer1 = std::nullopt,
                                        .buffer2 = std::nullopt,
                                        .rotation = rotation,
                                        .color_swap_16 = false };
    slint_esp_init(config);
}
#endif

void slint_esp_init(const SlintPlatformConfiguration &config)
{
    slint::platform::set_platform(std::make_unique<EspPlatform>(config));
}

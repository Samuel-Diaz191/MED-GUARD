/*
 * EEL 4775 — Final Integration Capstone
 * MedGuard: Real-Time ECG Monitoring System
 * (built on the Application 5 dual-core IPC pipeline spine, with WCET
 *  measurement infrastructure folded in from Application 2)
 *
 * Theme: MEDGUARD — ECG Sample -> Arrhythmia Decision -> Alert
 *
 * Scaffold Code - AI usage:
 *   Addition of the USE_WEBSERVER compile-time switch and a working serial
 *     monitor task on Core 0, plus per-task heartbeat counters
 *   Logic to allow for switching between a serial monitor and the (student-built)
 *     web monitor, so the pipeline runs in Wokwi with no Wi-Fi by default
 *   Commenting of code including human readable summaries
 *   Theme implementation (producer/consumer bodies, web monitor, queue sizing) —
 *     done with the help of Claude (Anthropic). See README for full AI-usage
 *     disclosure and citation of what was reused from Apps 1-3.
 *
 * ============================================================
 * PIPELINE (medical theme)
 * ============================================================
 *   ecg_sample_task (producer)       -> generates a simulated ECG sample every
 *                                       50 ms (20 Hz decimated sample rate),
 *                                       pushes it into data_q.
 *   arrhythmia_decision_task (consumer) -> pulls samples from data_q, flags
 *                                       any sample above ARRHYTHMIA_THRESHOLD_MV
 *                                       as an arrhythmia event.
 *   cycle_coordinator_task            -> rendezvous: waits for BOTH
 *                                       "sample produced" and "sample
 *                                       processed" bits, then notifies the
 *                                       alert task that a full cycle finished.
 *   alert_task (responder)            -> wakes on direct task notification
 *                                       (from the coordinator OR the manual
 *                                       patient-alert button ISR) and logs a
 *                                       routine cycle-complete message or an
 *                                       arrhythmia alert, depending on the
 *                                       last sample's status.
 *
 * All four Core-1 tasks are timed with the MEASURE_WCET macro folded in
 * from App 2's Medical Pulse Monitor, producing the mean/max/WCET+30%
 * evidence table shown in both monitors.
 *
 * ============================================================
 *  RUN MODE  (serial monitor vs. web monitor)
 * ============================================================
 * USE_WEBSERVER selects the Core-0 observability plane. The Core-1 pipeline is
 * identical in both modes.
 *
 *   USE_WEBSERVER = 0  -> Serial monitor (provided, working). Prints queue depth,
 *                         event bits, and heartbeats once a second. No Wi-Fi, so
 *                         the pipeline runs in Wokwi out of the box.
 *   USE_WEBSERVER = 1  -> Web monitor. Runs webmonitor_task, which renders the
 *                         same fields over HTTP (Wi-Fi STA + a single-page HTTP
 *                         handler, adapted from App 1's pattern).
 *
 * Start on USE_WEBSERVER=0 to get the pipeline moving in the simulator, then flip
 * to 1 once you have real Wi-Fi credentials to test against.
 * ============================================================
 */

#ifndef USE_WEBSERVER
#define USE_WEBSERVER 0
#endif

#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"

#if USE_WEBSERVER
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#endif

#define BUTTON_GPIO GPIO_NUM_18

/* ---------- Status LEDs ----------
 * BLUE_LED_GPIO toggles every time the WCET evidence table is printed (once
 * a second in the serial monitor, and once per page load in the web
 * monitor) — a simple visual heartbeat that the monitor is alive and
 * reporting. RED_LED_GPIO toggles every time alert_task raises an actual
 * arrhythmia alert (not routine cycle ticks) — a hardware indicator you can
 * point a camera at for the demo video, distinct from the serial log. */
#define BLUE_LED_GPIO GPIO_NUM_2
#define RED_LED_GPIO  GPIO_NUM_4

#define CONFIG_LOG_DEFAULT_LEVEL_INFO 1
#define CONFIG_LOG_MAXIMUM_LEVEL  5

static const char *TAG = "medguard_a5";

/* ---------- Themed data item ----------
 * A single decimated ECG sample. timestamp_ms lets the consumer/monitor show
 * how stale a sample is; ecg_mv is a simulated millivolt reading. */
typedef struct {
    uint32_t timestamp_ms;
    int      ecg_mv;
} ecg_sample_t;

/* Anything above this simulated reading is treated as an arrhythmia event.
 * This is a stand-in for a real QRS/R-R interval analysis algorithm — the
 * IPC pipeline structure is the point of this assignment, not the clinical
 * algorithm. */
#define ARRHYTHMIA_THRESHOLD_MV 150

/* ---------- IPC objects (created in app_main, used everywhere) ---------- */
static QueueHandle_t      data_q;        /* holds ecg_sample_t items — see queue sizing in README */
static EventGroupHandle_t evt_group;
static TaskHandle_t       responder_handle;

/* Event-group bit definitions */
#define EV_BIT_DATA_PRODUCED  (1 << 0)
#define EV_BIT_DATA_PROCESSED (1 << 1)

/* Set by the consumer after evaluating the most recent sample; read by the
 * alert task to decide whether a given wake-up is a routine cycle-complete
 * tick or an actual arrhythmia alert. Single bool reads/writes are atomic on
 * Xtensa, matching the same reasoning already used for the heartbeat counters. */
static volatile bool last_sample_arrhythmia = false;

/* Most recent item to pass through the queue, for the monitor's
 * "current depth + last item" requirement. Struct writes here are two
 * separate stores (timestamp_ms, then ecg_mv), so this is only ever read by
 * the monitor tasks for display purposes — not used for any control-flow
 * decision, where a torn read would matter (that's App 6's topic). */
static volatile ecg_sample_t last_item;

/* ---------- Latency benchmark (ported from App 3) ----------
 * Fires a binary semaphore AND a direct task notification from the SAME
 * ISR instant, so the two wake-up paths can be measured head-to-head under
 * identical conditions. This is a direct port of App 3's button_isr /
 * btn_task_sem / btn_task_notif pattern (cited in README) — kept as a
 * separate dedicated benchmark rather than folded into alert_task, so the
 * measurement doesn't interfere with (or get confused with) the real
 * pipeline's coordinator-triggered alert path. */
static SemaphoreHandle_t bench_sem;
static TaskHandle_t      bench_notif_handle;
static volatile int64_t  bench_isr_entry_us;
static volatile uint64_t bench_latency_max_sem_us;
static volatile uint64_t bench_latency_max_notif_us;

/* Per-task heartbeats — proof of life for the monitor. Single 32-bit reads are
 * atomic on Xtensa, so the monitor can read these without a lock (App 6's topic). */
static volatile uint32_t hb_prod, hb_cons, hb_coord, hb_resp;

/* ---------- WCET measurement infrastructure (folded in from App 2) ----------
 * Same MEASURE_WCET macro pattern as App 2's Medical Pulse Monitor: times a
 * task body, tracks the running max (worst observed so far) and a running
 * mean, and the monitor reports max + 30% as a simple WCET estimate. This
 * directly produces the "Task table + WCET evidence" required for the
 * capstone portfolio site. */
#define MEASURE_WCET(_max_var, _total_var, _count_var, _body) do { \
    int64_t _t0 = esp_timer_get_time();                            \
    _body;                                                         \
    int64_t _dt = esp_timer_get_time() - _t0;                      \
    if ((uint64_t)_dt > (_max_var)) (_max_var) = (uint64_t)_dt;    \
    (_total_var) += (uint64_t)_dt;                                 \
    (_count_var)++;                                                \
} while (0)

static volatile uint64_t wcet_ecg_max_us,   wcet_ecg_total_us;
static volatile uint32_t wcet_ecg_count;
static volatile uint64_t wcet_arrh_max_us,  wcet_arrh_total_us;
static volatile uint32_t wcet_arrh_count;
static volatile uint64_t wcet_coord_max_us, wcet_coord_total_us;
static volatile uint32_t wcet_coord_count;
static volatile uint64_t wcet_alert_max_us, wcet_alert_total_us;
static volatile uint32_t wcet_alert_count;

/* Counts samples dropped due to back-pressure — surfaced in both monitors so
 * the drop-oldest policy (see README) is directly observable, not just implied. */
static volatile uint32_t dropped_samples = 0;

/* ---------- Simple simulated ECG waveform ----------
 * A low, slowly-varying baseline with an occasional out-of-range spike every
 * 47 ticks, so the arrhythmia path is regularly exercised without needing
 * real sensor hardware in the simulator. */
static int simulate_ecg_reading(int tick)
{
    if (tick % 47 == 0) {
        return 180; /* simulated arrhythmia spike, mV out of normal range */
    }
    return 10 + (tick % 10);
}

/* ---------- Producer task (Core 1): ecg_sample_task ----------
 * Generates a themed ECG sample every 50 ms (20 Hz) and pushes it into data_q. */
static void ecg_sample_task(void *arg)
{
    int tick = 0;
    for (;;) {
        MEASURE_WCET(wcet_ecg_max_us, wcet_ecg_total_us, wcet_ecg_count, {
            /* NOTE: built field-by-field (not as one `{ .a = x, .b = y }`
             * brace initializer) because the C preprocessor only tracks ()
             * nesting when splitting macro arguments, not {}. A top-level
             * comma inside a brace initializer here would get misread as
             * separating MEASURE_WCET's own arguments and fail to compile. */
            ecg_sample_t sample;
            sample.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
            sample.ecg_mv       = simulate_ecg_reading(tick);

            if (xQueueSend(data_q, &sample, pdMS_TO_TICKS(5)) != pdTRUE) {
                /* Back-pressure policy: drop OLDEST, keep NEWEST.
                 * In patient monitoring, a stale ECG sample sitting behind a
                 * backlog is less clinically useful than the current reading —
                 * so when the queue is full we discard the oldest queued sample
                 * to make room for the one we just captured, rather than
                 * blocking the producer (which would stall real-time sampling)
                 * or silently dropping the new sample (which would hide the
                 * most current, most relevant data). See README for the
                 * worst-case burst math behind this choice. */
                ecg_sample_t discard;
                if (xQueueReceive(data_q, &discard, 0) == pdTRUE) {
                    xQueueSend(data_q, &sample, 0);
                }
                dropped_samples++;
                ESP_LOGW(TAG, "[ecg_sample] queue full - dropped oldest sample (total dropped=%lu)",
                         (unsigned long)dropped_samples);
            }

            xEventGroupSetBits(evt_group, EV_BIT_DATA_PRODUCED);
        });

        tick++;
        hb_prod++;
        vTaskDelay(pdMS_TO_TICKS(50));   /* 20 Hz producer (decimated ECG sample) */
    }
}

/* ---------- Consumer task (Core 1): arrhythmia_decision_task ----------
 * Pulls samples from data_q and flags out-of-range readings as arrhythmia
 * events. */
static void arrhythmia_decision_task(void *arg)
{
    ecg_sample_t sample;
    for (;;) {
        if (xQueueReceive(data_q, &sample, pdMS_TO_TICKS(200)) == pdTRUE) {
            bool arrhythmia = (sample.ecg_mv > ARRHYTHMIA_THRESHOLD_MV);
            last_sample_arrhythmia = arrhythmia;
            last_item.timestamp_ms = sample.timestamp_ms;
            last_item.ecg_mv       = sample.ecg_mv;

            MEASURE_WCET(wcet_arrh_max_us, wcet_arrh_total_us, wcet_arrh_count, {
                if (arrhythmia) {
                    ESP_LOGW(TAG, "[arrhythmia_decision] sample @ %lums = %d mV - ARRHYTHMIA DETECTED",
                             (unsigned long)sample.timestamp_ms, sample.ecg_mv);
                } else {
                    ESP_LOGI(TAG, "[arrhythmia_decision] sample @ %lums = %d mV - normal",
                             (unsigned long)sample.timestamp_ms, sample.ecg_mv);
                }
            });

            xEventGroupSetBits(evt_group, EV_BIT_DATA_PROCESSED);
            hb_cons++;
        }
    }
}

/* ---------- Coordinator task (Core 1): cycle_coordinator_task ----------
 * Waits for BOTH event bits to be set (one full produce+decide cycle for a
 * single ECG sample), then signals the alert task via direct task
 * notification that a cycle has completed. */
static void cycle_coordinator_task(void *arg)
{
    const EventBits_t wait_mask = EV_BIT_DATA_PRODUCED | EV_BIT_DATA_PROCESSED;
    for (;;) {
        EventBits_t got = xEventGroupWaitBits(evt_group, wait_mask,
                                              pdTRUE,   /* clear on exit */
                                              pdTRUE,   /* wait for ALL */
                                              portMAX_DELAY);
        if ((got & wait_mask) == wait_mask) {
            MEASURE_WCET(wcet_coord_max_us, wcet_coord_total_us, wcet_coord_count, {
                /* "Cycle complete" for this theme means: one ECG sample has
                 * been captured AND evaluated for arrhythmia. That's the unit
                 * of work the alert task cares about, so we notify it every
                 * cycle; the alert task itself decides (via
                 * last_sample_arrhythmia) whether that means a routine tick
                 * or an actual alert. */
                xTaskNotifyGive(responder_handle);
            });
            hb_coord++;
        }
    }
}

/* ---------- Responder task (Core 1): alert_task ----------
 * Wakes via direct task notification from the coordinator (every completed
 * cycle) OR from the manual patient-alert button ISR. Logs a routine
 * cycle-complete tick or an arrhythmia alert depending on the most recent
 * sample's status.
 *
 * Note: because both the coordinator and the button ISR notify the same
 * task the same way (a plain increment-and-take), this design can't
 * distinguish "coordinator woke me" from "the button woke me" purely from
 * the notification itself — it only knows how many notifications are
 * pending. That's an accepted limitation for this assignment's scope; a
 * fuller design could use xTaskNotify() with eSetValueWithoutOverwrite to
 * encode a reason code instead of the simple give/take pair used here. */
static void alert_task(void *arg)
{
    for (;;) {
        uint32_t n = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (n == 0) continue;

        bool this_was_an_alarm = false;

        MEASURE_WCET(wcet_alert_max_us, wcet_alert_total_us, wcet_alert_count, {
            if (last_sample_arrhythmia) {
                ESP_LOGE(TAG, "[alert] *** ARRHYTHMIA ALERT *** (notify count=%lu)", (unsigned long)n);
                this_was_an_alarm = true;
            } else {
                ESP_LOGI(TAG, "[alert] routine cycle acknowledged (notify count=%lu)", (unsigned long)n);
            }
        });
        hb_resp++;

        /* Red LED pulse deliberately kept OUTSIDE the MEASURE_WCET block above —
         * a blocking delay in there would inflate the reported WCET for
         * alert_task and make that evidence meaningless. A pulse (not a
         * toggle) so every real alarm gets exactly one visible flash. */
        if (this_was_an_alarm) {
            gpio_set_level(RED_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(RED_LED_GPIO, 0);
        }
    }
}

/* ---------- Benchmark tasks (ported from App 3) ----------
 * Both woken from the exact same ISR instant (bench_isr_entry_us), so the
 * printed latencies are a fair, head-to-head comparison of the two wake-up
 * mechanisms under identical conditions — same trigger, same priority (12,
 * matching alert_task), same core. */
static void bench_task_sem(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(bench_sem, portMAX_DELAY) == pdTRUE) {
            int64_t lat = esp_timer_get_time() - bench_isr_entry_us;
            if ((uint64_t)lat > bench_latency_max_sem_us) bench_latency_max_sem_us = (uint64_t)lat;
            ESP_LOGI(TAG, "[bench-sem] wake latency=%lld us (max=%llu)",
                     (long long)lat, (unsigned long long)bench_latency_max_sem_us);
        }
    }
}

static void bench_task_notif(void *arg)
{
    for (;;) {
        uint32_t count = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (count == 0) continue;
        int64_t lat = esp_timer_get_time() - bench_isr_entry_us;
        if ((uint64_t)lat > bench_latency_max_notif_us) bench_latency_max_notif_us = (uint64_t)lat;
        ESP_LOGI(TAG, "[bench-notif] wake latency=%lld us (max=%llu)",
                 (long long)lat, (unsigned long long)bench_latency_max_notif_us);
    }
}

/* ---------- Button ISR — manual patient-alert button ----------
 * Models a bedside "patient call" button wired directly to the alert task,
 * bypassing the queue/event-group pipeline entirely — the fastest possible
 * path for a manually-triggered alert. Also fires the App-3-ported latency
 * benchmark (bench_sem + bench_notif_handle) from the same instant. */
static volatile int64_t last_edge_us;
static void IRAM_ATTR button_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - last_edge_us < 200) return;
    last_edge_us = now;

    bench_isr_entry_us = now;   /* benchmark trigger instant, ported from App 3 */

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(responder_handle, &woken);     /* real pipeline: patient alert */
    xSemaphoreGiveFromISR(bench_sem, &woken);              /* benchmark: semaphore path */
    vTaskNotifyGiveFromISR(bench_notif_handle, &woken);    /* benchmark: notification path */
    portYIELD_FROM_ISR(woken);
}

#if USE_WEBSERVER
/* ---------- Web monitor task (Core 0)  [USE_WEBSERVER = 1] ----------
 * Wi-Fi STA connect + a single-page HTTP handler, adapted from App 1's
 * wifi_init_sta() / handle_root() / start_webserver() pattern (cited in
 * README). Renders the same fields the serial monitor prints: queue depth,
 * event-group bits, per-task heartbeats, dropped-sample count, and the most
 * recent sample's arrhythmia status.
 *
 * Set WIFI_SSID / WIFI_PASS to your network before building with
 * USE_WEBSERVER=1 — these are placeholders and will not connect as-is. */
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"

static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    /* Block until we actually have an IP so the HTTP server never starts
     * against a dead interface. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    for (;;) {
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ip_info.ip));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static esp_err_t handle_root(httpd_req_t *req)
{
    char buf[1600];
    UBaseType_t depth = uxQueueMessagesWaiting(data_q);
    EventBits_t bits  = xEventGroupGetBits(evt_group);

    uint64_t ecg_mean   = wcet_ecg_count   ? wcet_ecg_total_us   / wcet_ecg_count   : 0;
    uint64_t arrh_mean  = wcet_arrh_count  ? wcet_arrh_total_us  / wcet_arrh_count  : 0;
    uint64_t coord_mean = wcet_coord_count ? wcet_coord_total_us / wcet_coord_count : 0;
    uint64_t alert_mean = wcet_alert_count ? wcet_alert_total_us / wcet_alert_count : 0;

    snprintf(buf, sizeof(buf),
        "<html><head><meta http-equiv=\"refresh\" content=\"1\">"
        "<title>MedGuard ECG Pipeline Monitor</title></head><body>"
        "<h2>MedGuard ECG Pipeline Monitor</h2>"
        "<p><b>Queue depth:</b> %u</p>"
        "<p><b>Last item:</b> timestamp=%lu ms, ecg_mv=%d</p>"
        "<p><b>Event bits:</b> 0x%02x &nbsp; (DATA_PRODUCED=%d, DATA_PROCESSED=%d)</p>"
        "<p><b>Last sample status:</b> %s</p>"
        "<p><b>Dropped samples (back-pressure):</b> %lu</p>"
        "<p><b>Heartbeats</b> - ecg_sample=%lu &nbsp; arrhythmia_decision=%lu &nbsp; "
        "coordinator=%lu &nbsp; alert=%lu</p>"
        "<h3>WCET Evidence</h3>"
        "<table border=\"1\" cellpadding=\"4\"><tr><th>Task</th><th>Mean (us)</th>"
        "<th>Max (us)</th><th>WCET+30%% (us)</th></tr>"
        "<tr><td>ecg_sample</td><td>%llu</td><td>%llu</td><td>%llu</td></tr>"
        "<tr><td>arrhythmia_decision</td><td>%llu</td><td>%llu</td><td>%llu</td></tr>"
        "<tr><td>cycle_coordinator</td><td>%llu</td><td>%llu</td><td>%llu</td></tr>"
        "<tr><td>alert</td><td>%llu</td><td>%llu</td><td>%llu</td></tr>"
        "</table>"
        "</body></html>",
        (unsigned)depth,
        (unsigned long)last_item.timestamp_ms, last_item.ecg_mv,
        (unsigned)bits,
        (bits & EV_BIT_DATA_PRODUCED)  ? 1 : 0,
        (bits & EV_BIT_DATA_PROCESSED) ? 1 : 0,
        last_sample_arrhythmia ? "ARRHYTHMIA" : "normal",
        (unsigned long)dropped_samples,
        (unsigned long)hb_prod, (unsigned long)hb_cons,
        (unsigned long)hb_coord, (unsigned long)hb_resp,
        (unsigned long long)ecg_mean,   (unsigned long long)wcet_ecg_max_us,   (unsigned long long)(wcet_ecg_max_us * 13 / 10),
        (unsigned long long)arrh_mean,  (unsigned long long)wcet_arrh_max_us,  (unsigned long long)(wcet_arrh_max_us * 13 / 10),
        (unsigned long long)coord_mean, (unsigned long long)wcet_coord_max_us, (unsigned long long)(wcet_coord_max_us * 13 / 10),
        (unsigned long long)alert_mean, (unsigned long long)wcet_alert_max_us, (unsigned long long)(wcet_alert_max_us * 13 / 10));

    /* Brief pulse, not a toggle, so every page load flashes once — see the
     * comment in serial_monitor_task for why a toggle looks mismatched.
     * Kept short (20ms) since this runs inside the HTTP request handler and
     * shouldn't meaningfully delay the response. */
    gpio_set_level(BLUE_LED_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BLUE_LED_GPIO, 0);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);
    }
    return server;
}

static void webmonitor_task(void *arg)
{
    wifi_init_sta();
    start_webserver();
    ESP_LOGI(TAG, "[webmon] web monitor running - open the printed IP in a browser");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
#else
/* ---------- Serial monitor task (Core 0)  [USE_WEBSERVER = 0] ----------
 * Provided and working. Prints the same state the web monitor shows, so the
 * pipeline is observable in Wokwi with no Wi-Fi. */
static void serial_monitor_task(void *arg)
{
    for (;;) {
        UBaseType_t depth = uxQueueMessagesWaiting(data_q);
        EventBits_t bits  = xEventGroupGetBits(evt_group);
        ESP_LOGI(TAG,
                 "[monitor] q_depth=%u  last_item(ts=%lu,mV=%d)  evt=0x%02x  last=%s  dropped=%lu  "
                 "hb: ecg=%lu arrh=%lu coord=%lu alert=%lu",
                 (unsigned)depth,
                 (unsigned long)last_item.timestamp_ms, last_item.ecg_mv,
                 (unsigned)bits,
                 last_sample_arrhythmia ? "ARRHYTHMIA" : "normal",
                 (unsigned long)dropped_samples,
                 (unsigned long)hb_prod, (unsigned long)hb_cons,
                 (unsigned long)hb_coord, (unsigned long)hb_resp);

        /* WCET evidence table (folded in from App 2's Medical Pulse Monitor
         * format) — required for the capstone portfolio site. */
        uint64_t ecg_mean   = wcet_ecg_count   ? wcet_ecg_total_us   / wcet_ecg_count   : 0;
        uint64_t arrh_mean  = wcet_arrh_count  ? wcet_arrh_total_us  / wcet_arrh_count  : 0;
        uint64_t coord_mean = wcet_coord_count ? wcet_coord_total_us / wcet_coord_count : 0;
        uint64_t alert_mean = wcet_alert_count ? wcet_alert_total_us / wcet_alert_count : 0;

        printf("--- WCET evidence (Task, Mean us, Max us, WCET+30%% us) ---\n");
        printf("%-20s %-10llu %-10llu %-10llu\n", "ecg_sample",
               (unsigned long long)ecg_mean, (unsigned long long)wcet_ecg_max_us,
               (unsigned long long)(wcet_ecg_max_us * 13 / 10));
        printf("%-20s %-10llu %-10llu %-10llu\n", "arrhythmia_decision",
               (unsigned long long)arrh_mean, (unsigned long long)wcet_arrh_max_us,
               (unsigned long long)(wcet_arrh_max_us * 13 / 10));
        printf("%-20s %-10llu %-10llu %-10llu\n", "cycle_coordinator",
               (unsigned long long)coord_mean, (unsigned long long)wcet_coord_max_us,
               (unsigned long long)(wcet_coord_max_us * 13 / 10));
        printf("%-20s %-10llu %-10llu %-10llu\n", "alert",
               (unsigned long long)alert_mean, (unsigned long long)wcet_alert_max_us,
               (unsigned long long)(wcet_alert_max_us * 13 / 10));

        /* Explicit pulse (on, brief hold, off) rather than a state toggle —
         * a toggle only turns the LED ON every OTHER print (the alternating
         * prints turn it OFF), which looks laggy/mismatched against the
         * WCET table appearing every single cycle. A pulse gives exactly
         * one visible flash per print, always. */
        gpio_set_level(BLUE_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(BLUE_LED_GPIO, 0);

        vTaskDelay(pdMS_TO_TICKS(900));
    }
}
#endif /* USE_WEBSERVER */

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "==== MedGuard Final Capstone [medical device embedded firmware] starting - ECG IPC pipeline ====");

#if USE_WEBSERVER
    ESP_LOGI(TAG, "Monitor: WEB (USE_WEBSERVER=1)");
#else
    ESP_LOGI(TAG, "Monitor: SERIAL (USE_WEBSERVER=0) - Core-0 summary once/sec, no Wi-Fi");
#endif

    /* Queue sizing: depth 8, item size sizeof(ecg_sample_t).
     * Producer runs at 20 Hz (one sample every 50 ms). If the consumer
     * stalls for up to ~200 ms (its own receive timeout), the worst-case
     * backlog is 200ms / 50ms = 4 samples. Depth 8 gives a 2x safety margin
     * above that worst-case burst before the drop-oldest back-pressure
     * policy engages. Full math and justification in README. */
    data_q = xQueueCreate(/*depth=*/ 8, /*item size=*/ sizeof(ecg_sample_t));

    evt_group = xEventGroupCreate();

    /* Tasks on Core 1 (real-time plane). 4096-byte stacks: any task that calls
     * ESP_LOGI needs headroom for the vprintf formatting (2048 overflows). */
    xTaskCreatePinnedToCore(ecg_sample_task,        "ecg_sample", 4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(arrhythmia_decision_task,"arrhythmia",4096, NULL,  8, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(cycle_coordinator_task,  "coord",     4096, NULL,  9, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(alert_task,              "alert",     4096, NULL, 12, &responder_handle, APP_CPU_NUM);

    /* Latency benchmark tasks (ported from App 3) — same priority (12) as
     * alert_task so the comparison isn't skewed by priority differences. */
    bench_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(bench_task_sem,   "bench_sem",   4096, NULL, 12, NULL, APP_CPU_NUM);
    xTaskCreatePinnedToCore(bench_task_notif, "bench_notif", 4096, NULL, 12, &bench_notif_handle, APP_CPU_NUM);

    /* Observability plane on Core 0 (networking plane) — see README Q1 for
     * why the monitor is pinned here rather than Core 1. */
#if USE_WEBSERVER
    xTaskCreatePinnedToCore(webmonitor_task,    "webmon",  4096, NULL, 4, NULL, PRO_CPU_NUM);
#else
    xTaskCreatePinnedToCore(serial_monitor_task, "monitor", 4096, NULL, 4, NULL, PRO_CPU_NUM);
#endif

    /* Status LEDs — blue (WCET evidence heartbeat) and red (arrhythmia alert) */
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << BLUE_LED_GPIO) | (1ULL << RED_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);
    gpio_set_level(BLUE_LED_GPIO, 0);
    gpio_set_level(RED_LED_GPIO, 0);

    /* Manual patient-alert button ISR */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr, NULL);
}

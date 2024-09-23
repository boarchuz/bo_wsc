#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/lock.h>

#include "nvs_flash.h"
#include "esp_attr.h"
#include "esp_log.h"
#if __has_include("esp_idf_version.h")
#   include "esp_idf_version.h"
#endif
#if __has_include("esp_private/wifi_os_adapter.h")
#   include "esp_private/wifi_os_adapter.h"
#endif

#include "bo_wsc.h"

#include "sdkconfig.h"

static const char *TAG = "bo_wsc";

#ifndef ARRAY_SIZE
#   define ARRAY_SIZE(x) ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
#endif

#ifdef CONFIG_BO_WSC_RTC_MEM_SLOW
    #ifdef CONFIG_ESP32_RTCDATA_IN_FAST_MEM
        #define BO_WSC_RTC_BSS_ATTR     RTC_SLOW_ATTR
    #else
        #define BO_WSC_RTC_BSS_ATTR     __attribute__((section(".rtc.bss")))
    #endif
#else
    #ifdef CONFIG_ESP32_RTCDATA_IN_FAST_MEM
        #define BO_WSC_RTC_BSS_ATTR     __attribute__((section(".rtc.bss")))
    #else
        #define BO_WSC_RTC_BSS_ATTR     RTC_FAST_ATTR
    #endif
#endif

#if defined(CONFIG_BO_WSC_NVS_DISABLED) || defined(CONFIG_BO_WSC_LOCK_DISABLED)
#define _bo_wsc_lock()
#define _bo_wsc_release()
#else
static _lock_t s_lock;
#define _bo_wsc_lock() _lock_acquire(&s_lock)
#define _bo_wsc_release() _lock_release(&s_lock);
#endif

typedef struct {
    uint16_t valid : 1;
    #ifndef CONFIG_BO_WSC_NVS_DISABLED
    uint16_t dirty : 1;
    #endif
    uint16_t size : 10;
} bo_wsc_nvs_metadata_t;
_Static_assert(sizeof(bo_wsc_nvs_metadata_t) == sizeof(uint16_t), "");

/* NVS namespaces */
#define BO_WSC_NVS_NAMESPACE_LIST \
    X(misc,         "misc") \
    X(nvs_net80211, "nvs.net80211")

static const char *bo_wsc_nvs_namespace_names[] = {
    #define X(_name, _ns) \
        _ns,
    BO_WSC_NVS_NAMESPACE_LIST
    #undef X
};

typedef union {
    struct {
        #define X(_name, _ns) \
            nvs_handle_t _name;
        BO_WSC_NVS_NAMESPACE_LIST
        #undef X
    } by_name;
    nvs_handle_t by_index[0
        #define X(_name, _ns) \
            +1
        BO_WSC_NVS_NAMESPACE_LIST
        #undef X
    ];
} bo_wsc_nvs_namespaces_t;

static bo_wsc_nvs_namespaces_t s_bo_wsc_nvs;

/* NVS keys */
#define BRACE_INIT(...) {__VA_ARGS__}
/*
    As RTC memory is limited, to reduce memory footprint some of these are disabled if they are not required for fast STA connection. */
/*        Internal ID       NVS Key             Enabled     Size       Namespace          Initial Contents        */
#   define BO_WSC_NVS_KEY_LIST1 \
        X(log,              "log",              0,          4,          misc,             BRACE_INIT(0x03, 0x00, 0x01, 0x00)) \
        X(opmode,           "opmode",           0,          1,          nvs_net80211,     BRACE_INIT(0x02)) \
        X(sta_ssid,         "sta.ssid",         1,          36,         nvs_net80211,     BRACE_INIT([0 ... 35] = 0xFF)) \
        X(sta_authmode,     "sta.authmode",     0,          1,          nvs_net80211,     BRACE_INIT(0x01)) \
        X(sta_pswd,         "sta.pswd",         0,          65,         nvs_net80211,     BRACE_INIT([0 ... 64] = 0xFF)) \
        X(sta_pmk,          "sta.pmk",          1,          32,         nvs_net80211,     BRACE_INIT([0 ... 31] = 0xFF)) \
        X(sta_chan,         "sta.chan",         0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(auto_conn,        "auto.conn",        0,          1,          nvs_net80211,     BRACE_INIT(0x01)) \
        X(bssid_set,        "bssid.set",        1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_bssid,        "sta.bssid",        1,          6,          nvs_net80211,     BRACE_INIT([0 ... 5] = 0xFF)) \
        X(sta_lis_intval,   "sta.lis_intval",   0,          2,          nvs_net80211,     BRACE_INIT(0x03, 0x00)) \
        X(sta_phym,         "sta.phym",         0,          1,          nvs_net80211,     BRACE_INIT(0x03)) \
        X(sta_phybw,        "sta.phybw",        0,          1,          nvs_net80211,     BRACE_INIT(0x02)) \
        X(sta_apsw,         "sta.apsw",         0,          2,          nvs_net80211,     BRACE_INIT([0 ... 1] = 0xFF)) \
        X(sta_apinfo,       "sta.apinfo",       1,          700,        nvs_net80211,     BRACE_INIT([0 ... 699] = 0xFF)) \
        X(sta_scan_method,  "sta.scan_method",  0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_sort_method,  "sta.sort_method",  0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_minrssi,      "sta.minrssi",      0,          1,          nvs_net80211,     BRACE_INIT(0x81)) \
        X(sta_minauth,      "sta.minauth",      1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_pmf_e,        "sta.pmf_e",        0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_pmf_r,        "sta.pmf_r",        0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_btm_e,        "sta.btm_e",        0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_mbo_e,        "sta.mbo_e",        0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_rrm_e,        "sta.rrm_e",        0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(ap_ssid,          "ap.ssid",          0,          36,         nvs_net80211,     BRACE_INIT([0 ... 35] = 0xFF)) \
        X(ap_passwd,        "ap.passwd",        0,          65,         nvs_net80211,     BRACE_INIT([0 ... 64] = 0xFF)) \
        X(ap_pmk,           "ap.pmk",           0,          32,         nvs_net80211,     BRACE_INIT([0 ... 31] = 0xFF)) \
        X(ap_chan,          "ap.chan",          0,          1,          nvs_net80211,     BRACE_INIT(0x01)) \
        X(ap_authmode,      "ap.authmode",      0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(ap_hidden,        "ap.hidden",        0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(ap_max_conn,      "ap.max.conn",      0,          1,          nvs_net80211,     BRACE_INIT(0x04)) \
        X(bcn_interval,     "bcn.interval",     0,          2,          nvs_net80211,     BRACE_INIT(0x64, 0x00)) \
        X(ap_phym,          "ap.phym",          0,          1,          nvs_net80211,     BRACE_INIT(0x03)) \
        X(ap_phybw,         "ap.phybw",         0,          1,          nvs_net80211,     BRACE_INIT(0x02)) \
        X(ap_sndchan,       "ap.sndchan",       0,          1,          nvs_net80211,     BRACE_INIT(0x01)) \
        X(ap_pmf_e,         "ap.pmf_e",         0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(ap_pmf_r,         "ap.pmf_r",         0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(ap_p_cipher,      "ap.p_cipher",      0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(ap_ftm_r,         "ap.ftm_r",         0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(ap_sae_h2e,       "ap.sae_h2e",       0,          1,          nvs_net80211,     BRACE_INIT(0x04)) \
        X(ap_pmk_info,      "ap.pmk_info",      0,          132,        nvs_net80211,     BRACE_INIT(0x04, 0x00, 0x00, 0x00, 0x43, 0x4c, 0x69, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff)) \
        X(lorate,           "lorate",           0,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(country,          "country",          0,          12,         nvs_net80211,     BRACE_INIT([0 ... 11] = 0xFF))

#if defined(ESP_IDF_VERSION) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,1,0)
    #define BO_WSC_NVS_KEY_LIST2 \
        X(sta_ft,           "sta.ft",           1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_owe,          "sta.owe",          1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_trans_d,      "sta.trans_d",      1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_sae_h2e,      "sta.sae_h2e",      1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_sae_pk_mode,  "sta.sae_pk_mode",  1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_bss_retry,    "sta.bss_retry",    1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_owe_data,     "sta.owe_data",     1,          44,         nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_dcm,       "sta.he_dcm",       1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_dcm_tx,    "sta.he_dcm_c_tx",  1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_dcm_c_rx,  "sta.he_dcm_c_rx",  1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_mcs9_d,    "sta.he_mcs9_d",    1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_su_b_d,    "sta.he_su_b_d",    1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_su_b_f_d,  "sta.he_su_b_f_d",  1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_mu_b_f_d,  "sta.he_mu_b_f_d",  1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_he_cqi_f_d,   "sta.he_cqi_f_d",   1,          1,          nvs_net80211,     BRACE_INIT(0x00)) \
        X(sta_sae_h2e_id,   "sta.sae_h2e_id",   1,          32,         nvs_net80211,     BRACE_INIT(0x00)) \
        X(nan_phym,         "nan.phym",         0,          1,          nvs_net80211,     BRACE_INIT(0x00))
#else
#   define BO_WSC_NVS_KEY_LIST2
#endif

#define BO_WSC_NVS_KEY_LIST \
    BO_WSC_NVS_KEY_LIST1 \
    BO_WSC_NVS_KEY_LIST2

typedef struct {
    #ifndef CONFIG_BO_WSC_NVS_DISABLED
    const size_t namespace_index;
    #endif
    const char *key;
    const size_t offset;
    const size_t max_size;
} bo_wsc_nvs_desc_t;

BO_WSC_RTC_BSS_ATTR static union {
    struct {
        #define X(_name, _key, _en, _size, _ns, _default) \
            uint8_t _name[_en ? _size : 0];
        BO_WSC_NVS_KEY_LIST
        #undef X
    };
    uint8_t bytes[0 +
        #define X(_name, _key, _en, _size, _ns, _default) \
            + (_en ? _size : 0)
        BO_WSC_NVS_KEY_LIST
        #undef X
    ];
} bo_ws_nvs_cache;

#define X(_name, _key, _en, _size, _ns, _default) \
    + (_en ? _size : 0)
_Static_assert(sizeof(bo_ws_nvs_cache) == 0 BO_WSC_NVS_KEY_LIST, "");
#undef X

BO_WSC_RTC_BSS_ATTR static bo_wsc_nvs_metadata_t bo_ws_nvs_metadata[0
    #define X(_name, _key, _en, _size, _ns, _default) \
    + (_en ? 1 : 0)
    BO_WSC_NVS_KEY_LIST
    #undef X
];

static const bo_wsc_nvs_desc_t bo_ws_nvs_desc[] = {
    #ifndef CONFIG_BO_WSC_NVS_DISABLED
        #define X(_name, _key, _en, _size, _ns, _default) \
        { \
            .namespace_index = (offsetof(bo_wsc_nvs_namespaces_t, by_name._ns) / sizeof(((bo_wsc_nvs_namespaces_t*)0)->by_name._ns)), \
            .key = _key, \
            .offset = offsetof(typeof(bo_ws_nvs_cache), _name), \
            .max_size = _size, \
        },
    #else
        #define X(_name, _key, _en, _size, _ns, _default) \
        { \
            .key = _key, \
            .offset = offsetof(typeof(bo_ws_nvs_cache), _name), \
            .max_size = (_en ? _size : 0), \
        },
    #endif
    BO_WSC_NVS_KEY_LIST
    #undef X
};

static void __attribute__((constructor, section(("/DISCARD/")))) bo_wsc_check_metadata_size_bitwidth_adequate(void)
{
    #define X(_name, _key, _en, _size, _ns, _default) \
        if(_size > (bo_wsc_nvs_metadata_t){.size = -1}.size) \
        { \
            asm("bo_wsc_nvs_metadata_t_size_bitwidth_too_low_for_" # _name); \
        }
        BO_WSC_NVS_KEY_LIST
    #undef X
}

static ssize_t key_to_loc(const char *key)
{
    for(ssize_t i = 0; i < ARRAY_SIZE(bo_ws_nvs_desc); ++i) {
        if(strcmp(bo_ws_nvs_desc[i].key, key) == 0)
        {
            return i;
        }
    }
    ESP_LOGE(TAG, "%s [%s] not found", __func__, key);
    return -1;
}

#ifndef CONFIG_BO_WSC_NVS_DISABLED
static bo_wsc_nvs_mode_t s_nvs_mode;

static esp_err_t bo_wsc_nvs_ensure_namespace_open(size_t index)
{
    if(!s_bo_wsc_nvs.by_index[index] != 0) {
        ESP_LOGD(TAG, "[%s] opening \"%s\"", __func__, bo_wsc_nvs_namespace_names[index]);
        esp_err_t err = nvs_flash_init();
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "%s nvs_flash_init err 0x%x", __func__, err);
            return ESP_FAIL;
        }
        err = nvs_open(bo_wsc_nvs_namespace_names[index], NVS_READWRITE, &s_bo_wsc_nvs.by_index[index]);
        if(err != ESP_OK)
        {
            ESP_LOGE(TAG, "[%s] nvs_open [%s] err 0x%x", __func__, bo_wsc_nvs_namespace_names[index], err);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static bool bo_wsc_nvs_key_disabled(const char *key)
{
    #ifdef CONFIG_BO_WSC_OPMODE_NO_NVS
        if(strcmp("opmode", key) == 0)
        {
            return true;
        }
    #endif
    return false;
}

static esp_err_t bo_wsc_do_save(void)
{
    bool ns_dirty[ARRAY_SIZE(s_bo_wsc_nvs.by_index)] = {};
    esp_err_t err;

    // Loop through all items and set/erase to sync up RTC object with NVS, setting ns dirty if anything has changed
    for(int i = 0; i < ARRAY_SIZE(bo_ws_nvs_metadata); ++i) {
        if(bo_ws_nvs_metadata[i].valid && bo_ws_nvs_metadata[i].dirty && !bo_wsc_nvs_key_disabled(bo_ws_nvs_desc[i].key))
        {
            ESP_LOGD(TAG, "%s is dirty (ns:%d)", bo_ws_nvs_desc[i].key, bo_ws_nvs_desc[i].namespace_index);
            bool ns_altered = true;
            err = bo_wsc_nvs_ensure_namespace_open(bo_ws_nvs_desc[i].namespace_index);
            if(err != ESP_OK) {
                return err;
            }

            if(bo_ws_nvs_metadata[i].size > 0) {
                err = nvs_set_blob(s_bo_wsc_nvs.by_index[bo_ws_nvs_desc[i].namespace_index], bo_ws_nvs_desc[i].key, &bo_ws_nvs_cache.bytes[bo_ws_nvs_desc[i].offset], bo_ws_nvs_metadata[i].size);
                if(err != ESP_OK) {
                    ESP_LOGE(TAG, "%s [%s] set_blob err 0x%x", __func__, bo_ws_nvs_desc[i].key, err);
                    return err;
                }
            }
            else {
                err = nvs_erase_key(s_bo_wsc_nvs.by_index[bo_ws_nvs_desc[i].namespace_index], bo_ws_nvs_desc[i].key);
                if(err != ESP_OK) {
                    if(err == ESP_ERR_NVS_NOT_FOUND) {
                        ESP_LOGW(TAG, "%s [%s] already erased", __func__, bo_ws_nvs_desc[i].key);
                        ns_altered = false;
                    }
                    else {
                        ESP_LOGE(TAG, "%s [%s] erase err 0x%x", __func__, bo_ws_nvs_desc[i].key, err);
                        return err;
                    }
                }
            }
            // Set relevant namespace dirty
            if(ns_altered) {
                ns_dirty[bo_ws_nvs_desc[i].namespace_index] = true;
            }
            bo_ws_nvs_metadata[i].dirty = 0;
        }
    }

    // For each namespace, if anything changed then commit
    esp_err_t ret = ESP_OK;
    for(int n = 0; n < ARRAY_SIZE(ns_dirty); ++n) {
        if(ns_dirty[n]) {
            err = nvs_commit(s_bo_wsc_nvs.by_index[n]);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "[%s] commit (%d): 0x%x", __func__, n, err);
                ret = err;
            }
        }
    }
    return ret;
}

esp_err_t bo_wsc_nvs_save(void)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    _bo_wsc_lock();
    if(s_nvs_mode == BO_WSC_NVS_MODE_MANUAL) {
        ret = bo_wsc_do_save();
    }
    _bo_wsc_release();
    return ret;
}

esp_err_t bo_wsc_nvs_mode(bo_wsc_nvs_mode_t mode)
{
    assert(mode == BO_WSC_NVS_MODE_MANUAL || mode == BO_WSC_NVS_MODE_AUTO);

    esp_err_t err = ESP_OK;
    _bo_wsc_lock();
    if(s_nvs_mode != mode) {
        s_nvs_mode = mode;
        if(mode == BO_WSC_NVS_MODE_AUTO)
        {
            err = bo_wsc_do_save();
        }
    }
    _bo_wsc_release();
    return err;
}
#endif // CONFIG_BO_WSC_NVS_DISABLED

static esp_err_t bo_wsc_nvs_do_set(nvs_handle_t handle, const char* key, const void *data, size_t size)
{
    ssize_t i = key_to_loc(key);
    if(i < 0) {
        ESP_LOGE(TAG, "%s unknown key: %s (%u bytes)", __func__, key, size);
        ESP_LOG_BUFFER_HEX_LEVEL(key, data, size, ESP_LOG_ERROR);
        return ESP_ERR_INVALID_ARG;
    }

    if(size > bo_ws_nvs_desc[i].max_size) {
        if(bo_ws_nvs_desc[i].max_size == 0)
        {
            ESP_LOGD(TAG, "skipping %s set", key);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "%s [%s] exceeds buffer size: %u > %u", __func__, key, size, bo_ws_nvs_desc[i].max_size);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGD(TAG, "Setting %s (%u)", key, size);
    if(bo_ws_nvs_metadata[i].valid && bo_ws_nvs_metadata[i].size > 0)
    {
        ESP_LOG_BUFFER_HEX_LEVEL("old", &bo_ws_nvs_cache.bytes[bo_ws_nvs_desc[i].offset], bo_ws_nvs_metadata[i].size, ESP_LOG_DEBUG);
    }
    ESP_LOG_BUFFER_HEX_LEVEL("new", data, size, ESP_LOG_DEBUG);

    _bo_wsc_lock();
    if(
        !bo_ws_nvs_metadata[i].valid ||
        bo_ws_nvs_metadata[i].size != size ||
        memcmp(&bo_ws_nvs_cache.bytes[bo_ws_nvs_desc[i].offset], data, size) != 0
    )
    {
        ESP_LOGD(TAG, "%s [%s] updating", __func__, key);
        memset(&bo_ws_nvs_metadata[i], 0, sizeof(bo_ws_nvs_metadata[i]));
        memcpy(&bo_ws_nvs_cache.bytes[bo_ws_nvs_desc[i].offset], data, size);
        #ifndef CONFIG_BO_WSC_NVS_DISABLED
            bo_ws_nvs_metadata[i].dirty = 1;
        #endif
        bo_ws_nvs_metadata[i].size = size;
        bo_ws_nvs_metadata[i].valid = 1;
    }
    _bo_wsc_release();
    return ESP_OK;
}

static esp_err_t bo_wsc_nvs_do_get(nvs_handle_t handle, const char* key, void *data, size_t *size)
{
    ssize_t i = key_to_loc(key);
    if(i < 0) {
        ESP_LOGE(TAG, "%s unknown key: %s (%u bytes)", __func__, key, *size);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    _bo_wsc_lock();
    #ifndef CONFIG_BO_WSC_NVS_DISABLED
    if(!bo_ws_nvs_metadata[i].valid)
    {
        ret = bo_wsc_nvs_ensure_namespace_open(bo_ws_nvs_desc[i].namespace_index);
        if(ret == ESP_OK)
        {
            memset(&bo_ws_nvs_metadata[i], 0, sizeof(bo_ws_nvs_metadata[i]));
            size_t len = bo_ws_nvs_desc[i].max_size;
            ret = nvs_get_blob(s_bo_wsc_nvs.by_index[bo_ws_nvs_desc[i].namespace_index], bo_ws_nvs_desc[i].key, &bo_ws_nvs_cache.bytes[bo_ws_nvs_desc[i].offset], &len);
            if(ret == ESP_OK) {
                if(len != bo_ws_nvs_desc[i].max_size) {
                    ESP_LOGW(TAG, "[%s] \"%s\" size: %u != %u", __func__, bo_ws_nvs_desc[i].key, len, bo_ws_nvs_desc[i].max_size);
                }
                memcpy(data, &bo_ws_nvs_cache.bytes[bo_ws_nvs_desc[i].offset], len);
                bo_ws_nvs_metadata[i].dirty = 0;
                bo_ws_nvs_metadata[i].size = len;
                bo_ws_nvs_metadata[i].valid = 1;
            }
            else if(ret == ESP_ERR_NVS_NOT_FOUND)
            {
                bo_ws_nvs_metadata[i].dirty = 0;
                bo_ws_nvs_metadata[i].size = 0;
                bo_ws_nvs_metadata[i].valid = 1;
                ret = ESP_OK;
            }
            else
            {
                ESP_LOGE(TAG, "[%s] \"%s\"::\"%s\" err 0x%x", __func__, bo_wsc_nvs_namespace_names[bo_ws_nvs_desc[i].namespace_index], bo_ws_nvs_desc[i].key, ret);
            }
        }
    }
    #endif
    if(ret == ESP_OK)
    {
        if(bo_ws_nvs_desc[i].max_size == 0)
        {
            ESP_LOGD(TAG, "skipping %s get", key);
            ret = ESP_ERR_NVS_NOT_FOUND;
        }
        else
        {
            if(bo_ws_nvs_metadata[i].valid)
            {
                if(bo_ws_nvs_metadata[i].size == 0)
                {
                    ESP_LOGD(TAG, "%s does not exist", key);
                    ret = ESP_ERR_NVS_NOT_FOUND;
                }
                else if(*size < bo_ws_nvs_metadata[i].size)
                {
                    ESP_LOGE(TAG, "%s buffer insufficient for %s: %u < %u", __func__, key, *size, bo_ws_nvs_metadata[i].size);
                    ret = ESP_ERR_INVALID_SIZE;
                }
                else
                {
                    memcpy(data, &bo_ws_nvs_cache.bytes[bo_ws_nvs_desc[i].offset], bo_ws_nvs_metadata[i].size);
                    *size = bo_ws_nvs_metadata[i].size;
                }
            }
        #ifdef CONFIG_BO_WSC_NVS_DISABLED
            else {
                ESP_LOGD(TAG, "[%s] %s has not been set", __func__, key);
                ret = ESP_ERR_NVS_NOT_FOUND;
            }
        #endif
        }
    }
    _bo_wsc_release();
    return ret;
}

static esp_err_t bo_wsc_nvs_do_erase(nvs_handle_t handle, const char* key)
{
    ssize_t i = key_to_loc(key);
    if(i < 0) {
        ESP_LOGE(TAG, "%s unknown key: %s", __func__, key);
        return ESP_ERR_INVALID_ARG;
    }

    _bo_wsc_lock();
#ifndef CONFIG_BO_WSC_NVS_DISABLED
    if(bo_ws_nvs_metadata[i].valid && bo_ws_nvs_metadata[i].size > 0)
    {
        bo_ws_nvs_metadata[i].dirty = 1;
    }
#endif
    bo_ws_nvs_metadata[i].size = 0;
    bo_ws_nvs_metadata[i].valid = 1;
    _bo_wsc_release();
    return ESP_OK;
}

static esp_err_t bo_wsc_nvs_do_commit(nvs_handle_t handle)
{
    ESP_LOGD(TAG, "driver called commit");
#ifndef CONFIG_BO_WSC_NVS_DISABLED
    esp_err_t ret = ESP_OK;
    _bo_wsc_lock();
    if(s_nvs_mode == BO_WSC_NVS_MODE_AUTO) {
        ret = bo_wsc_do_save();
    }
    _bo_wsc_release();
    return ret;
#else
    return ESP_OK;
#endif
}

static esp_err_t bo_wsc_nvs_set_i8  (nvs_handle_t handle, const char* key, int8_t value)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s", __func__, handle, key);
    return bo_wsc_nvs_do_set(handle, key, &value, 1);
}

static esp_err_t bo_wsc_nvs_get_i8  (nvs_handle_t handle, const char* key, int8_t* out_value)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s", __func__, handle, key);
    size_t size = sizeof(int8_t);
    return bo_wsc_nvs_do_get(handle, key, out_value, &size);
}

static esp_err_t bo_wsc_nvs_set_u8  (nvs_handle_t handle, const char* key, uint8_t value)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s", __func__, handle, key);
    return bo_wsc_nvs_do_set(handle, key, &value, 1);
}

static esp_err_t bo_wsc_nvs_get_u8  (nvs_handle_t handle, const char* key, uint8_t* out_value)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s", __func__, handle, key);
    size_t size = sizeof(uint8_t);
    return bo_wsc_nvs_do_get(handle, key, out_value, &size);
}

static esp_err_t bo_wsc_nvs_set_u16 (nvs_handle_t handle, const char* key, uint16_t value)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s", __func__, handle, key);
    return bo_wsc_nvs_do_set(handle, key, &value, 2);
}

static esp_err_t bo_wsc_nvs_get_u16  (nvs_handle_t handle, const char* key, uint16_t* out_value)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s", __func__, handle, key);
    size_t size = sizeof(uint16_t);
    return bo_wsc_nvs_do_get(handle, key, out_value, &size);
}

static esp_err_t bo_wsc_nvs_open(const char* name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 ", %s, %d]", __func__, *out_handle, name, open_mode);

    for(int i = 0; i < ARRAY_SIZE(bo_wsc_nvs_namespace_names); ++i) {
        if(strcmp(name, bo_wsc_nvs_namespace_names[i]) == 0) {
            *out_handle = (nvs_handle_t)(&s_bo_wsc_nvs.by_index[i]);
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "%s ns unknown: %s", __func__, name);
    return ESP_FAIL;
}

static void bo_wsc_nvs_close(nvs_handle_t handle)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "]", __func__, handle);
}

static esp_err_t bo_wsc_nvs_commit(nvs_handle_t handle)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "]", __func__, handle);
    return bo_wsc_nvs_do_commit(handle);
}

static esp_err_t bo_wsc_nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s (%u bytes)", __func__, handle, key, length);
    return bo_wsc_nvs_do_set(handle, key, value, length);
}

static esp_err_t bo_wsc_nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s (%u bytes)", __func__, handle, key, *length);
    return bo_wsc_nvs_do_get(handle, key, out_value, length);
}

static esp_err_t bo_wsc_nvs_erase_key(nvs_handle_t handle, const char* key)
{
    ESP_LOGD(TAG, "%s [%" PRIu32 "] %s", __func__, handle, key);
    return bo_wsc_nvs_do_erase(handle, key);
}

esp_err_t bo_wsc_set(wifi_osi_funcs_t *osi_funcs)
{
    if(osi_funcs == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    osi_funcs->_nvs_set_i8 = bo_wsc_nvs_set_i8;
    osi_funcs->_nvs_get_i8 = bo_wsc_nvs_get_i8;
    osi_funcs->_nvs_set_u8 = bo_wsc_nvs_set_u8;
    osi_funcs->_nvs_get_u8 = bo_wsc_nvs_get_u8;
    osi_funcs->_nvs_set_u16 = bo_wsc_nvs_set_u16;
    osi_funcs->_nvs_get_u16 = bo_wsc_nvs_get_u16;
    osi_funcs->_nvs_open = bo_wsc_nvs_open;
    osi_funcs->_nvs_close = bo_wsc_nvs_close;
    osi_funcs->_nvs_commit = bo_wsc_nvs_commit;
    osi_funcs->_nvs_set_blob = bo_wsc_nvs_set_blob;
    osi_funcs->_nvs_get_blob = bo_wsc_nvs_get_blob;
    osi_funcs->_nvs_erase_key = bo_wsc_nvs_erase_key;
    return ESP_OK;
}

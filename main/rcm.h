// rcm.h - Construccion de la imagen RCM (fusee gelee) para Tegra X1 / Nintendo Switch.
//
// La imagen que espera el bootROM en modo RCM tiene este formato (offsets sobre el
// flujo de datos que se envia por el endpoint bulk OUT):
//
//   0x0000 .. 0x02A7  comando RCM (680 bytes): u32 = 0x30298 + relleno a cero
//   0x02A8 .. 0x0323  intermezzo (124 bytes)   -> se ejecuta en 0x40010000
//   0x0324 .. 0x10E7  relleno a cero
//   0x10E8 .. 0x50E7  payload[0x0000 .. 0x3FFF] -> se carga en 0x40010E40
//   0x50E8 .. 0x72A7  "stack spray": 2160 x u32(0x40010000)
//   0x72A8 ..         payload[0x4000 ..]        -> se carga en 0x40017000
//
// El bootROM copia los primeros 680 bytes al buffer de comando y todo lo que va
// despues a partir de 0x40010000, por lo que el offset 0x02A8 del flujo acaba en
// 0x40010000. El intermezzo reubica las dos mitades del payload en 0x40010000 y
// salta a la entrypoint.
//
// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Tamano de cada transferencia bulk que espera el bootROM.
#define RCM_CHUNK_SIZE 0x1000u

/// Longitud maxima del comando RCM (limite del bootROM).
#define RCM_MAX_IMAGE_SIZE 0x30298u

/// Tamanio minimo util: por debajo de esto no hay payload que ejecutar.
#define RCM_MIN_PAYLOAD_SIZE 0x400u

/// Payload maximo que cabe: la imagen rellenada no puede pasar de 0x30000
/// bytes (mayor multiplo de 0x1000 bajo el limite 0x30298 del bootROM).
#define RCM_MAX_PAYLOAD_SIZE 183639u

typedef struct {
    const uint8_t *payload;
    size_t payload_len;
    size_t image_len;    ///< Longitud total, multiple de RCM_CHUNK_SIZE.
    size_t chunk_count;  ///< image_len / RCM_CHUNK_SIZE.
} rcm_image_t;

/**
 * @brief Prepara la imagen RCM a partir del payload del usuario.
 *
 * No reserva memoria: solo valida tamanios y calcula la longitud final.
 *
 * @param img          Imagen a inicializar.
 * @param payload      Payload (por ejemplo hekate) enlazado en 0x40010000.
 * @param payload_len  Longitud del payload en bytes.
 * @return ESP_OK, ESP_ERR_INVALID_ARG o ESP_ERR_INVALID_SIZE si no cabe.
 */
esp_err_t rcm_image_init(rcm_image_t *img, const uint8_t *payload, size_t payload_len);

/**
 * @brief Genera el chunk `index` de la imagen RCM.
 *
 * @param img    Imagen inicializada con rcm_image_init().
 * @param index  Indice del chunk (0 .. chunk_count-1).
 * @param out    Buffer de salida de RCM_CHUNK_SIZE bytes.
 */
void rcm_image_chunk(const rcm_image_t *img, size_t index, uint8_t *out);

#ifdef __cplusplus
}
#endif

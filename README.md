# Inyector RCM para Nintendo Switch con ESP32-S3

Firmware para **ESP-IDF** que convierte un **ESP32-S3** en un *dongle* de
inyección de payloads RCM (fusée gelée) para Nintendo Switch. El ESP32-S3 usa
su controlador **USB OTG en modo host** para hablar con la consola cuando esta
está en modo RCM y le envía el payload (hekate, fusee, etc.) por el endpoint
bulk, igual que haría `fusee-launcher` desde un PC.

No hace falta PC, ni Android, ni un dongle comercial: conectas el ESP32-S3 a la
consola y esta arranca el payload.

> **Estado:** compilado con **ESP-IDF v5.3.2** y **flasheado correctamente** en
> una ESP32-S3 (16 MB de flash, 8 MB de PSRAM). El firmware arranca y queda a la
> espera de la consola. El protocolo está contrastado con fusee-launcher (imagen
> RCM idéntica byte a byte frente a una implementación de referencia en Python)
> y con el hilo de GBATemp del mismo tema. **La inyección contra una consola real
> todavía no se ha probado** (no hay Switch conectada), así que revisa la sección
> de problemas si algo falla.

---

## 1. Requisitos

| Elemento | Detalle |
|---|---|
| Placa | ESP32-S3 con USB OTG nativo (DevKitC-1, DevKitM-1, módulo WROOM-1, etc.) |
| Flash | 4 MB o más (por defecto se configura 16 MB, como la placa probada) |
| Alimentación | 5 V para el ESP32-S3 **y** para el VBUS de la consola |
| Cable | USB-A macho a USB-C macho (o un latiguillo equivalente) |
| Consola | Switch **V1 sin parchear (Erista)**. OLED, Lite y Mariko no son vulnerables |
| Software | ESP-IDF v5.1 o superior (`idf.py`). **No es un proyecto Arduino** |
| Payload | `hekate_ctcaer_x.y.z.bin` o `fusee.bin` (enlazados en `0x40010000`) |

El ESP32-S3 **no** necesita PSRAM: la imagen RCM se genera por bloques de 4 KB
directamente desde flash, sin reservar un búfer grande.

---

## 2. Cableado

El puerto USB OTG del ESP32-S3 usa pines fijos:

| Señal USB | GPIO del ESP32-S3 |
|---|---|
| D− | **GPIO19** |
| D+ | **GPIO20** |
| VBUS (5 V) | pin `5V` de la placa |
| GND | `GND` |

La forma más sencilla y segura es usar un **receptáculo USB-A hembra** (o cortar
un cable USB-A) y cablearlo al ESP32-S3:

```
          USB-A hembra                         ESP32-S3
        ┌──────────────┐
        │  VBUS (rojo) ├────────────────────── 5V
        │  D−  (blanco)├───────────────────── GPIO19
        │  D+  (verde) ├───────────────────── GPIO20
        │  GND (negro) ├───────────────────── GND
        └──────┬───────┘
               │  cable USB-A → USB-C
        ┌──────┴───────┐
        │  Nintendo    │
        │  Switch (C)  │
        └──────────────┘
```

Notas:

* El cable **USB-A → USB-C** es importante: lleva la señal de CC que la consola
  espera de un host "clásico". Un cable USB-C a USB-C no vale para este montaje.
* La consola **no alimenta** el VBUS en modo RCM: los 5 V los tiene que poner el
  dongle. Alimenta el ESP32-S3 por su **puerto UART** (el del CP210x) o por el pin
  `5V`, y lleva esos mismos 5 V al VBUS del USB-A.
* **No conectes el puerto USB nativo del ESP32-S3 al PC** mientras hace de host:
  ese conector es el que lleva D+/D− y entraría en conflicto.
* Opcional pero recomendable: condensador de 100 µF entre 5 V y GND, y
  resistencias de 22 Ω en serie con D+ y D−.
* El ESP32-S3 y la consola deben compartir GND.

---

## 3. Preparar el payload

Descarga un payload enlazado para ejecutarse en `0x40010000`, por ejemplo:

* **hekate**: <https://github.com/CTCaer/hekate/releases> →
  `hekate_ctcaer_x.y.z.bin`
* **Atmosphère**: <https://github.com/Atmosphere-NX/Atmosphere/releases> →
  `fusee.bin`

Cópialo como `main/payload.bin`:

```bash
cp ~/Descargas/hekate_ctcaer_6.2.1.bin main/payload.bin
```

Puedes comprobar la imagen que se va a generar sin flashear nada:

```bash
python3 tools/build_rcm_image.py --payload main/payload.bin --info
```

Salida de ejemplo:

```
payload           : main/payload.bin (130000 bytes)
imagen total      : 143360 bytes (35 bloques de 0x1000)
comando RCM       : 0x30298
intermezzo @0x02A8 : 5c009fe5...
payload parte 1 @0x10E8 -> 0x40010E40
stack spray @0x50E8 -> 0x40014E40..0x40017000
payload parte 2 @0x72A8 -> 0x40017000
```

Límites: el payload debe ocupar entre **1024** y **183 639** bytes.

---

## 4. Compilar y flashear

```bash
# Una sola vez, si no tienes el entorno cargado:
. $HOME/esp/esp-idf/export.sh

idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor     # macOS: /dev/cu.usbserial-XXXX
```

El proyecto ya trae `sdkconfig.defaults` con lo necesario:

* consola por **UART0** (el USB-Serial-JTAG se desactiva porque comparte
  GPIO19/GPIO20 con el OTG),
* `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=1024`,
* flash de 16 MB y partición de app grande.

Si tu módulo tiene 4 u 8 MB de flash, cambia `CONFIG_ESPTOOLPY_FLASHSIZE_16MB`
por `CONFIG_ESPTOOLPY_FLASHSIZE_4MB` u `CONFIG_ESPTOOLPY_FLASHSIZE_8MB` en
`sdkconfig.defaults`.

---

## 5. Uso

1. Alimenta el dongle (5 V al ESP32-S3 y al VBUS del USB-A).
2. Abre el monitor serie por el puerto **UART** a 115200 baudios.
3. Con la consola **apagada del todo**, conéctala al USB-A del dongle.
4. Pon la consola en **modo RCM**: mantén **VOL+**, acerca el jig al pin 10 del
   raíl del Joy-Con derecho y pulsa **Power**. La pantalla se queda negra.
5. El firmware detecta el dispositivo NVIDIA, lee el Device ID, envía la imagen
   y dispara el exploit en menos de un segundo.
6. La consola arranca el payload (hekate o fusee).

Log esperado:

```
I (312) rcm: Inyector RCM para Nintendo Switch (ESP32-S3, USB OTG host)
I (312) rcm: Payload embebido: 130000 bytes
I (322) rcm: Imagen RCM: 143360 bytes en 35 bloques de 0x1000
I (842) rcm: Consola en RCM detectada (0955:7321)
I (852) rcm: Device ID (16 bytes):
9f0e...
I (853) rcm: Enviando imagen RCM: 143360 bytes (35 bloques)
I (1250) rcm: Imagen enviada. Disparando el exploit...
I (1250) rcm: Payload inyectado. La consola deberia estar arrancando.
```

> Cada intento vale para **una** sesión de RCM. Si falla, apaga la consola del
> todo (mantén Power 12 s) y repite el proceso. El bootROM se queda esperando el
> resto de un payload que ya no va a llegar, así que no sirve reintentar sin
> reiniciar la consola.

---

## 6. Cómo funciona

### 6.1 El protocolo RCM

Cuando la consola entra en RCM, el bootROM del Tegra X1 se presenta como un
dispositivo USB NVIDIA (`0955:7321`) con dos endpoints bulk:

| Endpoint | Dirección | Uso |
|---|---|---|
| `0x81` | IN | Device ID (16 bytes) |
| `0x01` | OUT | comando RCM + payload |

El host:

1. Enumera el dispositivo y lo configura.
2. Lee 16 bytes del Device ID (el bootROM los envía en bucle hasta que se
   configura).
3. Envía el comando RCM: un `u32` con la longitud total (`0x30298`) seguido de
   relleno hasta 680 bytes.
4. Envía el resto de la imagen en bloques de **0x1000 bytes**. El bootROM los
   va copiando a partir de `0x40010000`, alternando dos búferes DMA
   (`0x40005000` y `0x40009000`).
5. Dispara el fallo: una petición de control `GET_STATUS` al endpoint
   (`bmRequestType = 0x82`, `bRequest = 0`, `wLength = 0x7000`). El bootROM hace
   `memcpy` de `wLength` bytes desde la pila al búfer DMA sin comprobar el
   tamaño: copia el *stack spray* encima de las direcciones de retorno y salta a
   `0x40010000`.

La imagen se construye replicando exactamente lo que hace `fusee-launcher`:

| Offset | Contenido |
|---|---|
| `0x0000` | comando RCM (`u32 0x30298` + ceros hasta 680 B) |
| `0x02A8` | intermezzo (124 B), se ejecuta en `0x40010000` |
| `0x0324` | relleno |
| `0x10E8` | payload `[0x0000:0x4000]` → `0x40010E40` |
| `0x50E8` | 2160 × `u32 0x40010000` (stack spray) |
| `0x72A8` | payload `[0x4000:]` → `0x40017000` |

El *intermezzo* es un pequeño stub ARM que, una vez tomado el control, reubica
las dos mitades del payload de forma contigua en `0x40010000` y salta a su
entrypoint.

### 6.2 Por qué USB OTG

El ESP32-S3 tiene un controlador USB 1.1 *full speed* con PHY interno. El
stack **USB Host** de ESP-IDF (`usb_host_install`, `usb_host_transfer_submit*`)
permite actuar como host sin hardware adicional. El dispositivo RCM funciona a
full speed sin problemas (los dongles comerciales también son full speed).

La transferencia de disparo es una petición de control de 0x7000 bytes. El
stack de ESP-IDF la soporta: el descriptor DMA del DWC OTG admite hasta 128 KB
por QTD, así que no hay problema con los 28 KB del `GET_STATUS`.

---

## 7. Problemas frecuentes

**No aparece nada en el monitor serie**
Estás usando el puerto USB equivocado. El firmware saca los logs por **UART0**
(conector "UART"/CP210x de la placa), no por el USB nativo. Revisa también que
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` esté desactivado.

**El ESP32-S3 no detecta la consola**
* Comprueba que llegan 5 V al VBUS del USB-A (la consola no los da).
* D+/D− están cruzados o el cable es solo de carga: usa un cable con datos.
* La consola tiene que estar en RCM de verdad (pantalla negra, sin logo).
* GND común entre placa y consola.

**"Dispositivo 0955:xxxx ignorado"**
El VID/PID no es el de RCM. Puede ser que la consola no esté en RCM o que el
dongle haya enumerado otro dispositivo.

**"La consola no respondió al Device ID"**
El dispositivo se enumeró pero el bootROM no contestó: normalmente la consola
no estaba en RCM o el cable solo alimenta. Reinicia la consola y repite.

**Se envía la imagen pero la consola no arranca**
* Es una consola parcheada (Mariko/OLED/Lite): el fallo no existe ahí.
* La consola ya estaba en RCM antes de encender el dongle: el estado del
  bootROM puede ser inconsistente. Reinicia la consola y repite.
* Prueba otro payload (hekate suele ser el más tolerante).

**El ESP32-S3 se reinicia al conectar la consola**
Consumo excesivo o caída de tensión en VBUS. Alimenta el dongle con una fuente
capaz de 1 A y añade el condensador de 100 µF.

---

## 8. Estructura del proyecto

```
switch-rcm-injector/
├── CMakeLists.txt
├── sdkconfig.defaults
├── LICENSE                  (GPLv2, por el intermezzo de fusee-launcher)
├── main/
│   ├── CMakeLists.txt
│   ├── main.c               USB host, detección, envío y disparo
│   ├── rcm.c / rcm.h        construcción de la imagen RCM
│   ├── intermezzo.h         stub relocador (124 B, generado del binario)
│   ├── intermezzo.bin       original, para la herramienta de Python
│   └── payload.bin          <-- lo pones tú
└── tools/
    └── build_rcm_image.py   generador/visor de la imagen RCM
```

---

## 9. Créditos

Este proyecto existe gracias al trabajo de otra gente:

* **fusée gelée / fusee-launcher** — [Kate Temkin (@ktemkin)](https://github.com/ktemkin)
  y [Mikaela Szekely (@Qyriad)](https://github.com/Qyriad), con el soporte de
  **ReSwitched**. El protocolo RCM, el *stack spray* y el stub `intermezzo`
  (124 B) son suyos. Licencia **GPLv2**, que se mantiene en este repositorio
  (ver [`LICENSE`](LICENSE)). Espejo del proyecto:
  <https://codeberg.org/libre-tegra/fusee-launcher-new>.
* **ReSwitched** — divulgación de la vulnerabilidad *fusée gelée*
  (CVE-2018-6242), descubierta por Kate Temkin.
* **hekate** — [CTCaer](https://github.com/CTCaer/hekate). Payload de ejemplo
  para la consola; **no** se incluye en el repositorio (se descarga aparte).
* **Atmosphère** — [equipo de Atmosphère](https://github.com/Atmosphere-NX/Atmosphere),
  autor de `fusee.bin`, el otro payload habitual.
* **ESP-IDF y el stack USB Host del ESP32-S3** — [Espressif Systems](https://www.espressif.com),
  licencia Apache-2.0.
* **Referencia ESP32-S3** — hilo de GBATemp
  [*V1 Unpatched Switch ESP32-S3 payload injector*](https://gbatemp.net/threads/v1-unpatched-switch-esp32-s3-payload-injector.679333/)
  (AmeliaFox y colaboradores), donde se documenta un inyector equivalente para
  ESP32-S3 y se confirma el uso de GPIO19/20 y del `GET_STATUS` de `0x7000`.
  No se ha copiado código de ese hilo; se cita como referencia y validación del
  enfoque.

Si eres autor de alguno de estos proyectos y quieres ajustar el crédito o la
atribución, abre un issue y se corrige.

## 10. Aviso de "vibecoding"

Este repositorio se ha desarrollado con **asistencia de IA** (*vibecoding*):
el código, la documentación y las pruebas se generaron en su mayor parte en un
diálogo con un agente de IA (**DeepSeek Harness**). El autor humano define el
objetivo, revisa el resultado y decide qué se publica.

Consecuencias prácticas:

* El firmware **se ha compilado, flasheado y arrancado** correctamente en una
  ESP32-S3, y el generador de la imagen RCM se ha verificado byte a byte contra
  una implementación de referencia.
* **La inyección contra una consola real no se ha validado** en el momento de
  publicar esto.
* Revisa el código antes de confiar en él y úsalo bajo tu responsabilidad.

## 11. Aviso

Este software es solo para **hardware propio**. Modificar una consola puede
anular la garantía e incumplir los términos de servicio de Nintendo. No lo uses
para piratear juegos ni para distribuir contenido con copyright. El autor no se
hace responsable del uso que se le dé.

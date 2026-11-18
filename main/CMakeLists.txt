# El payload a inyectar se embebe en el firmware desde main/payload.bin.
set(RCM_PAYLOAD "${CMAKE_CURRENT_LIST_DIR}/payload.bin")

if(NOT EXISTS "${RCM_PAYLOAD}")
    message(FATAL_ERROR
        "\n"
        "  Falta main/payload.bin.\n"
        "\n"
        "  Copia ahi el payload que quieras inyectar. Por ejemplo, descarga\n"
        "  hekate_ctcaer_x.y.z.bin desde https://github.com/CTCaer/hekate/releases\n"
        "  y renombralo como main/payload.bin.\n")
endif()

file(SIZE "${RCM_PAYLOAD}" RCM_PAYLOAD_SIZE)
if(RCM_PAYLOAD_SIZE GREATER 183639)
    message(FATAL_ERROR
        "  main/payload.bin ocupa ${RCM_PAYLOAD_SIZE} bytes; el maximo que cabe\n"
        "  en una imagen RCM es 183639 bytes.")
endif()

message(STATUS "Payload RCM embebido: ${RCM_PAYLOAD_SIZE} bytes")

idf_component_register(
    SRCS "main.c" "rcm.c"
    INCLUDE_DIRS "."
    EMBED_FILES "payload.bin")

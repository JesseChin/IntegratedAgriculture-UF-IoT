# Embed the server root certificate into the final binary
#
# (If this was a component, we would set COMPONENT_EMBED_TXTFILES here.)
idf_component_register(SRCS "esp_moisture_http.c"
                    INCLUDE_DIRS "."
                    EMBED_TXTFILES howsmyssl_com_root_cert.pem
                    postman_root_cert.pem)

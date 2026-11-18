#include "cmsis_dap_tcp.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "dap_main.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "netinet/tcp.h"

#define CMSIS_DAP_TCP_HEADER_SIZE 8U
#define CMSIS_DAP_TCP_REQUEST_TYPE 1U
#define CMSIS_DAP_TCP_RESPONSE_TYPE 2U
#define CMSIS_DAP_TCP_RETRY_DELAY_MS 1000U
#define CMSIS_DAP_TCP_PACKET_SIZE 1024U
#define CMSIS_DAP_TCP_PACKET_COUNT 32U
// Keep the host-side TCP window deep enough to hide network latency, but execute no more SWD
// commands back-to-back than the stable USB CMSIS-DAP path does before yielding.
#define CMSIS_DAP_TCP_EXECUTION_BATCH_SIZE DAP_PACKET_COUNT
#define CMSIS_DAP_COMMAND_INFO 0x00U
#define CMSIS_DAP_INFO_PACKET_COUNT 0xFEU
#define CMSIS_DAP_INFO_PACKET_SIZE 0xFFU

static const char *TAG = "CMSIS_DAP_TCP";
static const uint8_t CMSIS_DAP_TCP_SIGNATURE[4] = {'D', 'A', 'P', 0};

typedef enum {
    CMSIS_DAP_REQUEST_RECEIVED,
    CMSIS_DAP_REQUEST_CLOSED,
    CMSIS_DAP_REQUEST_INVALID,
} cmsis_dap_request_result_t;

static bool receive_all(int socket_fd, uint8_t *buffer, size_t length)
{
    size_t received = 0;

    while (received < length) {
        const ssize_t count = recv(socket_fd, buffer + received, length - received, 0);
        if (count == 0) {
            return false;
        }
        if (count <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        received += (size_t) count;
    }

    return true;
}

static bool send_all(int socket_fd, const uint8_t *buffer, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        const ssize_t count = send(socket_fd, buffer + sent, length - sent, 0);
        if (count <= 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += (size_t) count;
    }

    return true;
}

static cmsis_dap_request_result_t receive_request(int socket_fd, uint8_t *request)
{
    uint8_t header[CMSIS_DAP_TCP_HEADER_SIZE];
    if (!receive_all(socket_fd, header, sizeof(header))) {
        return CMSIS_DAP_REQUEST_CLOSED;
    }
    if (memcmp(header, CMSIS_DAP_TCP_SIGNATURE, sizeof(CMSIS_DAP_TCP_SIGNATURE)) != 0 ||
        header[6] != CMSIS_DAP_TCP_REQUEST_TYPE) {
        ESP_LOGW(TAG, "Invalid CMSIS-DAP TCP request header");
        return CMSIS_DAP_REQUEST_INVALID;
    }

    const size_t request_length = (size_t) header[4] | ((size_t) header[5] << 8U);
    if (request_length == 0 || request_length > CMSIS_DAP_TCP_PACKET_SIZE) {
        ESP_LOGW(TAG, "Invalid CMSIS-DAP TCP payload length: %u", (unsigned) request_length);
        return CMSIS_DAP_REQUEST_INVALID;
    }

    memset(request, 0, CMSIS_DAP_TCP_PACKET_SIZE);
    if (!receive_all(socket_fd, request, request_length)) {
        return CMSIS_DAP_REQUEST_CLOSED;
    }
    return CMSIS_DAP_REQUEST_RECEIVED;
}

static bool request_is_buffered(int socket_fd, bool *client_closed)
{
    uint8_t next_byte;
    while (true) {
        const ssize_t count = recv(socket_fd, &next_byte, sizeof(next_byte), MSG_PEEK | MSG_DONTWAIT);
        if (count > 0) {
            return true;
        }
        if (count == 0) {
            *client_closed = true;
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        *client_closed = true;
        return false;
    }
}

static bool handle_client(int socket_fd)
{
    static uint8_t request[CMSIS_DAP_TCP_PACKET_SIZE];
    static uint8_t response_batch[
        CMSIS_DAP_TCP_EXECUTION_BATCH_SIZE *
        (CMSIS_DAP_TCP_HEADER_SIZE + CMSIS_DAP_TCP_PACKET_SIZE)];

    while (true) {
        size_t response_batch_length = 0;
        bool client_closed = false;

        for (size_t packet = 0; packet < CMSIS_DAP_TCP_EXECUTION_BATCH_SIZE; ++packet) {
            const cmsis_dap_request_result_t request_result = receive_request(socket_fd, request);
            if (request_result == CMSIS_DAP_REQUEST_CLOSED) {
                client_closed = true;
                break;
            }
            if (request_result == CMSIS_DAP_REQUEST_INVALID) {
                return false;
            }

            uint8_t *response_frame = response_batch + response_batch_length;
            const uint16_t response_length = (uint16_t) chry_dap_execute_command(
                request, response_frame + CMSIS_DAP_TCP_HEADER_SIZE);
            if (response_length > CMSIS_DAP_TCP_PACKET_SIZE) {
                ESP_LOGE(TAG, "CMSIS-DAP response is too large: %u", (unsigned) response_length);
                return false;
            }

            // The USB interface remains a 64-byte full-speed CMSIS-DAP endpoint, but the TCP
            // transport is not constrained by the USB endpoint packet size. Advertising the
            // larger TCP buffer lets hosts use substantially larger DAP_TransferBlock commands.
            if (request[0] == CMSIS_DAP_COMMAND_INFO &&
                request[1] == CMSIS_DAP_INFO_PACKET_SIZE) {
                response_frame[CMSIS_DAP_TCP_HEADER_SIZE + 1U] = 2U;
                response_frame[CMSIS_DAP_TCP_HEADER_SIZE + 2U] =
                    (uint8_t) CMSIS_DAP_TCP_PACKET_SIZE;
                response_frame[CMSIS_DAP_TCP_HEADER_SIZE + 3U] =
                    (uint8_t) (CMSIS_DAP_TCP_PACKET_SIZE >> 8U);
            } else if (request[0] == CMSIS_DAP_COMMAND_INFO &&
                       request[1] == CMSIS_DAP_INFO_PACKET_COUNT) {
                response_frame[CMSIS_DAP_TCP_HEADER_SIZE + 1U] = 1U;
                response_frame[CMSIS_DAP_TCP_HEADER_SIZE + 2U] =
                    CMSIS_DAP_TCP_PACKET_COUNT;
            }

            memcpy(response_frame, CMSIS_DAP_TCP_SIGNATURE, sizeof(CMSIS_DAP_TCP_SIGNATURE));
            response_frame[4] = (uint8_t) response_length;
            response_frame[5] = (uint8_t) (response_length >> 8U);
            response_frame[6] = CMSIS_DAP_TCP_RESPONSE_TYPE;
            response_frame[7] = 0;
            response_batch_length += CMSIS_DAP_TCP_HEADER_SIZE + response_length;

            if (packet + 1U == CMSIS_DAP_TCP_EXECUTION_BATCH_SIZE ||
                !request_is_buffered(socket_fd, &client_closed)) {
                break;
            }
        }

        if (response_batch_length != 0 &&
            !send_all(socket_fd, response_batch, response_batch_length)) {
            return true;
        }
        if (client_closed) {
            return true;
        }

        // USB returns to cherry_dap_task after draining at most DAP_PACKET_COUNT requests, where
        // that task yields. Mirror the same execution boundary for TCP while keeping later
        // requests queued in the socket, so Wi-Fi latency is hidden without one long SWD burst.
        taskYIELD();
    }
}

static int open_listener(void)
{
    const int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        return -1;
    }

    const int enabled = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CMSIS_DAP_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (const struct sockaddr *) &address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "Unable to bind port %u: errno %d", CMSIS_DAP_TCP_PORT, errno);
        close(socket_fd);
        return -1;
    }
    if (listen(socket_fd, DAP_PACKET_COUNT) != 0) {
        ESP_LOGE(TAG, "Unable to listen on port %u: errno %d", CMSIS_DAP_TCP_PORT, errno);
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

void cmsis_dap_tcp_server_task(void *argument)
{
    (void) argument;

    const esp_err_t power_save_result = esp_wifi_set_ps(WIFI_PS_NONE);
    if (power_save_result != ESP_OK) {
        ESP_LOGW(TAG, "Could not disable Wi-Fi power save: %s", esp_err_to_name(power_save_result));
    }

    while (true) {
        const int listen_socket = open_listener();
        if (listen_socket < 0) {
            vTaskDelay(pdMS_TO_TICKS(CMSIS_DAP_TCP_RETRY_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "CMSIS-DAP TCP server listening on port %u", CMSIS_DAP_TCP_PORT);
        while (true) {
            struct sockaddr_storage source_address;
            socklen_t source_length = sizeof(source_address);
            const int client_socket = accept(
                listen_socket, (struct sockaddr *) &source_address, &source_length);
            if (client_socket < 0) {
                ESP_LOGE(TAG, "Accept failed: errno %d", errno);
                break;
            }

            const int enabled = 1;
            setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
            setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
            ESP_LOGI(TAG, "CMSIS-DAP TCP client connected");

            const bool clean_disconnect = handle_client(client_socket);
            if (!clean_disconnect) {
                ESP_LOGW(TAG, "Closing invalid CMSIS-DAP TCP connection");
            }
            shutdown(client_socket, SHUT_RDWR);
            close(client_socket);
            ESP_LOGI(TAG, "CMSIS-DAP TCP client disconnected");
        }

        close(listen_socket);
        vTaskDelay(pdMS_TO_TICKS(CMSIS_DAP_TCP_RETRY_DELAY_MS));
    }
}

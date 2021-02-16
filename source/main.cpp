/*
 * Copyright (c) 2017 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mbed.h"
#include "CellularNonIPSocket.h"
#include "UDPSocket.h"
#include "TCPSocket.h"
#include "cellular_demo_tracing.h"

/* configuration choices in mbed_app.json */
#define UDP 0
#define TCP 1
#define NONIP 2

#if MBED_CONF_APP_SOCK_TYPE == TCP
static constexpr char SOCKET_TYPE[] = "TCP";
#elif MBED_CONF_APP_SOCK_TYPE == UDP
static constexpr char SOCKET_TYPE[] = "UDP";
#elif MBED_CONF_APP_SOCK_TYPE == NONIP
static constexpr char SOCKET_TYPE[] = "CellularNonIP";
#endif
static const char ECHO_HOSTNAME[] = MBED_CONF_APP_ECHO_SERVER_HOSTNAME;

// IoT Technologies
static const int IOT_TECHNOLOGY_CATM1           = 0;
static const int IOT_TECHNOLOGY_NBIOT           = 1;
static const int IOT_TECHNOLOGY_CATM1_PREFERRED = 2;
static const int IOT_TECHNOLOGY_NBIOT_PREFERRED = 3;

// Desired IoT technology
static const int DESIRED_IOT_TECHNOLOGY = IOT_TECHNOLOGY_NBIOT;

void set_iot_technology(CellularDevice *dev, int desired_iot_technology)
{
    int iot_technology = -1;

    // Check if the device is ready
    while (dev->is_ready() != NSAPI_ERROR_OK) {
        // Power cycle the module
        dev->hard_power_off();
        dev->hard_power_on();
        dev->soft_power_off();
        dev->soft_power_on();
        ThisThread::sleep_for(10000);
        dev->init();
    }
    
    // Check the current IoT technology first
    ATHandler *at_handler = dev->get_at_handler();
    at_handler->lock();
    at_handler->cmd_start_stop("#WS46", "?");
    at_handler->resp_start("#WS46:");

    //2,4,12,
    //2+8+2048 = 2058
    //set ATT nbiot bands
    at_handler->at_cmd_discard("#BND", "=", "%d%d%d", 5, 0, 2058);


    // Read the current IoT technology
    iot_technology = at_handler->read_int();
    at_handler->resp_stop();

    // If already configured as desired, return
    if (iot_technology == desired_iot_technology) {
        at_handler->unlock();
        return;
    }

    //set nbiot
    at_handler->at_cmd_discard("#WS46", "=", "%d", desired_iot_technology);
    if (at_handler->get_last_error() != NSAPI_ERROR_OK) {
        printf("ERROR: Unable to set IoT technology!\n");
        at_handler->unlock();
        return;
    }

    at_handler->unlock();

    // Power cycle the module
    dev->hard_power_off();
    dev->hard_power_on();
    dev->soft_power_on();

    return;
}




class CellularDemo {
    static constexpr uint8_t RETRY_COUNT = 3;

public:
    CellularDemo(NetworkInterface &network)
        : _net(network)
    { }

    ~CellularDemo() { }

    /** Run the cellular demo. */
    void run()
    {
        /* sim pin, apn, credentials and possible plmn are taken automatically from json
         * when using NetworkInterface::set_default_parameters() */
        _net.set_default_parameters();

        nsapi_size_or_error_t ret = NSAPI_ERROR_NO_CONNECTION;

        if (connect_cellular()) {
            /* ping echo server */
            if (!test_send_and_receive()) {
                printf("Sending and received data failed.\n");
            }

            ret = _net.disconnect();

            if (ret != NSAPI_ERROR_OK) {
                printf("Disconnect failed (error: %d).\n", ret);
            }
        }

        if (ret == NSAPI_ERROR_OK) {
            printf("Success. Exiting\n");
        } else {
            printf("Failure. Exiting\n");
        }
    }

private:
    /**
     * For UDP or TCP it opens a socket with the given echo server and performs an echo transaction.
     * For Cellular Non-IP it opens a socket for which the data delivery path is decided
     * by network's control plane CIoT optimisation setup, for the given APN.
     */
    bool test_send_and_receive()
    {
        nsapi_size_or_error_t ret;

        ret = _socket.open(&_net);

        if (ret != NSAPI_ERROR_OK) {
            printf("%sSocket.open() fails, code: %d\n", SOCKET_TYPE, ret);
            return false;
        }

        _socket.set_timeout(15000);

        if (!resolve_hostname()) {
            return false;
        }

        if (!connect_socket()) {
            return false;
        }

        ret = send_test_data();

        if (ret < 0) {
            printf("%sSocket.send() fails, code: %d\n", SOCKET_TYPE, ret);
            return false;
        } else {
            printf("%s: Sent %d Bytes to %s\n", SOCKET_TYPE, ret, ECHO_HOSTNAME);
        }

        ret = receive_test_data();

        if (ret < 0) {
            printf("%sSocket.recv() fails, code: %d\n", SOCKET_TYPE, ret);
            return false;
        } else {
            printf("Received from echo server %d Bytes\n", ret);
        }

        _socket.close();

        if (ret != NSAPI_ERROR_OK) {
            printf("%sSocket.close() fails, code: %d\n", SOCKET_TYPE, ret);
            return false;
        }

        return true;
    }

    /** Connects to the Cellular Network */
    bool connect_cellular()
    {
        printf("Establishing connection\n");

        /* check if we're already connected */
        if (_net.get_connection_status() == NSAPI_STATUS_GLOBAL_UP) {
            return true;
        }

        nsapi_error_t ret;

        for (uint8_t retry = 0; retry <= RETRY_COUNT; retry++) {
            ret = _net.connect();

            if (ret == NSAPI_ERROR_OK) {
                printf("Connection Established.\n");
                return true;
            } else if (ret == NSAPI_ERROR_AUTH_FAILURE) {
                printf("Authentication Failure.\n");
                return false;
            } else {
                printf("Couldn't connect: %d, will retry\n", ret);
            }
        }

        printf("Fatal connection failure: %d\n", ret);

        return false;
    }

    /** Connects to the Cellular Network */
    bool resolve_hostname()
    {
#if MBED_CONF_APP_SOCK_TYPE != NONIP
        nsapi_error_t ret = _net.gethostbyname(ECHO_HOSTNAME, &_socket_address);

        if (ret != NSAPI_ERROR_OK) {
            printf("Couldn't resolve remote host: %s, code: %d\n", ECHO_HOSTNAME, ret);
            return false;
        }

        _socket_address.set_port(MBED_CONF_APP_ECHO_SERVER_PORT);
#endif
        return true;
    }

    bool connect_socket()
    {
#if MBED_CONF_APP_SOCK_TYPE == TCP
        nsapi_error_t ret = _socket.connect(_socket_address);
        if (ret < 0) {
            printf("TCPSocket.connect() fails, code: %d\n", ret);
            return false;
        } else {
            printf("TCP: connected with %s server\n", ECHO_HOSTNAME);
        }
#endif
        return true;
    }

    nsapi_error_t send_test_data()
    {
        const char *echo_string = "TEST";
#if MBED_CONF_APP_SOCK_TYPE == UDP
        return _socket.sendto(_socket_address, (void*)echo_string, strlen(echo_string));
#else
        return _socket.send((void*)echo_string, strlen(echo_string));
#endif
    }

    nsapi_error_t receive_test_data()
    {
        char receive_buffer[4];
#if MBED_CONF_APP_SOCK_TYPE == UDP
        return _socket.recvfrom(&_socket_address, (void*)receive_buffer, sizeof(receive_buffer));
#else
        return _socket.recv((void*)receive_buffer, sizeof(receive_buffer));
#endif
    }

private:
    NetworkInterface &_net;

#if MBED_CONF_APP_SOCK_TYPE == TCP
    TCPSocket _socket;
    SocketAddress _socket_address;
#elif MBED_CONF_APP_SOCK_TYPE == UDP
    UDPSocket _socket;
    SocketAddress _socket_address;
#elif MBED_CONF_APP_SOCK_TYPE == NONIP
    CellularNonIPSocket _socket;
#endif
};

int main() {
    printf("\nmbed-os-example-cellular-agora-nbiot\n");

    trace_open();

#if MBED_CONF_APP_SOCK_TYPE == NONIP
    NetworkInterface *net = CellularContext::get_default_nonip_instance();
#else
    NetworkInterface *net = CellularContext::get_default_instance();
    CellularDevice *dev = CellularDevice::get_target_default_instance();
#endif

    if (net) {
        CellularDemo example(*net);
        set_iot_technology(dev, DESIRED_IOT_TECHNOLOGY);
        example.run();
    } else {
        printf("Failed to get_default_instance()\n");
    }

    trace_close();

    return 0;
}

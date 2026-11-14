#ifndef NOVA_NETWORK_H
#define NOVA_NETWORK_H
#include <stdbool.h>
#include <stdint.h>
#define NETWORK_RESPONSE_MAX 196608u
void network_init(void);
void network_poll(void);
bool network_present(void);
bool network_link_up(void);
const char *network_status(void);
const char *network_ip(void);
const char *network_gateway(void);
const char *network_dns(void);
uint32_t network_rx_packets(void);
uint32_t network_tx_packets(void);
/* Starts one asynchronous request; a completed response contains the HTTP body.
 * HTTPS URLs use the host bridge at 10.0.2.2:8088, rather than native TLS. */
bool network_fetch(const char *url);
bool network_busy(void);
const char *network_response(void);
const char *network_error(void);
uint32_t network_response_length(void);
/* HTTP status code of the last response (0 if none) and its Location header
 * value for 3xx redirects (empty string if the header is absent). */
int network_status_code(void);
const char *network_redirect(void);
/* Changes once for each completed request or error. */
uint32_t network_generation(void);
#endif

#include <string.h>

#include "network.h"
static bool quiet_request;
#include "system.h"
#include "terminal.h"
#include <stddef.h>

/* A single-request, polling network stack for QEMU user networking.
 * DMA addresses are physical because this learning kernel does not enable paging.
 * HTTPS is fetched through the explicit host bridge, which performs TLS. */

#define LOCAL_IP 0x0a00020fu
#define GATEWAY_IP 0x0a000202u
#define DNS_IP 0x0a000203u
#define RX_SIZE 8192u
#define RAW_SIZE 262144u
#define HTTP_HEADER_MAX 16384u
#define TCP_MSS 1400u
#define PACKED __attribute__((packed))

typedef struct PACKED { uint8_t dst[6], src[6]; uint16_t type; } Ethernet;
typedef struct PACKED {
    uint16_t hardware, protocol; uint8_t hlen, plen; uint16_t operation;
    uint8_t sender_mac[6]; uint32_t sender_ip; uint8_t target_mac[6]; uint32_t target_ip;
} Arp;
typedef struct PACKED {
    uint8_t version, tos; uint16_t length, id, fragment; uint8_t ttl, protocol;
    uint16_t checksum; uint32_t source, destination;
} Ipv4;
typedef struct PACKED { uint16_t source, destination, length, checksum; } Udp;
typedef struct PACKED {
    uint16_t source, destination; uint32_t sequence, acknowledgement;
    uint8_t offset, flags; uint16_t window, checksum, urgent;
} Tcp;
typedef struct { uint32_t ip; uint8_t mac[6]; } ArpEntry;
typedef enum { IDLE, DNS_ARP, DNS_WAIT, TCP_ARP, TCP_SYN, TCP_RESPONSE } FetchState;

static uint16_t io_base;
static bool present, link_up;
static uint8_t own_mac[6];
static uint8_t receive_ring[RX_SIZE + 16u + 1536u] __attribute__((aligned(256)));
static uint8_t transmit_buffer[4][1600] __attribute__((aligned(256)));
static uint16_t receive_offset;
static uint8_t transmit_slot;
static uint32_t rx_packets, tx_packets;
static ArpEntry arp_cache[8];
static unsigned int arp_next;
static uint16_t packet_id = 1;
static FetchState state;
static uint32_t started, last_send, last_activity, generation;
static uint32_t remote_ip, initial_seq, send_next, receive_next;
static uint16_t remote_port, local_port, dns_id;
static unsigned int attempts;
static char host[254], request[TCP_MSS + 1u], response[NETWORK_RESPONSE_MAX + 1u], error_text[128];
static uint32_t request_length, response_length, raw_length;
static uint8_t raw_response[RAW_SIZE];
static int http_status;
static char redirect_location[1024];
static bool request_acked;
static const char *status_text = "Offline: no RTL8139 network adapter";

static inline void out8(uint16_t port, uint8_t value) { __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
static inline void out16(uint16_t port, uint16_t value) { __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
static inline void out32(uint16_t port, uint32_t value) { __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
static inline uint8_t in8(uint16_t port) { uint8_t value; __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory"); return value; }
static inline uint16_t in16(uint16_t port) { uint16_t value; __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory"); return value; }
static inline uint32_t in32(uint16_t port) { uint32_t value; __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory"); return value; }
static uint16_t swap16(uint16_t value) { return (uint16_t)((value << 8) | (value >> 8)); }
static uint32_t swap32(uint32_t value) { return __builtin_bswap32(value); }
static void copy(void *destination, const void *source, uint32_t count) {
    uint8_t *d = destination; const uint8_t *s = source;
    for (uint32_t i = 0; i < count; ++i) d[i] = s[i];
}
static void zero(void *destination, uint32_t count) { uint8_t *d = destination; for (uint32_t i = 0; i < count; ++i) d[i] = 0; }
static uint32_t length(const char *text) { uint32_t n = 0; while (text[n]) ++n; return n; }
static bool starts(const char *text, const char *prefix) { while (*prefix) if (*text++ != *prefix++) return false; return true; }
static bool same_case(const uint8_t *text, uint32_t count, const char *wanted) {
    uint32_t i = 0;
    for (; wanted[i]; ++i) {
        if (i >= count) return false;
        uint8_t c = text[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (uint8_t)wanted[i]) return false;
    }
    return i == count;
}
static void set_text(char *destination, const char *text, uint32_t capacity) {
    uint32_t n = 0; while (text[n] && n + 1u < capacity) { destination[n] = text[n]; ++n; } destination[n] = 0;
}
static void log_line(const char *text) { terminal_log(text); terminal_log("\n"); }
static void log_number(const char *prefix, uint32_t number) {
    char text[80], digits[11]; uint32_t n = 0, d = 0;
    while (*prefix && n < 60u) text[n++] = *prefix++;
    do { digits[d++] = (char)('0' + number % 10u); number /= 10u; } while (number);
    while (d) text[n++] = digits[--d];
    text[n] = 0; log_line(text);
}
static uint32_t checksum_sum(uint32_t sum, const uint8_t *data, uint32_t count) {
    while (count > 1u) { sum += ((uint32_t)data[0] << 8) | data[1]; data += 2; count -= 2; }
    if (count) sum += (uint32_t)*data << 8;
    return sum;
}
static uint16_t checksum_finish(uint32_t sum) { while (sum >> 16) sum = (sum & 65535u) + (sum >> 16); return swap16((uint16_t)~sum); }
static uint16_t transport_checksum(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, uint16_t count) {
    uint32_t sum = (source >> 16) + (source & 65535u) + (destination >> 16) + (destination & 65535u) + protocol + count;
    return checksum_finish(checksum_sum(sum, data, count));
}
static uint32_t pci_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    out32(0xcf8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)device << 11) | ((uint32_t)function << 8) | (offset & 0xfcu)); return in32(0xcfc);
}
static void pci_write(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    out32(0xcf8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)device << 11) | ((uint32_t)function << 8) | (offset & 0xfcu)); out32(0xcfc, value);
}
static bool transmit(const void *frame, uint16_t count) {
    if (!present || count > 1600u) return false;
    uint8_t slot = transmit_slot;
    if (!(in32((uint16_t)(io_base + 0x10u + slot * 4u)) & (1u << 13))) return false;
    copy(transmit_buffer[slot], frame, count);
    if (count < 60u) { zero(transmit_buffer[slot] + count, 60u - count); count = 60u; }
    __asm__ volatile("" : : : "memory");
    out32((uint16_t)(io_base + 0x10u + slot * 4u), count);
    transmit_slot = (uint8_t)((slot + 1u) & 3u); ++tx_packets; return true;
}
static uint32_t route(uint32_t ip) { return (ip & 0xffffff00u) == (LOCAL_IP & 0xffffff00u) ? ip : GATEWAY_IP; }
static const uint8_t *find_mac(uint32_t ip) {
    ip = route(ip); for (unsigned int i = 0; i < 8; ++i) if (arp_cache[i].ip == ip) return arp_cache[i].mac; return NULL;
}
static void learn_mac(uint32_t ip, const uint8_t *mac) {
    if (!ip) return;
    for (unsigned int i = 0; i < 8; ++i) if (arp_cache[i].ip == ip) { copy(arp_cache[i].mac, mac, 6); return; }
    arp_cache[arp_next].ip = ip; copy(arp_cache[arp_next].mac, mac, 6); arp_next = (arp_next + 1u) & 7u;
}
static void arp_send(uint32_t target, const uint8_t *target_mac, bool reply) {
    uint8_t frame[60]; zero(frame, sizeof(frame)); Ethernet *eth = (Ethernet *)frame; Arp *arp = (Arp *)(frame + sizeof(Ethernet));
    for (unsigned int i = 0; i < 6; ++i) eth->dst[i] = target_mac ? target_mac[i] : 255;
    copy(eth->src, own_mac, 6); eth->type = swap16(0x0806);
    arp->hardware = swap16(1); arp->protocol = swap16(0x0800); arp->hlen = 6; arp->plen = 4; arp->operation = swap16(reply ? 2 : 1);
    copy(arp->sender_mac, own_mac, 6); arp->sender_ip = swap32(LOCAL_IP); arp->target_ip = swap32(target);
    if (target_mac) copy(arp->target_mac, target_mac, 6);
    transmit(frame, sizeof(frame));
}
static void ip_header(Ethernet *eth, Ipv4 *ip, const uint8_t *mac, uint32_t destination, uint8_t protocol, uint16_t payload_length) {
    copy(eth->dst, mac, 6); copy(eth->src, own_mac, 6); eth->type = swap16(0x0800);
    zero(ip, sizeof(*ip)); ip->version = 0x45; ip->length = swap16((uint16_t)(sizeof(*ip) + payload_length)); ip->id = swap16(packet_id++);
    ip->fragment = swap16(0x4000); ip->ttl = 64; ip->protocol = protocol; ip->source = swap32(LOCAL_IP); ip->destination = swap32(destination);
    ip->checksum = checksum_finish(checksum_sum(0, (const uint8_t *)ip, sizeof(*ip)));
}
static void dns_send(void) {
    const uint8_t *mac = find_mac(DNS_IP); if (!mac) return;
    uint8_t frame[600]; Ethernet *eth = (Ethernet *)frame; Ipv4 *ip = (Ipv4 *)(frame + 14); Udp *udp = (Udp *)(frame + 34); uint8_t *dns = frame + 42;
    zero(dns, 12); dns[0] = (uint8_t)(dns_id >> 8); dns[1] = (uint8_t)dns_id; dns[2] = 1; dns[5] = 1;
    uint32_t p = 12, start = 0, n = length(host);
    for (uint32_t i = 0; i <= n; ++i) if (host[i] == '.' || !host[i]) { uint32_t part = i - start; if (!part || part > 63) return; dns[p++] = (uint8_t)part; copy(dns + p, host + start, part); p += part; start = i + 1; }
    dns[p++] = 0; dns[p++] = 0; dns[p++] = 1; dns[p++] = 0; dns[p++] = 1;
    udp->source = swap16(local_port); udp->destination = swap16(53); udp->length = swap16((uint16_t)(p + 8)); udp->checksum = 0;
    udp->checksum = transport_checksum(LOCAL_IP, DNS_IP, 17, udp, (uint16_t)(p + 8));
    ip_header(eth, ip, mac, DNS_IP, 17, (uint16_t)(p + 8)); transmit(frame, (uint16_t)(42u + p));
}
static void tcp_send(uint8_t flags, uint32_t sequence, const void *payload, uint16_t count) {
    const uint8_t *mac = find_mac(remote_ip); if (!mac) return;
    uint8_t frame[1600]; Ethernet *eth = (Ethernet *)frame; Ipv4 *ip = (Ipv4 *)(frame + 14); Tcp *tcp = (Tcp *)(frame + 34);
    uint16_t header_length = (flags & 2u) ? 24u : 20u;
    zero(tcp, header_length); tcp->source = swap16(local_port); tcp->destination = swap16(remote_port); tcp->sequence = swap32(sequence);
    tcp->acknowledgement = swap32((flags & 16u) ? receive_next : 0); tcp->offset = (uint8_t)((header_length / 4u) << 4); tcp->flags = flags; tcp->window = swap16(32768);
    if (header_length == 24) { uint8_t *options = (uint8_t *)tcp + 20; options[0] = 2; options[1] = 4; options[2] = (uint8_t)(TCP_MSS >> 8); options[3] = (uint8_t)TCP_MSS; }
    if (count) copy((uint8_t *)tcp + header_length, payload, count);
    tcp->checksum = transport_checksum(LOCAL_IP, remote_ip, 6, tcp, (uint16_t)(header_length + count));
    ip_header(eth, ip, mac, remote_ip, 6, (uint16_t)(header_length + count)); transmit(frame, (uint16_t)(34u + header_length + count));
}
static void fail(const char *reason) {
    if (state == TCP_RESPONSE) tcp_send(0x14, send_next, NULL, 0);
    set_text(error_text, reason, sizeof(error_text)); state = IDLE; status_text = link_up ? "Connected - request failed" : "Offline - network cable disconnected"; ++generation;
    terminal_log("HTTP_ERROR: "); log_line(error_text);
}
static void done(uint32_t count) {
    response_length = count; response[count] = 0; tcp_send(0x14, send_next, NULL, 0); state = IDLE; status_text = "Connected - page received"; ++generation; if(!quiet_request)log_number("HTTP_DONE: ", count);
}
static bool decimal(const uint8_t *text, uint32_t count, uint32_t *value) {
    uint32_t n = 0; if (!count) return false;
    for (uint32_t i = 0; i < count; ++i) { if (text[i] < '0' || text[i] > '9' || n > 10000000u) return false; n = n * 10u + text[i] - '0'; }
    *value = n; return true;
}
/* Returns 1 when a complete chunk stream is present, 0 when incomplete, -1 when invalid. */
static int chunks(uint32_t offset, uint32_t *body_count, bool decode) {
    uint32_t out = 0;
    while (offset < raw_length) {
        uint32_t size = 0, digits = 0, line = offset;
        while (line < raw_length && raw_response[line] != '\r' && raw_response[line] != ';') {
            uint8_t c = raw_response[line++]; uint32_t digit;
            if (c >= '0' && c <= '9') digit = c - '0'; else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10u; else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10u; else return -1;
            if (++digits > 7u) return -1;
            size = size * 16u + digit;
        }
        if (!digits) return -1;
        while (line < raw_length && raw_response[line] != '\r') ++line;
        if (line + 1u >= raw_length) return 0;
        if (raw_response[line + 1] != '\n') return -1;
        offset = line + 2u;
        if (!size) { /* The zero chunk must include its final CRLF or trailers. */
            if (offset + 1u >= raw_length) return 0;
            if (raw_response[offset] == '\r' && raw_response[offset + 1] == '\n') { *body_count = out; return 1; }
            for (uint32_t i = offset; i + 3u < raw_length; ++i) if (raw_response[i] == '\r' && raw_response[i + 1] == '\n' && raw_response[i + 2] == '\r' && raw_response[i + 3] == '\n') { *body_count = out; return 1; }
            return 0;
        }
        if (size > NETWORK_RESPONSE_MAX - out) return -1;
        if (size > raw_length - offset || raw_length - offset - size < 2u) return 0;
        if (raw_response[offset + size] != '\r' || raw_response[offset + size + 1] != '\n') return -1;
        if (decode) copy(response + out, raw_response + offset, size);
        out += size; offset += size + 2u;
    }
    return 0;
}
static void http_progress(bool closed) {
    uint32_t header = 0;
    for (uint32_t i = 0; i + 3u < raw_length; ++i) if (raw_response[i] == '\r' && raw_response[i + 1] == '\n' && raw_response[i + 2] == '\r' && raw_response[i + 3] == '\n') { header = i + 4u; break; }
    if (!header) { if (closed || raw_length > HTTP_HEADER_MAX) fail("Invalid or oversized HTTP headers"); return; }
    if (header > HTTP_HEADER_MAX || raw_length < 12u || !starts((const char *)raw_response, "HTTP/1.")) { fail("Invalid HTTP response"); return; }
    http_status = 0;
    {
        uint32_t digit = 0;
        while (raw_response[digit] && (uint8_t)raw_response[digit] != ' ' && digit < 64) ++digit;
        if (raw_response[digit] == ' ' && digit + 4 <= header) {
            http_status = (raw_response[digit + 1] - '0') * 100 +
                          (raw_response[digit + 2] - '0') * 10 +
                          (raw_response[digit + 3] - '0');
        }
    }
    bool chunked = false, has_length = false; uint32_t expected = 0, line = 0;
    while (line + 1u < header) {
        uint32_t end = line; while (end + 1u < header && raw_response[end] != '\r') ++end;
        uint32_t colon = line; while (colon < end && raw_response[colon] != ':') ++colon;
        uint32_t value = colon + 1u; while (value < end && (raw_response[value] == ' ' || raw_response[value] == '\t')) ++value;
        uint32_t value_end = end; while (value_end > value && (raw_response[value_end - 1] == ' ' || raw_response[value_end - 1] == '\t')) --value_end;
        if (colon < end && same_case(raw_response + line, colon - line, "content-length")) {
            if (!decimal(raw_response + value, value_end - value, &expected)) { fail("Invalid HTTP content length"); return; } has_length = true;
        }
        if (colon < end && same_case(raw_response + line, colon - line, "location")) {
            uint32_t count = value_end - value;
            if (count >= sizeof(redirect_location)) count = sizeof(redirect_location) - 1u;
            for (uint32_t i = 0; i < count; ++i) redirect_location[i] = (char)raw_response[value + i];
            redirect_location[count] = 0;
        }
        if (colon < end && same_case(raw_response + line, colon - line, "transfer-encoding")) {
            for (uint32_t i = value; i + 7u <= value_end; ++i) if (same_case(raw_response + i, 7, "chunked")) chunked = true;
        }
        line = end + 2u;
    }
    if (chunked) {
        uint32_t count = 0; int result = chunks(header, &count, false);
        if (result < 0) fail("Chunked page is invalid or exceeds 48 KiB");
        else if (result > 0) { chunks(header, &count, true); done(count); }
        else if (closed) fail("Connection closed before page was complete");
        return;
    }
    if (has_length && expected > NETWORK_RESPONSE_MAX) { fail("Page exceeds the 48 KiB browser limit"); return; }
    uint32_t count = raw_length - header;
    if ((has_length && count >= expected) || (closed && !has_length)) {
        if (has_length) count = expected;
        if (count > NETWORK_RESPONSE_MAX) { fail("Page exceeds the 48 KiB browser limit"); return; }
        copy(response, raw_response + header, count); done(count);
    } else if (closed) fail("Connection closed before page was complete");
}
static bool dns_skip_name(const uint8_t *dns, uint32_t size, uint32_t *position) {
    uint32_t p = *position, labels = 0;
    while (p < size && ++labels < 128u) {
        uint8_t n = dns[p++]; if (!n) { *position = p; return true; }
        if ((n & 0xc0u) == 0xc0u) { if (p >= size) return false; *position = p + 1u; return true; }
        if (n > 63 || p + n > size) return false;
        p += n;
    }
    return false;
}
static uint16_t read16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t read32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void dns_receive(const uint8_t *dns, uint32_t size) {
    if (state != DNS_WAIT || size < 12u || read16(dns) != dns_id || !(dns[2] & 0x80u)) return;
    if (dns[3] & 15u) { fail("DNS could not resolve this hostname"); return; }
    uint32_t position = 12, questions = read16(dns + 4), answers = read16(dns + 6);
    for (uint32_t i = 0; i < questions; ++i) { if (!dns_skip_name(dns, size, &position) || position + 4u > size) return; position += 4; }
    for (uint32_t i = 0; i < answers; ++i) {
        if (!dns_skip_name(dns, size, &position) || position + 10u > size) return;
        uint16_t type = read16(dns + position), klass = read16(dns + position + 2), count = read16(dns + position + 8); position += 10;
        if (position + count > size) return;
        if (type == 1 && klass == 1 && count == 4) { remote_ip = read32(dns + position); state = TCP_ARP; attempts = 0; last_send = system_uptime() - 2u; terminal_log("DNS_RESOLVED: "); log_line(host); return; }
        position += count;
    }
    fail("DNS returned no IPv4 address");
}
static void tcp_receive(const Ipv4 *ip, const uint8_t *data, uint32_t size) {
    if (state != TCP_SYN && state != TCP_RESPONSE) return;
    if (swap32(ip->source) != remote_ip || size < 20u) return;
    const Tcp *tcp = (const Tcp *)data;
    if (swap16(tcp->source) != remote_port || swap16(tcp->destination) != local_port) return;
    uint32_t header = (uint32_t)(tcp->offset >> 4) * 4u; if (header < 20u || header > size) return;
    if (transport_checksum(remote_ip, LOCAL_IP, 6, tcp, (uint16_t)size) != 0) return;
    uint32_t sequence = swap32(tcp->sequence), ack = swap32(tcp->acknowledgement); uint8_t flags = tcp->flags;
    if (flags & 4u) { fail("Server refused or reset the connection"); return; }
    if (state == TCP_SYN) {
        if ((flags & 0x12u) != 0x12u || ack != initial_seq + 1u) return;
        receive_next = sequence + 1u; send_next = initial_seq + 1u; state = TCP_RESPONSE; tcp_send(0x10, send_next, NULL, 0);
        tcp_send(0x18, send_next, request, (uint16_t)request_length); send_next += request_length; last_send = system_uptime(); last_activity = last_send; attempts = 1; return;
    }
    last_activity = system_uptime();
    if ((flags & 16u) && ack == send_next) request_acked = true;
    uint32_t count = size - header;
    if (count && sequence != receive_next) { tcp_send(0x10, send_next, NULL, 0); return; }
    if (count) {
        if (count > RAW_SIZE - raw_length) { fail("Page exceeds the browser response limit"); return; }
        copy(raw_response + raw_length, data + header, count); raw_length += count; receive_next += count;
        tcp_send(0x10, send_next, NULL, 0); http_progress(false); if (state == IDLE) return;
    }
    if ((flags & 1u) && sequence + count == receive_next) { ++receive_next; tcp_send(0x10, send_next, NULL, 0); http_progress(true); }
}
static void ethernet_receive(const uint8_t *frame, uint32_t size) {
    if (size < sizeof(Ethernet)) return;
    const Ethernet *eth = (const Ethernet *)frame;
    if (swap16(eth->type) == 0x0806) {
        if (size < 42u) return;
        const Arp *arp = (const Arp *)(frame + 14);
        if (swap16(arp->hardware) != 1 || swap16(arp->protocol) != 0x0800 || arp->hlen != 6 || arp->plen != 4) return;
        learn_mac(swap32(arp->sender_ip), arp->sender_mac);
        if (swap16(arp->operation) == 1 && swap32(arp->target_ip) == LOCAL_IP) arp_send(swap32(arp->sender_ip), arp->sender_mac, true);
        return;
    }
    if (swap16(eth->type) != 0x0800 || size < 34u) return;
    const Ipv4 *ip = (const Ipv4 *)(frame + 14); uint32_t header = (uint32_t)(ip->version & 15u) * 4u, total = swap16(ip->length);
    if ((ip->version >> 4) != 4 || header < 20u || total < header || total > size - 14u || swap32(ip->destination) != LOCAL_IP || (swap16(ip->fragment) & 0x3fffu)) return;
    if (checksum_finish(checksum_sum(0, (const uint8_t *)ip, header)) != 0) return;
    const uint8_t *payload = (const uint8_t *)ip + header; uint32_t count = total - header;
    if (ip->protocol == 6) tcp_receive(ip, payload, count);
    else if (ip->protocol == 17 && count >= 8u && swap32(ip->source) == DNS_IP) {
        const Udp *udp = (const Udp *)payload; uint16_t n = swap16(udp->length);
        if (swap16(udp->source) == 53 && swap16(udp->destination) == local_port && n >= 8u && n <= count && (!udp->checksum || transport_checksum(DNS_IP, LOCAL_IP, 17, udp, n) == 0)) dns_receive(payload + 8, n - 8u);
    }
}
void network_init(void) {
    present = false; link_up = false; state = IDLE;
    for (uint32_t bus = 0; bus < 256u && !present; ++bus) for (uint8_t dev = 0; dev < 32u && !present; ++dev) {
        uint32_t first = pci_read((uint8_t)bus, dev, 0, 0); if ((first & 65535u) == 65535u) continue;
        uint8_t functions = (pci_read((uint8_t)bus, dev, 0, 0x0c) & 0x00800000u) ? 8 : 1;
        for (uint8_t fn = 0; fn < functions; ++fn) {
            if (pci_read((uint8_t)bus, dev, fn, 0) != 0x813910ecu) continue;
            uint32_t bar = pci_read((uint8_t)bus, dev, fn, 0x10); if (!(bar & 1u) || (bar & ~3u) > 65535u) continue;
            io_base = (uint16_t)(bar & ~3u); uint32_t command = pci_read((uint8_t)bus, dev, fn, 4); pci_write((uint8_t)bus, dev, fn, 4, (command & 65535u) | 5u); present = true; break;
        }
    }
    if (!present) { log_line("NETWORK_OFFLINE: no RTL8139 adapter"); return; }
    out8((uint16_t)(io_base + 0x52), 0); out8((uint16_t)(io_base + 0x37), 0x10);
    uint32_t waits = 1000000u; while ((in8((uint16_t)(io_base + 0x37)) & 0x10u) && --waits) { }
    if (!waits) { present = false; status_text = "Offline: network adapter reset timed out"; log_line("NETWORK_OFFLINE: adapter reset timeout"); return; }
    for (uint16_t i = 0; i < 6; ++i) own_mac[i] = in8((uint16_t)(io_base + i));
    zero(receive_ring, sizeof(receive_ring)); zero(arp_cache, sizeof(arp_cache)); receive_offset = 0; transmit_slot = 0;
    out16((uint16_t)(io_base + 0x3c), 0); out16((uint16_t)(io_base + 0x3e), 0xffff);
    out32((uint16_t)(io_base + 0x30), (uint32_t)(uintptr_t)receive_ring);
    for (uint16_t i = 0; i < 4; ++i) out32((uint16_t)(io_base + 0x20u + i * 4u), (uint32_t)(uintptr_t)transmit_buffer[i]);
    out32((uint16_t)(io_base + 0x44), 0x0000e78au); /* 8 KiB ring, wrap, physical/broadcast, unlimited DMA. */
    out32((uint16_t)(io_base + 0x40), 0x03000700u); out8((uint16_t)(io_base + 0x37), 0x0c);
    out16((uint16_t)(io_base + 0x38), (uint16_t)-16);
    link_up = !(in8((uint16_t)(io_base + 0x58)) & 4u); status_text = link_up ? "Connected - QEMU user network" : "Offline - network cable disconnected";
    log_line("NETWORK_READY: 10.0.2.15"); arp_send(GATEWAY_IP, NULL, false);
}
void network_poll(void) {
    if (!present) return;
    link_up = !(in8((uint16_t)(io_base + 0x58)) & 4u);
    for (unsigned int packets = 0; packets < 32u && !(in8((uint16_t)(io_base + 0x37)) & 1u); ++packets) {
        __asm__ volatile("" : : : "memory");
        uint8_t *record = receive_ring + receive_offset; uint16_t flags = (uint16_t)(record[0] | ((uint16_t)record[1] << 8)); uint16_t count = (uint16_t)(record[2] | ((uint16_t)record[3] << 8));
        if (count == 0xfff0u) break;
        if (!(flags & 1u) || count < 4u || count > 1518u) { /* Drop corrupt/overflowed RX data and resume at producer offset. */
            receive_offset = (uint16_t)((in16((uint16_t)(io_base + 0x3a)) + 3u) & ~3u); receive_offset %= RX_SIZE;
            out16((uint16_t)(io_base + 0x38), (uint16_t)(receive_offset - 16u)); break;
        }
        ++rx_packets; ethernet_receive(record + 4, count - 4u);
        receive_offset = (uint16_t)((receive_offset + count + 7u) & ~3u); receive_offset %= RX_SIZE;
        out16((uint16_t)(io_base + 0x38), (uint16_t)(receive_offset - 16u));
    }
    uint16_t interrupts = in16((uint16_t)(io_base + 0x3e));
    if (interrupts & 0x50u) { receive_offset = (uint16_t)((in16((uint16_t)(io_base + 0x3a)) + 3u) & ~3u); receive_offset %= RX_SIZE; out16((uint16_t)(io_base + 0x38), (uint16_t)(receive_offset - 16u)); }
    if (interrupts) out16((uint16_t)(io_base + 0x3e), interrupts);
    if (state == IDLE) return;
    uint32_t now = system_uptime();
    if (!link_up) { fail("Network cable is disconnected"); return; }
    if (now - started >= 13u) { fail("Request timed out; check the host network or HTTPS bridge"); return; }
    if (state == DNS_ARP || state == TCP_ARP) {
        uint32_t target = state == DNS_ARP ? DNS_IP : remote_ip;
        if (find_mac(target)) { state = state == DNS_ARP ? DNS_WAIT : TCP_SYN; attempts = 0; last_send = now - 2u; }
        else if (now - last_send >= 1u) { arp_send(route(target), NULL, false); last_send = now; if (++attempts >= 5u) fail("Gateway did not answer ARP; check QEMU network settings"); }
    }
    if (state == DNS_WAIT && now - last_send >= 2u) { dns_send(); last_send = now; if (++attempts > 4u) fail("DNS server did not answer"); }
    if (state == TCP_SYN && now - last_send >= 2u) { tcp_send(2, initial_seq, NULL, 0); last_send = now; if (++attempts > 4u) fail(remote_port == 8088 ? "HTTPS bridge is unavailable; start the Windows bridge" : "Web server did not answer"); }
    if (state == TCP_RESPONSE && !request_acked && now - last_send >= 2u) { tcp_send(0x18, initial_seq + 1u, request, (uint16_t)request_length); last_send = now; if (++attempts > 4u) fail("Web request was not acknowledged"); }
    if (state == TCP_RESPONSE && now - last_activity >= 8u) fail("Web server stopped responding");
}
static bool parse_ip(const char *text, uint32_t *result) {
    uint32_t ip = 0, p = 0;
    for (unsigned int part = 0; part < 4; ++part) {
        uint32_t n = 0, digits = 0; while (text[p] >= '0' && text[p] <= '9') { n = n * 10u + text[p++] - '0'; if (++digits > 3u || n > 255u) return false; }
        if (!digits) return false;
        ip = (ip << 8) | n;
        if (part < 3) { if (text[p++] != '.') return false; } else if (text[p]) return false;
    }
    *result = ip; return true;
}
static bool append(char *buffer, uint32_t *used, uint32_t capacity, const char *text) {
    while (*text) { if (*used + 1u >= capacity) return false; buffer[(*used)++] = *text++; } buffer[*used] = 0; return true;
}
bool network_fetch(const char *url) {
    if (state != IDLE) return false;
    response_length = 0; response[0] = 0; error_text[0] = 0; raw_length = 0; request_length = 0; request_acked = false;
    http_status = 0; redirect_location[0] = 0;
    if (!present || !link_up) { fail("No network connection; enable the RTL8139 adapter in QEMU"); return false; }
    if (!url || length(url) > 1000u) { fail("URL is empty or too long"); return false; }
    bool secure = starts(url, "https://"); const char *authority;
    if (secure) authority = url + 8; else if (starts(url, "http://")) authority = url + 7; else { fail("Use an http:// or https:// address"); return false; }
    uint32_t n = 0; while (authority[n] && authority[n] != '/' && authority[n] != ':' && authority[n] != '?' && authority[n] != '#') {
        uint8_t c = (uint8_t)authority[n]; if (n >= sizeof(host) - 1u || c <= ' ' || c >= 127u || c == '@' || c == '[' || c == '\\') { fail("Invalid hostname"); return false; } host[n] = (char)c; ++n;
    }
    if (!n) { fail("Enter a hostname"); return false; } host[n] = 0; const char *path = authority + n; remote_port = secure ? 443 : 80;
    if (*path == ':') { ++path; uint32_t port = 0, digits = 0; while (*path >= '0' && *path <= '9') { port = port * 10u + (uint32_t)(*path++ - '0'); if (++digits > 5u || port > 65535u) break; } if (!digits || !port || port > 65535u || (*path && *path != '/' && *path != '?' && *path != '#')) { fail("Invalid port number"); return false; } remote_port = (uint16_t)port; }
    if (secure) {
        remote_ip = GATEWAY_IP; remote_port = 8088;
        append(request, &request_length, sizeof(request), "GET /fetch?url=");
        const char *hex = "0123456789ABCDEF";
        for (uint32_t i = 0; url[i] && url[i] != '#'; ++i) {
            uint8_t c = (uint8_t)url[i]; char encoded[4] = { '%', hex[c >> 4], hex[c & 15u], 0 };
            bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
            char literal[2] = { (char)c, 0 }; if (!append(request, &request_length, sizeof(request), safe ? literal : encoded)) { fail("URL is too long for the HTTPS bridge"); return false; }
        }
        if (!append(request, &request_length, sizeof(request), " HTTP/1.0\r\nHost: 10.0.2.2:8088\r\nConnection: close\r\n\r\n")) { fail("URL is too long"); return false; }
        state = TCP_ARP;
    } else {
        append(request, &request_length, sizeof(request), "GET ");
        if (*path != '/') append(request, &request_length, sizeof(request), "/");
        while (*path && *path != '#') { uint8_t c = (uint8_t)*path++; if (c <= ' ' || c >= 127u || request_length + 1u >= sizeof(request)) { fail("URL contains invalid characters or is too long"); return false; } request[request_length++] = (char)c; }
        request[request_length] = 0;
        bool ok = append(request, &request_length, sizeof(request), " HTTP/1.0\r\nHost: ") && append(request, &request_length, sizeof(request), host);
        if (remote_port != 80) { char port[8], digits[5]; uint32_t count = 0, p = remote_port, k = 0; port[k++] = ':'; do { digits[count++] = (char)('0' + p % 10u); p /= 10u; } while (p); while (count) port[k++] = digits[--count]; port[k] = 0; ok = ok && append(request, &request_length, sizeof(request), port); }
        ok = ok && append(request, &request_length, sizeof(request), "\r\nUser-Agent: NovaOS/0.4\r\nAccept: text/html,text/plain\r\nConnection: close\r\n\r\n");
        if (!ok) { fail("URL is too long"); return false; }
        state = parse_ip(host, &remote_ip) ? TCP_ARP : DNS_ARP;
    }
    local_port = (uint16_t)(49152u + (generation & 16383u)); dns_id = (uint16_t)(0x4e00u + generation); initial_seq += 0x10001u; send_next = initial_seq; receive_next = 0;
    started = system_uptime(); last_activity = started; last_send = started - 2u; attempts = 0; status_text = secure ? "Fetching via the host HTTPS bridge" : "Fetching HTTP page"; quiet_request = strstr(url,"http://10.0.2.2:8089/")==url; if(!quiet_request){terminal_log("HTTP_FETCH: "); log_line(url);} return true;
}
bool network_present(void) { return present; }
bool network_link_up(void) { return present && link_up; }
const char *network_status(void) { return present && !link_up ? "Offline - network cable disconnected" : status_text; }
const char *network_ip(void) { return present ? "10.0.2.15" : "0.0.0.0"; }
const char *network_gateway(void) { return "10.0.2.2"; }
const char *network_dns(void) { return "10.0.2.3"; }
uint32_t network_rx_packets(void) { return rx_packets; }
uint32_t network_tx_packets(void) { return tx_packets; }
bool network_busy(void) { return state != IDLE; }
const char *network_response(void) { return response; }
const char *network_error(void) { return error_text; }
uint32_t network_response_length(void) { return response_length; }
int network_status_code(void) { return http_status; }
const char *network_redirect(void) { return redirect_location; }
uint32_t network_generation(void) { return generation; }

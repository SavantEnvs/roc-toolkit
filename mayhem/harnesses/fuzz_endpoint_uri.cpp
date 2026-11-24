// Fuzz the endpoint URI parser/formatter of the public API (Ragel endpoint_uri_parse.rl,
// percent-decoding/encoding in pct.cpp, protocol map, URI validation and formatting).
//
// Input: byte 0 selects the operation, the rest is a C string (NUL-terminated copy).
//   0: roc_endpoint_set_uri(), then read back uri/host/resource/protocol/port
//   1: set a protocol + host, then roc_endpoint_set_host(input) and get_uri
//   2: set rtsp://host:1/, then roc_endpoint_set_resource(input) and get_uri
// No state is kept across inputs.
#include <roc/endpoint.h>
#include <roc/log.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    roc_log_set_level(ROC_LOG_NONE);
    return 0;
}

static void read_back(roc_endpoint* ep) {
    char buf[1024];
    size_t sz = sizeof(buf);
    (void)roc_endpoint_get_uri(ep, buf, &sz);
    sz = sizeof(buf);
    (void)roc_endpoint_get_host(ep, buf, &sz);
    sz = sizeof(buf);
    (void)roc_endpoint_get_resource(ep, buf, &sz);
    roc_protocol proto;
    (void)roc_endpoint_get_protocol(ep, &proto);
    int port;
    (void)roc_endpoint_get_port(ep, &port);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) {
        return -1;
    }
    const uint8_t op = data[0] % 3;
    char* str = (char*)malloc(size);
    memcpy(str, data + 1, size - 1);
    str[size - 1] = '\0';

    roc_endpoint* ep = NULL;
    if (roc_endpoint_allocate(&ep) != 0) {
        abort();
    }
    switch (op) {
    case 0:
        if (roc_endpoint_set_uri(ep, str) == 0) {
            read_back(ep);
        }
        break;
    case 1:
        roc_endpoint_set_protocol(ep, ROC_PROTO_RTP);
        roc_endpoint_set_port(ep, 10001);
        if (roc_endpoint_set_host(ep, str) == 0) {
            read_back(ep);
        }
        break;
    case 2:
        roc_endpoint_set_uri(ep, "rtsp://localhost:1/");
        if (roc_endpoint_set_resource(ep, str) == 0) {
            read_back(ep);
        }
        break;
    }
    roc_endpoint_deallocate(ep);
    free(str);
    return 0;
}

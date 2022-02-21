/*
 * tcp proxy server
 *
 * @build:        make clean && make examples WITH_OPENSSL=yes
 * @http_server:  bin/httpd -s restart -d
 * @proxy_server: bin/tcp_proxy_server 1080 172.29.117.46:3003 83048a0632f3031e ./.zt
 *                bin/tcp_proxy_server 1080 127.0.0.1:8443
 *                bin/tcp_proxy_server 1080 www.baidu.com
 *                bin/tcp_proxy_server 1080 www.baidu.com:443
 * @client:       bin/curl -v 127.0.0.1:1080
 *                bin/nc 127.0.0.1 1080
 *                > GET / HTTP/1.1
 *                > Connection: close
 *                > [Enter]
 *                > GET / HTTP/1.1
 *                > Connection: keep-alive
 *                > [Enter]
 */

#include "hevent.h"
#include "hloop.h"
#include "hsocket.h"
#include "ZeroTierSockets.h"

#include "assert.h"

static char proxy_host[64] = "0.0.0.0";
static int  proxy_port = 1080;
static int  proxy_ssl = 0;

static char backend_host[64] = "127.0.0.1";
static int  backend_port = 80;
static int  backend_ssl = 0;

// hloop_create_tcp_server -> on_accept -> hio_setup_tcp_upstream

#define ZTS_DELAY 20

void zt_boot(char* storage_path) {
    int err = ZTS_ERR_OK;
    // Initialize node
    if ((err = zts_init_from_storage(storage_path)) != ZTS_ERR_OK) {
        printf("Unable to start service, error = %d. Exiting.\n", err);
        exit(1);
    }
    // Start node
    if ((err = zts_node_start()) != ZTS_ERR_OK) {
        printf("Unable to start service, error = %d. Exiting.\n", err);
        exit(1);
    }
    printf("Waiting for node to come online\n");
    while (! zts_node_is_online())
        zts_util_delay(ZTS_DELAY);
    printf("Public identity (node ID) is %llx\n", (long long int)zts_node_get_id());
}

void zt_join(long long int net_id, char* remote_addr) {
    int err = ZTS_ERR_OK;
    // Join network
    printf("Joining network %llx\n", net_id);
    if (zts_net_join(net_id) != ZTS_ERR_OK) {
        printf("Unable to join network. Exiting.\n");
        exit(1);
    }
    printf("Don't forget to authorize this device in my.zerotier.com or the web API!\n");
    printf("Waiting for join to complete\n");
    while (! zts_net_transport_is_ready(net_id))
        zts_util_delay(ZTS_DELAY);
    // Get assigned address (of the family type we care about)
    int family = zts_util_get_ip_family(remote_addr);
    printf("Waiting for address assignment from network\n");
    while (! (err = zts_addr_is_assigned(net_id, family)))
        zts_util_delay(ZTS_DELAY);
    char ipstr[ZTS_IP_MAX_STR_LEN] = { 0 };
    zts_addr_get_str(net_id, family, ipstr, ZTS_IP_MAX_STR_LEN);
    printf("IP address on network %llx is %s\n", net_id, ipstr);
}

hio_t* hio_setup_tcp_zts_upstream(hio_t* io, const char* host, int port, int ssl) {
    printf("hio_setup_tcp_zts_upstream\n");
    hio_t* upstream_io = hio_create_socket(io->loop, host, port, HIO_TYPE_TCP|HIO_TYPE_ZTS, HIO_CLIENT_SIDE);
    if (upstream_io == NULL) return NULL;
    if (ssl) hio_enable_ssl(upstream_io);
    hio_setup_upstream(io, upstream_io);
    hio_setcb_read(io, hio_write_upstream);
    hio_setcb_read(upstream_io, hio_write_upstream);
    hio_setcb_close(io, hio_close_upstream);
    hio_setcb_close(upstream_io, hio_close_upstream);
    hio_setcb_connect(upstream_io, hio_read_upstream);
    hio_connect(upstream_io);
    return upstream_io;
}

static void on_accept(hio_t* io) {
    /*
    printf("on_accept connfd=%d\n", hio_fd(io));
    char localaddrstr[SOCKADDR_STRLEN] = {0};
    char peeraddrstr[SOCKADDR_STRLEN] = {0};
    printf("accept connfd=%d [%s] <= [%s]\n", hio_fd(io),
            SOCKADDR_STR(hio_localaddr(io), localaddrstr),
            SOCKADDR_STR(hio_peeraddr(io), peeraddrstr));
    */

    if (backend_port % 1000 == 443) backend_ssl = 1;
    hio_setup_tcp_zts_upstream(io, backend_host, backend_port, backend_ssl);
    // hio_setup_tcp_upstream(io, backend_host, backend_port, backend_ssl);
}

int main(int argc, char** argv) {

    assert((sizeof struct sockaddr_in6) == (sizeof struct zts_sockaddr_in6));
    assert((sizeof struct sockaddr_in) == (sizeof struct zts_sockaddr_in));
    assert((sizeof struct sockaddr) == (sizeof struct zts_sockaddr));
    assert(ZTS_SOCK_STREAM == SOCK_STREAM);
    assert(ZTS_SOCK_DGRAM == SOCK_DGRAM);
    assert(ZTS_SOCK_RAW == SOCK_RAW);
    assert(ZTS_AF_UNSPEC == AF_UNSPEC);
    assert(ZTS_AF_INET == AF_INET);
    assert(ZTS_AF_INET6 == AF_INET6);
    assert(ZTS_SOL_SOCKET == SOL_SOCKET);
    assert(ZTS_SO_REUSEADDR == SO_REUSEADDR);
    puts("ok");

    if (argc < 5) {
        printf("Usage: %s proxy_port backend_host:backend_port net_id id_storage_path\n", argv[0]);
        return -10;
    }
    proxy_port = atoi(argv[1]);
    char* pos = strchr(argv[2], ':');
    if (pos) {
        int len = pos - argv[2];
        if (len > 0) {
            memcpy(backend_host, argv[2], len);
            backend_host[len] = '\0';
        }
        backend_port = atoi(pos + 1);
    } else {
        strncpy(backend_host, argv[2], sizeof(backend_host));
    }
    if (backend_port == 0)
        backend_port = 80;

    long long int net_id = strtoull(argv[3], NULL, 16);   // At least 64 bits
    char* storage_path = argv[4];
    printf("%s:%d zts_proxy %s:%d %s\n", proxy_host, proxy_port, backend_host, backend_port, argv[3]);

    zt_boot(storage_path);
    zt_join(net_id, backend_host);

    hloop_t* loop = hloop_new(0);
    hio_t* listenio = hloop_create_tcp_server(loop, proxy_host, proxy_port, on_accept);
    if (listenio == NULL) {
        return -20;
    }
    printf("listenfd=%d\n", hio_fd(listenio));
    if (proxy_ssl) {
        hio_enable_ssl(listenio);
    }
    hloop_run(loop);
    hloop_free(&loop);
    return 0;
}

#include "vpnworker.h"

extern "C"  {
#include "openfortivpn/src/config.h"
#include "openfortivpn/src/log.h"
#include "openfortivpn/src/tunnel.h"
#include "openfortivpn/src/http.h"

#include <unistd.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <openssl/err.h>
#ifndef __APPLE__
#include <pty.h>
#else
#include <util.h>
#endif
#include <signal.h>
#include <sys/wait.h>
#include <assert.h>
}

#include <QDebug>
#include <QCoreApplication>

// -------------------
// Included from tunnel.c
// -------------------

static int on_ppp_if_up(struct tunnel *tunnel)
{
    log_info("Interface %s is UP.\n", tunnel->ppp_iface);

    if (tunnel->config->set_routes) {
        int ret;

        log_info("Setting new routes...\n");

        ret = ipv4_set_tunnel_routes(tunnel);

        if (ret != 0) {
            log_warn("Adding route table is incomplete. "
                     "Please check route table.\n");
        }
    }

    if (tunnel->config->set_dns) {
        log_info("Adding VPN nameservers...\n");
        ipv4_add_nameservers_to_resolv_conf(tunnel);
    }

    log_info("Tunnel is up and running.\n");

    return 0;
}

static int on_ppp_if_down(struct tunnel *tunnel)
{
    log_info("Setting ppp interface down.\n");

    if (tunnel->config->set_routes) {
        log_info("Restoring routes...\n");
        ipv4_restore_routes(tunnel);
    }

    if (tunnel->config->set_dns) {
        log_info("Removing VPN nameservers...\n");
        ipv4_del_nameservers_from_resolv_conf(tunnel);
    }

    return 0;
}

static int pppd_run(struct tunnel *tunnel)
{
    pid_t pid;
    int amaster;
#ifndef __APPLE__
    struct termios termp;
#endif

    static const char pppd_path[] = "/usr/sbin/pppd";

    if (access(pppd_path, F_OK) != 0) {
        log_error("%s: %s.\n", pppd_path, strerror(errno));
        return 1;
    }

#ifndef __APPLE__
    termp.c_cflag = B9600;
    termp.c_cc[VTIME] = 0;
    termp.c_cc[VMIN] = 1;

    pid = forkpty(&amaster, NULL, &termp, NULL);
#else
    pid = forkpty(&amaster, NULL, NULL, NULL);
#endif

    if (pid == -1) {
        log_error("forkpty: %s\n", strerror(errno));
        return 1;
    } else if (pid == 0) { // child process
        static const char *args[] = {
            pppd_path,
            "38400", // speed
            ":1.1.1.1", // <local_IP_address>:<remote_IP_address>
            "noipdefault",
            "noaccomp",
            "noauth",
            "default-asyncmap",
            "nopcomp",
            "receive-all",
            "nodefaultroute",
            "nodetach",
            "lcp-max-configure", "40",
            "mru", "1354",
            NULL, // "usepeerdns"
            NULL, NULL, NULL, // "debug", "logfile", pppd_log
            NULL, NULL, // "plugin", pppd_plugin
            NULL, NULL, // "ipparam", pppd_ipparam
            NULL, NULL, // "ifname", pppd_ifname
            NULL // terminal null pointer required by execvp()
        };

        // Dynamically get first NULL pointer so that changes of
        // args above don't need code changes here
        int i = ARRAY_SIZE(args) - 1;
        while (args[i] == NULL)
            i--;
        i++;

        /*
         * Coverity detected a defect:
         *  CID 196857: Out-of-bounds write (OVERRUN)
         * It is actually a false positive. Because 'args' is not
         * constant, Coverity is unable to infer that the NULL
         * elements 'args' has been initialized with shall still
         * be present when initializing 'i' in the above loop.
         */
        if (tunnel->config->pppd_use_peerdns)
            args[i++] = "usepeerdns";
        if (tunnel->config->pppd_log) {
            args[i++] = "debug";
            args[i++] = "logfile";
            args[i++] = tunnel->config->pppd_log;
        }
        if (tunnel->config->pppd_plugin) {
            args[i++] = "plugin";
            args[i++] = tunnel->config->pppd_plugin;
        }
        if (tunnel->config->pppd_ipparam) {
            args[i++] = "ipparam";
            args[i++] = tunnel->config->pppd_ipparam;
        }
        if (tunnel->config->pppd_ifname) {
            args[i++] = "ifname";
            args[i++] = tunnel->config->pppd_ifname;
        }
        // Assert that we didn't use up all NULL pointers above
        assert(i < ARRAY_SIZE(args));

        close(tunnel->ssl_socket);
        execv(args[0], (char *const *)args);
        /*
         * The following call to fprintf() doesn't work, probably
         * because of the prior call to forkpty().
         * TODO: print a meaningful message using strerror(errno)
         */
        fprintf(stderr, "execvp: %s\n", strerror(errno));
        _exit(EXIT_FAILURE);
    }

    // Set non-blocking
    int flags = fcntl(amaster, F_GETFL, 0);
    if (flags == -1)
        flags = 0;
    if (fcntl(amaster, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_error("fcntl: %s\n", strerror(errno));
        return 1;
    }

    tunnel->pppd_pid = pid;
    tunnel->pppd_pty = amaster;

    return 0;
}

static const char * const pppd_message[] = {
    "Returned an unknown exit status", // fall back
    "Has detached, or otherwise the connection was successfully"
    " established and terminated at the peer's request.",
    "An immediately fatal error of some kind occurred, such as an"
    " essential system call failing, or running out of virtual memory.",
    "An error was detected in processing the options given, such as two"
    " mutually exclusive options being used.",
    "Is not setuid-root and the invoking user is not root.",
    "The kernel does not support PPP, for example, the PPP kernel driver"
    " is not included or cannot be loaded.",
    "Terminated because it was sent a SIGINT, SIGTERM or SIGHUP signal.",
    "The serial port could not be locked.",
    "The serial port could not be opened.",
    "The connect script failed (returned a non-zero exit status).",
    "The command specified as the argument to the pty option"
    " could not be run.",
    "The PPP negotiation failed, that is, it didn't reach the point"
    " where at least one network protocol (e.g. IP) was running.",
    "The peer system failed (or refused) to authenticate itself.",
    "The link was established successfully and terminated because"
    " it was idle.",
    "The link was established successfully and terminated because the"
    " connect time limit was reached.",
    "Callback was negotiated and an incoming call should arrive shortly.",
    "The link was terminated because the peer is not responding to echo"
    " requests.", // emitted when exiting normally
    "The link was terminated by the modem hanging up.",
    "The PPP negotiation failed because serial loopback was detected.",
    "The init script failed (returned a non-zero exit status).",
    "We failed to authenticate ourselves to the peer."
};

static int pppd_terminate(struct tunnel *tunnel)
{
    close(tunnel->pppd_pty);

    log_debug("Waiting for pppd to exit...\n");
    int status;
    if (waitpid(tunnel->pppd_pid, &status, 0) == -1) {
        log_error("waitpid: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(status)) {
        int exit_status = WEXITSTATUS(status);
        log_debug("waitpid: pppd exit status code %d\n", exit_status);
        if (exit_status) {
            size_t len_pppd_message = ARRAY_SIZE(pppd_message);
            if (exit_status >= len_pppd_message)
                exit_status = 0;
            if (exit_status != 16) // emitted when exiting normally
                log_error("pppd: %s\n", pppd_message[exit_status]);
        }
    } else if (WIFSIGNALED(status)) {
        int signal_number = WTERMSIG(status);
        log_debug("waitpid: pppd terminated by signal %d\n",
                  signal_number);
        log_error("pppd: terminated by signal: %s\n",
                  strsignal(signal_number));
    }

    return 0;
}

static int get_gateway_host_ip(struct tunnel *tunnel)
{
    struct addrinfo hints;
    hints.ai_family = AF_INET;
    struct addrinfo *result = NULL;

    int ret = getaddrinfo(tunnel->config->gateway_host, NULL, &hints, &result);

    if (ret) {
        if (ret == EAI_SYSTEM)
            log_error("getaddrinfo: %s\n", strerror(errno));
        else
            log_error("getaddrinfo: %s\n", gai_strerror(ret));
        return 1;
    }

    tunnel->config->gateway_ip = ((struct sockaddr_in *)
                                  result->ai_addr)->sin_addr;
    freeaddrinfo(result);

    setenv("VPN_GATEWAY", inet_ntoa(tunnel->config->gateway_ip), 0);

    return 0;
}

vpnWorker::vpnWorker(QObject *parent) : QObject(parent)
{
    ptr_tunnel = 0;
}

void vpnWorker::setConfig(vpnProfile c)
{
    vpnConfig = c;
}

void vpnWorker::updateStatus(vpnClientConnection::connectionStatus status)
{
    emit statusChanged(status);
    QCoreApplication::processEvents();
}

void vpnWorker::process()
{
    qDebug() << "vpnWorker::process::slot";

    struct vpn_config config = {};

    //memset(&config, 0, sizeof (config));
    init_logging();
    //init_vpn_config(&config);
    strncpy(config.gateway_host, vpnConfig.gateway_host.toStdString().c_str(), FIELD_SIZE);
    config.gateway_host[FIELD_SIZE] = '\0';
    config.gateway_port = vpnConfig.gateway_port;
    strncpy(config.username, vpnConfig.username.toStdString().c_str(), FIELD_SIZE);
    config.username[FIELD_SIZE] = '\0';
    strncpy(config.password, vpnConfig.password.toStdString().c_str(), FIELD_SIZE);
    config.password[FIELD_SIZE] = '\0';
    config.set_routes = (vpnConfig.set_routes) ? 1 : 0;

    if(!vpnConfig.user_cert.isEmpty() && !vpnConfig.user_key.isEmpty())
    {
        config.user_cert = strdup(vpnConfig.user_cert.toStdString().c_str());
        config.user_key = strdup(vpnConfig.user_key.toStdString().c_str());
        if(!vpnConfig.trusted_cert.isEmpty())
            add_trusted_cert(&config, vpnConfig.trusted_cert.toStdString().c_str());
    }

    if(!vpnConfig.realm.isEmpty())
        strncpy(config.realm, vpnConfig.user_key.toStdString().c_str(), FIELD_SIZE);

    config.set_dns = (vpnConfig.set_dns) ? 1 : 0;
    config.verify_cert = (vpnConfig.verify_cert) ? 1 : 0;
    config.insecure_ssl = (vpnConfig.insecure_ssl) ? 1 : 0;
    config.pppd_use_peerdns = (vpnConfig.pppd_no_peerdns) ? 0 : 1;

    if(vpnConfig.debug)
        increase_verbosity();

    int ret;
    struct tunnel tunnel;

    memset(&tunnel, 0, sizeof(tunnel));
#ifdef __APPLE__
    // initialize value
    tunnel.ipv4.split_routes = 0;
#endif
    tunnel.config = &config;
    tunnel.on_ppp_if_up = on_ppp_if_up;
    tunnel.on_ppp_if_down = on_ppp_if_down;
    tunnel.ipv4.ns1_addr.s_addr = 0;
    tunnel.ipv4.ns2_addr.s_addr = 0;
    tunnel.ssl_handle = NULL;
    tunnel.ssl_context = NULL;

    tunnel.state = STATE_CONNECTING;
    ptr_tunnel = &tunnel;

    // Step 0: get gateway host IP
    ret = get_gateway_host_ip(&tunnel);
    if (ret)
        goto err_tunnel;

    // Step 1: open a SSL connection to the gateway
    ret = ssl_connect(&tunnel);
    if (ret)
        goto err_tunnel;
    log_info("Connected to gateway.\n");

    // Step 2: connect to the HTTP interface and authenticate to get a
    // cookie
    ret = auth_log_in(&tunnel);
    if (ret != 1) {
        log_error("Could not authenticate to gateway (%s).\n",
                  err_http_str(ret));
        ret = 1;
        goto err_tunnel;
    }
    log_info("Authenticated.\n");
    log_debug("Cookie: %s\n", config.cookie);

    ret = auth_request_vpn_allocation(&tunnel);
    if (ret != 1) {
        log_error("VPN allocation request failed (%s).\n",
                  err_http_str(ret));
        ret = 1;
        goto err_tunnel;
    }
    log_info("Remote gateway has allocated a VPN.\n");

    ret = ssl_connect(&tunnel);
    if (ret)
        goto err_tunnel;

    // Step 3: get configuration
    ret = auth_get_config(&tunnel);
    if (ret != 1) {
        log_error("Could not get VPN configuration (%s).\n",
                  err_http_str(ret));
        ret = 1;
        goto err_tunnel;
    }

    // Step 4: run a pppd process
    ret = pppd_run(&tunnel);
    if (ret)
        goto err_tunnel;

    // Step 5: ask gateway to start tunneling
    ret = http_send(&tunnel,
                    "GET /remote/sslvpn-tunnel HTTP/1.1\n"
                    "Host: sslvpn\n"
                    "Cookie: %s\n\n%c",
                    tunnel.config->cookie, '\0');
    if (ret != 1) {
        log_error("Could not start tunnel (%s).\n", err_http_str(ret));
        ret = 1;
        goto err_start_tunnel;
    }

    tunnel.state = STATE_CONNECTING;
    ret = 0;

    log_info("Custom: %s.\n", tunnel.config->gateway_host);
    // Step 6: perform io between pppd and the gateway, while tunnel is up
    io_loop(&tunnel);

    log_info("Custom2: %s.\n", tunnel.ipv4.ip_addr);

    if (tunnel.state == STATE_UP)
        if (tunnel.on_ppp_if_down != NULL)
            tunnel.on_ppp_if_down(&tunnel);

    tunnel.state = STATE_DISCONNECTING;
    emit finished();

err_start_tunnel:
    pppd_terminate(&tunnel);
    log_info("Terminated pppd.\n");
    emit finished();
err_tunnel:
    log_info("Closed connection to gateway.\n");
    tunnel.state = STATE_DOWN;

    if (ssl_connect(&tunnel)) {
        log_info("Could not log out.\n");
    } else {
        auth_log_out(&tunnel);
        log_info("Logged out.\n");
    }

    // explicitly free the buffer allocated for split routes of the ipv4 config
    if (tunnel.ipv4.split_rt != NULL) {
        free(tunnel.ipv4.split_rt);
        tunnel.ipv4.split_rt = NULL;
    }

    emit finished();
}

void vpnWorker::end()
{
    ptr_tunnel->on_ppp_if_down(ptr_tunnel);
}

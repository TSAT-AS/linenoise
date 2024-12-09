#include <functional>
#include <stdexcept>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include "linenoise.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>

class Session
{
public:
    Session(boost::asio::io_context& io_context, int master_fd, int slave_fd)
    : master_side_(io_context, master_fd)
    , input_buffer_(1024)
    , output_buffer_(1024)
    , m_master_fd(master_fd)
    , m_slave_fd(slave_fd)
    , m_tty_listener([](const char *, size_t){ syslog(LOG_INFO, "TTY LISTENER NOT SET"); })
    , m_cmd_listener([](std::string){ syslog(LOG_INFO, "CMD LISTENER NOT SET"); })
    {}

    ~Session()
    {
        stop();
    }

    void setTtyOutputListener(std::function<void(const char *, size_t)> listener)
    {
        m_tty_listener = listener;
    }

    void setCmdListener(std::function<void(std::string)> listener)
    {
        m_cmd_listener = listener;
    }

    void do_read()
    {
        syslog(LOG_ERR, "do_read()");
        boost::asio::async_read(master_side_, input_buffer_, boost::asio::transfer_at_least(1),
        [this](boost::system::error_code ec, std::size_t length)
        {
            if (!ec)
            {
                syslog(LOG_INFO, "Master side read %zu bytes", length);
                const char *data = boost::asio::buffer_cast<const char *>(input_buffer_.data());
                m_tty_listener(data, length);
                input_buffer_.consume(length);
                do_read();
            }
            else
            {
                syslog(LOG_ERR, "ERROR: master side read error");
                // close();
                m_should_run = false;
            }
        });
    }

    void start()
    {
        syslog(LOG_INFO, "Starting session");

        do_read();

        m_should_run = true;

        m_pty_slave_worker =
            std::thread([this]()
            {
                struct linenoiseState ls{};
                char buf[1024];
                while(m_should_run)
                {

                    int res = linenoiseEditStart(&ls, m_slave_fd, m_slave_fd, buf, sizeof(buf), "hello> ");
                    if(res == -1)
                    {
                        syslog(LOG_ERR, "linenoiseEditStart error");
                        m_should_run = false;
                        continue;
                    }

                    while(1)
                    {
                        syslog(LOG_ERR, "linenoiseEditStart success");

                        char *line = linenoiseEditFeed(&ls);
                        if(line == nullptr)
                        {
                            // Ctrl-C or Ctrl-D
                            // or pty has been closed from the master
                            break;
                        }
                        else if(line == linenoiseEditMore)
                        {
                            // Waiting for more input
                            continue;
                        }
                        else
                        {
                            m_cmd_listener(line);
                            linenoiseHistoryAdd(line);
                            linenoiseFree(line);
                            break;
                        }
                    }
                    linenoiseEditStop(&ls);
                }
                syslog(LOG_INFO, "Slave thread exiting...");
            }
        );
    }

    void stop()
    {
        syslog(LOG_INFO, "Stopping session");
        m_should_run = false;
        // if(m_pty_master_worker.joinable()) m_pty_master_worker.join();
        close(m_master_fd);
        if(m_pty_slave_worker.joinable()) m_pty_slave_worker.join();
        close(m_slave_fd);
    }

    void feed(const char *buf, size_t len)
    {
        syslog(LOG_INFO, "Feed '%s'", std::string(buf, len).c_str());
        // if(::write(m_master_fd, buf, len) == -1)
        // {
        //     return false;
        // }
        // return true;

        // use ostream to populate output buffer
        std::ostream os(&output_buffer_);
        os.write(buf, len);

        boost::asio::async_write(master_side_, output_buffer_,
        [this](boost::system::error_code ec, std::size_t length)
        {
          if (ec)
          {
            syslog(LOG_ERR, "Master side write error");
            // close();
            m_should_run = false;
          }
          else
          {
            syslog(LOG_INFO, "Master side write %zu bytes", length);
          }
        });
    }

    bool isRunning() const
    {
        return m_should_run;
    }

private:
    boost::asio::posix::stream_descriptor master_side_;
    boost::asio::streambuf input_buffer_;
    boost::asio::streambuf output_buffer_;

    int m_master_fd;
    int m_slave_fd;
    std::function<void(const char *, size_t)> m_tty_listener;
    std::function<void(std::string)> m_cmd_listener;
    std::thread m_pty_master_worker;
    std::thread m_pty_slave_worker;
    bool m_should_run = false;
};

Session createSession(boost::asio::io_context& io_context)
{
    // Open the master pseudo-terminal
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd == -1) {
        syslog(LOG_ERR, "ERROR: posix_openpt");
        exit(1);
    }

    // Grant access to the slave
    if (grantpt(master_fd) == -1) {
        syslog(LOG_ERR, "ERROR: grantpt");
        close(master_fd);
        exit(1);
    }

    // Unlock the slave
    if (unlockpt(master_fd) == -1) {
        syslog(LOG_ERR, "ERROR: unlockpt");
        close(master_fd);
        exit(1);
    }

    // Get the name of the slave device
    char slave_name[100];
    if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) != 0) {
        syslog(LOG_ERR, "ERROR: ptsname_r");
        close(master_fd);
        exit(1);
    }

    int slave_fd = open(slave_name, O_RDWR);
    if (slave_fd == -1) {
        syslog(LOG_ERR, "ERROR: open");
        exit(1);
    }

    struct winsize ws;
    ws.ws_col = 80;
    int res = ioctl(slave_fd, TIOCSWINSZ, &ws);
    if(res == -1)
    {
        syslog(LOG_ERR, "ERROR: ioctl");
        exit(1);
    }

    return Session(io_context, master_fd, slave_fd);
}

int main()
{
    openlog("pty-test", LOG_CONS, LOG_DAEMON);

    int i, fd0, fd1, fd2;
    pid_t pid;
    struct rlimit rl;
    struct sigaction sa;
    /*
     * Clear file creation mask.
     */
    umask(0);
    /*
     * Get maximum number of file descriptors.
     */
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0)
    {
        printf("can't get file limit\n");
        exit(1);
    }
    /*
     * Become a session leader to lose controlling TTY.
     */
    if ((pid = fork()) < 0)
    {
        printf("can't fork\n");
        exit(1);
    }
    else if (pid != 0) /* parent */
    {
        exit(0);
    }
    setsid();
    /*
     * Ensure future opens won't allocate controlling TTYs.
     */
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGHUP, &sa, NULL) < 0)
    {
        printf("can't ignore SIGHUP\n");
        exit(1);
    }
    if ((pid = fork()) < 0)
    {
        printf("can't fork\n");
        exit(1);
    }
    else if (pid != 0) /* parent */
    {
        exit(0);
    }
    /*
     * Change the current working directory to the root so
     * we won't prevent file systems from being unmounted.
     */
    if (chdir("/") < 0)
    {
        printf("can't change directory to /\n");
        exit(1);
    }
    /*
     * Close all open file descriptors.
     */
    if (rl.rlim_max == RLIM_INFINITY)
        rl.rlim_max = 1024;
    for (i = 0; i < rl.rlim_max; i++)
        close(i);
    /*
     * Attach file descriptors 0, 1, and 2 to /dev/null.
     */
    fd0 = open("/dev/null", O_RDWR);
    fd1 = dup(0);
    fd2 = dup(0);
    /*
     * Initialize the log file.
     */
    if (fd0 != 0 || fd1 != 1 || fd2 != 2)
    {
        syslog(LOG_ERR, "unexpected file descriptors %d %d %d",
               fd0, fd1, fd2);
        exit(1);
    }


    // Set up the TCP socket
    const int TCP_PORT = 4545;
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        syslog(LOG_ERR, "ERROR: socket");
        exit(EXIT_FAILURE);
    }

    // Enable SO_REUSEADDR
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        syslog(LOG_ERR, "setsockopt(SO_REUSEADDR)");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Set SO_LINGER for a short period
    struct linger linger_opt = {1, 5}; // Enable linger, wait for 5 seconds
    if (setsockopt(server_sock, SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt)) == -1) {
        syslog(LOG_ERR, "setsockopt(SO_LINGER)");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(TCP_PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        syslog(LOG_ERR, "ERROR: bind");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, 1) == -1) {
        syslog(LOG_ERR, "ERROR: listen");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "Listening on TCP port %d", TCP_PORT);

    // Accept a single client connection
    int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock == -1) {
        syslog(LOG_ERR, "ERROR: accept");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Set up the UDP socket
    // const int UDP_PORT = 4545;
    // int client_sock = socket(AF_INET, SOCK_DGRAM, 0);
    // if (client_sock == -1)
    // {
    //     syslog(LOG_ERR, "ERROR: socket");
    //     exit(EXIT_FAILURE);
    // }

    // struct sockaddr_in server_addr, client_addr;
    // socklen_t client_len = sizeof(client_addr);
    // memset(&server_addr, 0, sizeof(server_addr));

    // server_addr.sin_family = AF_INET;
    // server_addr.sin_addr.s_addr = INADDR_ANY;
    // server_addr.sin_port = htons(UDP_PORT);

    // if (bind(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    // {
    //     syslog(LOG_ERR, "ERROR: bind");
    //     close(client_sock);
    //     exit(EXIT_FAILURE);
    // }
    // syslog(LOG_INFO, "Listening on UDP port %d", UDP_PORT);

    boost::asio::io_context io_context;

    syslog(LOG_INFO, "Create session");
    Session session = createSession(io_context);
    session.setCmdListener([](std::string cmd)
    {
        syslog(LOG_INFO, "CMD '%s'", cmd.c_str());
    });
    session.setTtyOutputListener([client_sock, &client_addr](const char * buffer, size_t n)
    {
        syslog(LOG_INFO, "TTY '%s', send to client socket", std::string(buffer, n).c_str());
        sendto(client_sock, buffer, n, 0, (struct sockaddr *)&client_addr, sizeof(client_addr));
    });
    session.start();

    // Buffer for incoming UDP data
    const size_t BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];

    // Use poll() to wait for events on both the UDP socket and master_fd
    struct pollfd fds[1];
    fds[0].fd = client_sock;
    fds[0].events = POLLIN;

    io_context.poll();
    // Communication loop
    while (1)
    {
        int ret = poll(fds, 1, 100); // Wait indefinitely for events
        if (ret == -1)
        {
            syslog(LOG_ERR, "ERROR: poll");
            break;
        }

        if(ret == 0)
        {
            io_context.poll();
            if(!session.isRunning())
            {
                syslog(LOG_ERR, "Session is closed, exiting...");
                break;
            }
            continue;
        }

        // Check if there is data on the UDP socket
        if (fds[0].revents & POLLIN)
        {
            // ssize_t n = recvfrom(client_sock, buffer, BUFFER_SIZE - 1, 0,
            //                         (struct sockaddr *)&client_addr, &client_len);
            ssize_t n = read(client_sock, buffer, BUFFER_SIZE - 1);
            if (n == 0) {
                syslog(LOG_INFO, "Client disconnected");
                session.stop();
                break;
            }
            if (n == -1)
            {
                syslog(LOG_ERR, "Client read error");
                continue;
            }

            // insert '\r' if only '\n' is received
            if (n >= 2 && buffer[n-2] == '\r' && buffer[n-1] == '\n')
            {
                // Nothing to do
            }
            else if (n >= 1 && buffer[n-1] == '\n')
            {
                buffer[n-1] = '\r';
                buffer[n] = '\n';
                ++n;
            }
            buffer[n] = '\0'; // Null-terminate for safety
            syslog(LOG_INFO, "Received client socket data: %s", buffer);

            // Write data to the master_fd (pseudo-terminal)
            session.feed(buffer, n);
        }
        else if(fds[0].revents & POLLHUP)
        {
            syslog(LOG_ERR, "Client disconnected");
            session.stop();
            break;
        }
    }

    // Close sockets and file descriptors
    close(client_sock);

    syslog(LOG_INFO, "Exiting...");
    return 0;
}

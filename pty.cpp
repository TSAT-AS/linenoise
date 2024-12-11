#include <functional>
#include <memory>
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

class Session : public std::enable_shared_from_this<Session>
{
public:
    static constexpr std::size_t BUFFER_SIZE = 1024;

    Session(boost::asio::io_context& io_context, int master_fd, int slave_fd)
    : m_master_stream(io_context, master_fd)
    , m_master_input_buffer(BUFFER_SIZE)
    , m_master_output_buffer(BUFFER_SIZE)
    , m_slave_stream(io_context, slave_fd)
    , m_tty_listener([](const char *, size_t){ syslog(LOG_INFO, "TTY LISTENER NOT SET"); })
    , m_cmd_listener([](std::string){ syslog(LOG_INFO, "CMD LISTENER NOT SET"); })
    {}

    ~Session()
    {
        stop();
        m_master_stream.close();
        m_slave_stream.close();
    }

    void setTtyOutputListener(std::function<void(const char *, size_t)> listener)
    {
        m_tty_listener = listener;
    }

    void setCmdListener(std::function<void(std::string)> listener)
    {
        m_cmd_listener = listener;
    }

    void do_read_master_output()
    {
        syslog(LOG_ERR, "do_read_master_output()");
        if(!m_should_run)
        {
            return;
        }

        boost::asio::async_read(m_master_stream, m_master_input_buffer, boost::asio::transfer_at_least(1),
        [self = shared_from_this(), this](boost::system::error_code ec, std::size_t length)
        {
            if (!ec)
            {
                syslog(LOG_INFO, "Master: got data: %s", std::string(boost::asio::buffer_cast<const char *>(m_master_input_buffer.data()), length).c_str());
                const char *data = boost::asio::buffer_cast<const char *>(m_master_input_buffer.data());
                m_tty_listener(data, length);
                m_master_input_buffer.consume(length);
                do_read_master_output();
            }
            else if(ec == boost::asio::error::operation_aborted)
            {
                syslog(LOG_ERR, "Master: PTY connection aborted");
            }
            else
            {
                syslog(LOG_ERR, "Master: PTY connection closed");
                stop();
            }
        });
    }

    void do_read_slave_input()
    {
        syslog(LOG_ERR, "do_read_slave_input()");
        if(!m_should_run)
        {
            return;
        }

        // Start command parsing here to get initial prompt (before receiving any command)
        if(!m_parsing_in_progress)
        {
            int res = linenoiseEditStart(
                &m_linenoise_state,
                m_slave_stream.native_handle(),
                m_slave_stream.native_handle(),
                m_slave_buffer.data(),
                m_slave_buffer.size(),
                "tsat3500> "
            );
            if(res == -1)
            {
                syslog(LOG_ERR, "Could not start linenoise parsing");
                stop();
                return;
            }
            m_parsing_in_progress = true;
        }

        m_slave_stream.async_wait(boost::asio::posix::stream_descriptor::wait_read,
        [self = shared_from_this(), this](boost::system::error_code ec)
        {
            if (!ec)
            {
                syslog(LOG_INFO, "Slave side read ready");

                const char *line = linenoiseEditFeed(&m_linenoise_state);
                if(line == nullptr)
                {
                    // Abort command parsing:
                    // * Received Ctrl-C or Ctrl-D
                    // * pty has been closed from the master side
                    m_parsing_in_progress = false;
                    linenoiseEditStop(&m_linenoise_state);
                }
                else if(line == linenoiseEditMore)
                {
                    // Command not ready,
                    // waiting for more input
                }
                else
                {
                    // Command completed
                    m_cmd_listener(line);
                    linenoiseHistoryAdd(line);
                    linenoiseFree((void*)line);
                    m_parsing_in_progress = false;
                    linenoiseEditStop(&m_linenoise_state);
                }

                do_read_slave_input();
            }
            else
            {
                syslog(LOG_ERR, "Slave: PTY connection closed");
                stop();
            }
        });
    }

    void start()
    {
        syslog(LOG_INFO, "Starting session");
        m_should_run = true;
        do_read_master_output();
        do_read_slave_input();
    }

    void stop()
    {
        syslog(LOG_INFO, "Stopping session");
        m_should_run = false;
        boost::system::error_code dummy;
        m_master_stream.cancel(dummy);
        m_slave_stream.cancel(dummy);
    }

    void feed(const char *buf, size_t len)
    {
        syslog(LOG_INFO, "Feed '%s'", std::string(buf, len).c_str());
        std::ostream os(&m_master_output_buffer);
        os.write(buf, len);

        boost::asio::async_write(m_master_stream, m_master_output_buffer,
        [this](boost::system::error_code ec, std::size_t length)
        {
          if (!ec)
          {
            syslog(LOG_INFO, "Master side write %zu bytes", length);
          }
          else if(ec == boost::asio::error::operation_aborted)
          {
            syslog(LOG_ERR, "Master side write aborted");
          }
          else
          {
            syslog(LOG_ERR, "Master side write error");
            stop();
          }
        });
    }

    bool isRunning() const
    {
        return m_should_run;
    }

private:
    bool m_should_run{false};

    // PTY master side
    boost::asio::posix::stream_descriptor m_master_stream;
    boost::asio::streambuf m_master_input_buffer;
    boost::asio::streambuf m_master_output_buffer;

    // PTY slave side / Linenoise
    boost::asio::posix::stream_descriptor m_slave_stream;
    std::array<char, 1024> m_slave_buffer{};
    struct linenoiseState m_linenoise_state{};
    bool m_parsing_in_progress{false};

    // Result listeners
    std::function<void(const char *, size_t)> m_tty_listener;
    std::function<void(std::string)> m_cmd_listener;
};

std::shared_ptr<Session> createSession(boost::asio::io_context& io_context)
{
    // Open the master pseudo-terminal
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd == -1) {
        syslog(LOG_ERR, "ERROR: posix_openpt");
        return {};
    }

    // Grant access to the slave
    if (grantpt(master_fd) == -1) {
        syslog(LOG_ERR, "ERROR: grantpt");
        close(master_fd);
        return {};
    }

    // Unlock the slave
    if (unlockpt(master_fd) == -1) {
        syslog(LOG_ERR, "ERROR: unlockpt");
        close(master_fd);
        return {};
    }

    // Get the name of the slave device
    char slave_name[100];
    if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) != 0) {
        syslog(LOG_ERR, "ERROR: ptsname_r");
        close(master_fd);
        return {};
    }

    // Open the slave pseudo-terminal
    int slave_fd = open(slave_name, O_RDWR);
    if (slave_fd == -1) {
        syslog(LOG_ERR, "ERROR: open");
        close(master_fd);
        return {};
    }

    // Set the PTY window size
    // This is important because linenoise wants to know the column count
    // and will start producing magic ANSI control sequence querries
    // (which we don't handle) if not provided
    struct winsize ws{};
    ws.ws_col = 80;
    int res = ioctl(slave_fd, TIOCSWINSZ, &ws);
    if(res == -1)
    {
        syslog(LOG_ERR, "ERROR: ioctl");
        close(slave_fd);
        close(master_fd);
        return {};
    }

    return std::make_shared<Session>(io_context, master_fd, slave_fd);
}

int main()
{
    openlog("pty-test", LOG_CONS, LOG_DAEMON);

    int fd0, fd1, fd2;
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
    for (size_t i = 0; i < rl.rlim_max; i++)
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

    {

    syslog(LOG_INFO, "Create session");
    auto session = createSession(io_context);
    if(!session)
    {
        syslog(LOG_ERR, "Could not create session");
        return 1;
    }

    session->setCmdListener([](std::string cmd)
    {
        syslog(LOG_INFO, "CMD '%s'", cmd.c_str());
    });
    session->setTtyOutputListener([client_sock, &client_addr](const char * buffer, size_t n)
    {
        syslog(LOG_INFO, "TTY '%s', send to client socket", std::string(buffer, n).c_str());
        sendto(client_sock, buffer, n, 0, (struct sockaddr *)&client_addr, sizeof(client_addr));
    });
    session->start();

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
        int ret = poll(fds, 1, 10); // Wait indefinitely for events
        if (ret == -1)
        {
            syslog(LOG_ERR, "ERROR: poll");
            break;
        }

        if(ret == 0)
        {
            io_context.poll();
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
                break;
            }
            if (n == -1)
            {
                syslog(LOG_ERR, "Client read error");
                continue;
            }

            // insert '\r' if only '\n' is received
            // needed by e.g. netcat when '-C' is not active
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
            session->feed(buffer, n);
        }
        else if(fds[0].revents & POLLHUP)
        {
            syslog(LOG_ERR, "Client disconnected");
            break;
        }
    }

    }

    // Close sockets and file descriptors
    close(client_sock);

    std::this_thread::sleep_for(std::chrono::seconds(1));
    io_context.poll();

    syslog(LOG_INFO, "Exiting...");
    return 0;
}

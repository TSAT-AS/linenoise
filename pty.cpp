#include <iostream>
#include <thread>
#include <stdexcept>
#include <functional>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/resource.h>
#include "linenoise.h"
#include <signal.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
class Session
{
public:
    Session(int master_fd, int slave_fd)
    : m_master_fd(master_fd)
    , m_slave_fd(slave_fd)
    , m_tty_listener([](const char * buffer, size_t n){ syslog(LOG_INFO, "TTY '%s'", std::string(buffer, n).c_str()); })
    , m_cmd_listener([](const char * buffer, size_t n){ syslog(LOG_INFO, "CMD '%s'", std::string(buffer, n).c_str()); })
    {}

    ~Session()
    {
        m_should_run = false;
        if(m_pty_master_worker.joinable()) m_pty_master_worker.join();
        if(m_pty_slave_worker.joinable()) m_pty_slave_worker.join();
        close(m_master_fd);
        close(m_slave_fd);
    }

    void setTtyOutputListener(std::function<void(const char *, size_t)> listener)
    {
        m_tty_listener = listener;
    }

    void setCmdListener(std::function<void(const char *, size_t)> listener)
    {
        m_cmd_listener = listener;
    }

    void run()
    {
        m_pty_master_worker =
            std::thread([this]()
            {
                // Buffer for incoming UDP data
                const size_t BUFFER_SIZE = 1024;
                char buffer[BUFFER_SIZE];

                // Use poll() to wait for events on master_fd
                struct pollfd fds[1];
                fds[0].fd = m_master_fd;
                fds[0].events = POLLIN | POLLHUP;

                // Communication loop
                while (m_should_run)
                {
                    int ret = poll(fds, 1, -1); // Wait indefinitely for events
                    if (ret == -1)
                    {
                        syslog(LOG_ERR, "poll");
                        break;
                    }

                    // Check if there is data on the master_fd from the slave
                    if (fds[0].revents & POLLIN)
                    {
                        ssize_t m = read(m_master_fd, buffer, BUFFER_SIZE - 1);
                        if (m > 0)
                        {
                            buffer[m] = '\0';
                            syslog(LOG_INFO, "Slave response: '%s'", buffer);
                            m_tty_listener(buffer, m);
                        }
                    }
                    else if(fds[0].revents & POLLHUP)
                    {
                        syslog(LOG_ERR, "Linenoise has finished, close down link");
                        m_should_run = false;
                        close(m_master_fd);
                    }
                }
            }
        );

        m_pty_slave_worker =
            std::thread([this]()
            {
                struct linenoiseState ls{};
                char buf[1024];
                while(m_should_run)
                {
                    linenoiseEditStart(&ls, m_slave_fd, m_slave_fd, buf, sizeof(buf), "hello> ");

                    while(1)
                    {
                        char *line = linenoiseEditFeed(&ls);
                        if(line == nullptr)
                        {
                            syslog(LOG_INFO, "Good Bye!");
                            m_should_run = false;
                            close(m_slave_fd);
                            break;
                        }
                        else if(line == linenoiseEditMore)
                        {
                            continue;
                        }
                        else
                        {
                            m_cmd_listener(line, strlen(line));
                            linenoiseHistoryAdd(line);
                            linenoiseFree(line);
                            write(m_slave_fd, "\r\n", 2);
                            break;
                        }
                    }
                }
            }
        );
    }

    bool feed(const char *buf, size_t len)
    {
        syslog(LOG_INFO, "Feed '%s'", std::string(buf, len).c_str());
        if(::write(m_master_fd, buf, len) == -1)
        {
            return false;
        }
        return true;
    }

    bool isRunning() const
    {
        return m_should_run;
    }

private:
    int m_master_fd;
    int m_slave_fd;
    std::function<void(const char *, size_t)> m_tty_listener;
    std::function<void(const char *, size_t)> m_cmd_listener;
    std::thread m_pty_master_worker;
    std::thread m_pty_slave_worker;
    bool m_should_run = true;
};

Session createSession()
{
    // Open the master pseudo-terminal
    int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (master_fd == -1) {
        perror("posix_openpt");
        exit(1);
    }

    // Grant access to the slave
    if (grantpt(master_fd) == -1) {
        perror("grantpt");
        close(master_fd);
        exit(1);
    }

    // Unlock the slave
    if (unlockpt(master_fd) == -1) {
        perror("unlockpt");
        close(master_fd);
        exit(1);
    }

    // Get the name of the slave device
    char slave_name[100];
    if (ptsname_r(master_fd, slave_name, sizeof(slave_name)) != 0) {
        perror("ptsname_r");
        close(master_fd);
        exit(1);
    }

    int slave_fd = open(slave_name, O_RDWR);
    if (slave_fd == -1) {
        perror("open");
        exit(1);
    }

    struct winsize ws;
    ws.ws_col = 80;
    int res = ioctl(slave_fd, TIOCSWINSZ, &ws);
    if(res == -1)
    {
        perror("ioctl");
        exit(1);
    }

    return Session(master_fd, slave_fd);
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




    // Set up the UDP socket
    const int UDP_PORT = 4545;
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(UDP_PORT);

    if (bind(udp_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        close(udp_sock);
        exit(EXIT_FAILURE);
    }
    syslog(LOG_INFO, "Listening on UDP port %d", UDP_PORT);






    syslog(LOG_INFO, "Create session");
    Session session = createSession();

    session.setCmdListener([](const char * buffer, size_t n)
    {
        syslog(LOG_INFO, "CMD '%s'", std::string(buffer, n).c_str());
    });
    session.setTtyOutputListener([udp_sock, &client_addr](const char * buffer, size_t n)
    {
        syslog(LOG_INFO, "TTY '%s', send to UDP socket", std::string(buffer, n).c_str());
        sendto(udp_sock, buffer, n, 0, (struct sockaddr *)&client_addr, sizeof(client_addr));
    });

    syslog(LOG_INFO, "Start session");
    session.run();

    // Buffer for incoming UDP data
    const size_t BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];

    // Use poll() to wait for events on both the UDP socket and master_fd
    struct pollfd fds[1];
    fds[0].fd = udp_sock;
    fds[0].events = POLLIN;

    // Communication loop
    while (1)
    {
        int ret = poll(fds, 1, 1000); // Wait indefinitely for events
        if (ret == -1)
        {
            perror("poll");
            break;
        }

        if(ret == 0)
        {
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
            ssize_t n = recvfrom(udp_sock, buffer, BUFFER_SIZE - 1, 0,
                                    (struct sockaddr *)&client_addr, &client_len);
            if (n == -1)
            {
                perror("recvfrom");
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
            syslog(LOG_INFO, "Received UDP data: %s", buffer);

            // Write data to the master_fd (pseudo-terminal)
            if(!session.feed(buffer, n))
            {
                syslog(LOG_ERR, "PTYs are closed, exiting...");
                break;
            }
        }
    }

    // Close sockets and file descriptors
    close(udp_sock);

    syslog(LOG_INFO, "Exiting...");
    return 0;
}

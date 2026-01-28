#include "include/nova_net.hpp"
#include <iostream>

int main(int, char **) {
    using namespace evqovv::nova_net;

    event_loop loop;
    auto acceptor = std::make_shared<tcp_acceptor>(loop, "0.0.0.0", 10003);
    acceptor->set_new_connection_handler(
        [](::std::shared_ptr<tcp_connection> conn) {
            conn->set_message_handler([conn](const std::string &msg) {
                std::cout << "recv: " << msg << '\n';
                conn->send(msg);
            });
        });

    acceptor->start();
    loop.loop();
}

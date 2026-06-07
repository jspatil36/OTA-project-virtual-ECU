#pragma once

#include <iostream>
#include <vector>
#include <thread>
#include <boost/asio.hpp>
#include "doip_session.hpp" // Include the new session header

using boost::asio::ip::tcp;

class DoIPServer {
public:
    DoIPServer(boost::asio::io_context& io_context, short port)
        : m_io_context(io_context),
          m_acceptor(io_context, tcp::endpoint(tcp::v4(), port)) {
        std::cout << "[DoIP] Server starting on port " << port << "..." << std::endl;
    }

    void run() {
        try {
            start_accept();
            m_io_context.run();
        } catch (const std::exception& e) {
            std::cerr << "[DoIP] Server exception: " << e.what() << std::endl;
        }
        std::cout << "[DoIP] Server has stopped." << std::endl;
    }

    void stop() {
        m_io_context.stop();
    }

private:
    void start_accept() {
        // Asynchronously accept a new connection.
        m_acceptor.async_accept([this](const boost::system::error_code& error, tcp::socket socket) {
            if (!error) {
                // Connection successful. Create a new session and start it.
                // The session will manage its own lifecycle from here.
                std::make_shared<DoIPSession>(std::move(socket))->start();
            } else {
                std::cerr << "[DoIP] Error accepting connection: " << error.message() << std::endl;
            }

            // Immediately start waiting for the next connection.
            start_accept();
        });
    }

    boost::asio::io_context& m_io_context;
    tcp::acceptor m_acceptor;
};

//
// Copyright (c) 2016-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

//------------------------------------------------------------------------------
//
// Example: Advanced server
//
//------------------------------------------------------------------------------

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/config.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using tcp = boost::asio::ip::tcp;               // from <boost/asio/ip/tcp.hpp>
namespace http = boost::beast::http;            // from <boost/beast/http.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

// Return a reasonable mime type based on the extension of a file.
boost::beast::string_view
mime_type(boost::beast::string_view path)
{
    using boost::beast::iequals;
    auto const ext = [&path]
    {
        auto const pos = path.rfind(".");
        if(pos == boost::beast::string_view::npos)
            return boost::beast::string_view{};
        return path.substr(pos);
    }();
    if(iequals(ext, ".htm"))  return "text/html";
    if(iequals(ext, ".html")) return "text/html";
    if(iequals(ext, ".php"))  return "text/html";
    if(iequals(ext, ".css"))  return "text/css";
    if(iequals(ext, ".txt"))  return "text/plain";
    if(iequals(ext, ".js"))   return "application/javascript";
    if(iequals(ext, ".json")) return "application/json";
    if(iequals(ext, ".xml"))  return "application/xml";
    if(iequals(ext, ".swf"))  return "application/x-shockwave-flash";
    if(iequals(ext, ".flv"))  return "video/x-flv";
    if(iequals(ext, ".png"))  return "image/png";
    if(iequals(ext, ".jpe"))  return "image/jpeg";
    if(iequals(ext, ".jpeg")) return "image/jpeg";
    if(iequals(ext, ".jpg"))  return "image/jpeg";
    if(iequals(ext, ".gif"))  return "image/gif";
    if(iequals(ext, ".bmp"))  return "image/bmp";
    if(iequals(ext, ".ico"))  return "image/vnd.microsoft.icon";
    if(iequals(ext, ".tiff")) return "image/tiff";
    if(iequals(ext, ".tif"))  return "image/tiff";
    if(iequals(ext, ".svg"))  return "image/svg+xml";
    if(iequals(ext, ".svgz")) return "image/svg+xml";
    return "application/text";
}

// Append an HTTP rel-path to a local filesystem path.
// The returned path is normalized for the platform.
std::string
path_cat(
    boost::beast::string_view base,
    boost::beast::string_view path)
{
    if(base.empty())
        return path.to_string();
    std::string result = base.to_string();
#if BOOST_MSVC
    char constexpr path_separator = '\\';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
    for(auto& c : result)
        if(c == '/')
            c = path_separator;
#else
    char constexpr path_separator = '/';
    if(result.back() == path_separator)
        result.resize(result.size() - 1);
    result.append(path.data(), path.size());
#endif
    return result;
}

// This function produces an HTTP response for the given
// request. The type of the response object depends on the
// contents of the request, so the interface requires the
// caller to pass a generic lambda for receiving the response.
template<
    class Body, class Allocator,
    class Send>
void
handle_request(
    boost::beast::string_view doc_root,
    http::request<Body, http::basic_fields<Allocator>>&& req,
    Send&& send)
{
    // Returns a bad request response
    auto const bad_request =
    [&req](boost::beast::string_view why)
    {
        http::response<http::string_body> res{http::status::bad_request, req.version};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body = why.to_string();
        res.prepare_payload();
        return res;
    };

    // Returns a not found response
    auto const not_found =
    [&req](boost::beast::string_view target)
    {
        http::response<http::string_body> res{http::status::not_found, req.version};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body = "The resource '" + target.to_string() + "' was not found.";
        res.prepare_payload();
        return res;
    };

    // Returns a server error response
    auto const server_error =
    [&req](boost::beast::string_view what)
    {
        http::response<http::string_body> res{http::status::internal_server_error, req.version};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body = "An error occurred: '" + what.to_string() + "'";
        res.prepare_payload();
        return res;
    };

    // Make sure we can handle the method
    if( req.method() != http::verb::get &&
        req.method() != http::verb::head)
        return send(bad_request("Unknown HTTP-method"));

    // Request path must be absolute and not contain "..".
    if( req.target().empty() ||
        req.target()[0] != '/' ||
        req.target().find("..") != boost::beast::string_view::npos)
        return send(bad_request("Illegal request-target"));

    // Build the path to the requested file
    std::string path = path_cat(doc_root, req.target());
    if(req.target().back() == '/')
        path.append("index.html");

    // Attempt to open the file
    boost::beast::error_code ec;
    http::file_body::value_type body;
    body.open(path.c_str(), boost::beast::file_mode::scan, ec);

    // Handle the case where the file doesn't exist
    if(ec == boost::system::errc::no_such_file_or_directory)
        return send(not_found(req.target()));

    // Handle an unknown error
    if(ec)
        return send(server_error(ec.message()));

    // Respond to HEAD request
    if(req.method() == http::verb::head)
    {
        http::response<http::empty_body> res{http::status::ok, req.version};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, mime_type(path));
        res.content_length(body.size());
        res.keep_alive(req.keep_alive());
        return send(std::move(res));
    }

    // Respond to GET request
    http::response<http::file_body> res{
        std::piecewise_construct,
        std::make_tuple(std::move(body)),
        std::make_tuple(http::status::ok, req.version)};
    res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    res.set(http::field::content_type, mime_type(path));
    res.content_length(body.size());
    res.keep_alive(req.keep_alive());
    return send(std::move(res));
}

//------------------------------------------------------------------------------

// Report a failure
void
fail(boost::system::error_code ec, char const* what)
{
    std::cerr << what << ": " << ec.message() << "\n";
}

// Echoes back all received WebSocket messages
class websocket_session : public std::enable_shared_from_this<websocket_session>
{
    websocket::stream<tcp::socket> ws_;
    boost::asio::io_service::strand strand_;
    boost::asio::steady_timer timer_;
    boost::beast::multi_buffer buffer_;

public:
    // Take ownership of the socket
    explicit
    websocket_session(tcp::socket socket)
        : ws_(std::move(socket))
        , strand_(ws_.get_io_service())
        , timer_(ws_.get_io_service(),
            (std::chrono::steady_clock::time_point::max)())
    {
    }

    // Start the asynchronous operation
    template<class Body, class Allocator>
    void
    run(http::request<Body, http::basic_fields<Allocator>> req)
    {
        // Run the timer. The timer is operated
        // continuously, this simplifies the code.
        on_timer({});

        // Set the timer
        timer_.expires_from_now(std::chrono::seconds(15));

        // Accept the websocket handshake
        ws_.async_accept(
            req,
            strand_.wrap(std::bind(
                &websocket_session::on_accept,
                shared_from_this(),
                std::placeholders::_1)));
    }

    // Called when the timer expires.
    void
    on_timer(boost::system::error_code ec)
    {
        if(ec && ec != boost::asio::error::operation_aborted)
            return fail(ec, "timer");

        // Verify that the timer really expired since the deadline may have moved.
        if(timer_.expires_at() <= std::chrono::steady_clock::now())
        {
            // Closing the socket cancels all outstanding operations. They
            // will complete with boost::asio::error::operation_aborted
            ws_.next_layer().shutdown(tcp::socket::shutdown_both, ec);
            ws_.next_layer().close(ec);
            return;
        }

        // Wait on the timer
        timer_.async_wait(
            strand_.wrap(std::bind(
                &websocket_session::on_timer,
                shared_from_this(),
                std::placeholders::_1)));
    }

    void
    on_accept(boost::system::error_code ec)
    {
        // Happens when the timer closes the socket
        if(ec == boost::asio::error::operation_aborted)
            return;

        if(ec)
            return fail(ec, "accept");

        // Read a message
        do_read();
    }

    void
    do_read()
    {
        // Set the timer
        timer_.expires_from_now(std::chrono::seconds(15));

        // Read a message into our buffer
        ws_.async_read(
            buffer_,
            strand_.wrap(std::bind(
                &websocket_session::on_read,
                shared_from_this(),
                std::placeholders::_1)));
    }

    void
    on_read(boost::system::error_code ec)
    {
        // Happens when the timer closes the socket
        if(ec == boost::asio::error::operation_aborted)
            return;

        // This indicates that the websocket_session was closed
        if(ec == websocket::error::closed)
            return;

        if(ec)
            fail(ec, "read");

        // Echo the message
        ws_.text(ws_.got_text());
        ws_.async_write(
            buffer_.data(),
            strand_.wrap(std::bind(
                &websocket_session::on_write,
                shared_from_this(),
                std::placeholders::_1)));
    }

    void
    on_write(boost::system::error_code ec)
    {
        // Happens when the timer closes the socket
        if(ec == boost::asio::error::operation_aborted)
            return;

        if(ec)
            return fail(ec, "write");

        // Clear the buffer
        buffer_.consume(buffer_.size());

        // Do another read
        do_read();
    }
};

// Handles an HTTP server connection
class http_session : public std::enable_shared_from_this<http_session>
{
    // This queue is used for HTTP pipelining.
    class queue
    {
        enum
        {
            // Maximum number of responses we will queue
            limit = 8
        };

        // The type-erased, saved work item
        struct work
        {
            virtual ~work() = default;
            virtual void operator()() = 0;
        };

        http_session& self_;
        std::vector<std::unique_ptr<work>> items_;

    public:
        explicit
        queue(http_session& self)
            : self_(self)
        {
            static_assert(limit > 0, "queue limit must be positive");
            items_.reserve(limit);
        }

        // Returns `true` if we have reached the queue limit
        bool
        is_full() const
        {
            return items_.size() >= limit;
        }

        // Called when a message finishes sending
        // Returns `true` if the caller should initiate a read
        bool
        on_write()
        {
            BOOST_ASSERT(! items_.empty());
            auto const was_full = is_full();
            items_.erase(items_.begin());
            if(! items_.empty())
                (*items_.front())();
            return was_full;
        }

        // Called by the HTTP handler to send a response.
        template<bool isRequest, class Body, class Fields>
        void
        operator()(http::message<isRequest, Body, Fields>&& msg)
        {
            // This holds a work item
            struct work_impl : work
            {
                http_session& self_;
                http::message<isRequest, Body, Fields> msg_;

                work_impl(
                    http_session& self,
                    http::message<isRequest, Body, Fields>&& msg)
                    : self_(self)
                    , msg_(std::move(msg))
                {
                }

                void
                operator()()
                {
                    http::async_write(
                        self_.socket_,
                        msg_,
                        self_.strand_.wrap(std::bind(
                            &http_session::on_write,
                            self_.shared_from_this(),
                            std::placeholders::_1)));
                }
            };

            // Allocate and store the work
            items_.emplace_back(new work_impl(self_, std::move(msg)));

            // If there was no previous work, start this one
            if(items_.size() == 1)
                (*items_.front())();
        }
    };

    tcp::socket socket_;
    boost::asio::io_service::strand strand_;
    boost::asio::steady_timer timer_;
    boost::beast::flat_buffer buffer_;
    std::string const& doc_root_;
    http::request<http::string_body> req_;
    queue queue_;

public:
    // Take ownership of the socket
    explicit
    http_session(
        tcp::socket socket,
        std::string const& doc_root)
        : socket_(std::move(socket))
        , strand_(socket_.get_io_service())
        , timer_(socket_.get_io_service(),
            (std::chrono::steady_clock::time_point::max)())
        , doc_root_(doc_root)
        , queue_(*this)
    {
    }

    // Start the asynchronous operation
    void
    run()
    {
        // Run the timer. The timer is operated
        // continuously, this simplifies the code.
        on_timer({});

        do_read();
    }

    void
    do_read()
    {
        // Set the timer
        timer_.expires_from_now(std::chrono::seconds(15));

        // Read a request
        http::async_read(socket_, buffer_, req_,
            strand_.wrap(std::bind(
                &http_session::on_read,
                shared_from_this(),
                std::placeholders::_1)));
    }

    // Called when the timer expires.
    void
    on_timer(boost::system::error_code ec)
    {
        if(ec && ec != boost::asio::error::operation_aborted)
            return fail(ec, "timer");

        // Verify that the timer really expired since the deadline may have moved.
        if(timer_.expires_at() <= std::chrono::steady_clock::now())
        {
            // Closing the socket cancels all outstanding operations. They
            // will complete with boost::asio::error::operation_aborted
            socket_.shutdown(tcp::socket::shutdown_both, ec);
            socket_.close(ec);
            return;
        }

        // Wait on the timer
        timer_.async_wait(
            strand_.wrap(std::bind(
                &http_session::on_timer,
                shared_from_this(),
                std::placeholders::_1)));
    }

    void
    on_read(boost::system::error_code ec)
    {
        // Happens when the timer closes the socket
        if(ec == boost::asio::error::operation_aborted)
            return;

        // This means they closed the connection
        if(ec == http::error::end_of_stream)
            return do_close();

        if(ec)
            return fail(ec, "read");

        // See if it is a WebSocket Upgrade
        if(websocket::is_upgrade(req_))
        {
            // Create a WebSocket websocket_session by transferring the socket
            std::make_shared<websocket_session>(
                std::move(socket_))->run(std::move(req_));
            return;
        }

        // Send the response
        handle_request(doc_root_, std::move(req_), queue_);

        // If we aren't at the queue limit, try to pipeline another request
        if(! queue_.is_full())
            do_read();
    }

    void
    on_write(boost::system::error_code ec)
    {
        // Happens when the timer closes the socket
        if(ec == boost::asio::error::operation_aborted)
            return;

        if(ec == http::error::end_of_stream)
        {
            // This means we should close the connection, usually because
            // the response indicated the "Connection: close" semantic.
            return do_close();
        }

        if(ec)
            return fail(ec, "write");

        // Inform the queue that a write completed
        if(queue_.on_write())
        {
            // Read another request
            do_read();
        }
    }

    void
    do_close()
    {
        // Send a TCP shutdown
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_send, ec);

        // At this point the connection is closed gracefully
    }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
    boost::asio::io_service::strand strand_;
    tcp::acceptor acceptor_;
    tcp::socket socket_;
    std::string const& doc_root_;

public:
    listener(
        boost::asio::io_service& ios,
        tcp::endpoint endpoint,
        std::string const& doc_root)
        : strand_(ios)
        , acceptor_(ios)
        , socket_(ios)
        , doc_root_(doc_root)
    {
        boost::system::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if(ec)
        {
            fail(ec, "open");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if(ec)
        {
            fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(
            boost::asio::socket_base::max_connections, ec);
        if(ec)
        {
            fail(ec, "listen");
            return;
        }
    }

    // Start accepting incoming connections
    void
    run()
    {
        if(! acceptor_.is_open())
            return;
        do_accept();
    }

    void
    do_accept()
    {
        acceptor_.async_accept(
            socket_,
            std::bind(
                &listener::on_accept,
                shared_from_this(),
                std::placeholders::_1));
    }

    void
    on_accept(boost::system::error_code ec)
    {
        if(ec)
        {
            fail(ec, "accept");
        }
        else
        {
            // Create the http_session and run it
            std::make_shared<http_session>(
                std::move(socket_),
                doc_root_)->run();
        }

        // Accept another connection
        do_accept();
    }
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Check command line arguments.
    if (argc != 5)
    {
        std::cerr <<
            "Usage: advanced-server <address> <port> <doc_root> <threads>\n" <<
            "Example:\n" <<
            "    advanced-server 0.0.0.0 8080 . 1\n";
        return EXIT_FAILURE;
    }
    auto const address = boost::asio::ip::address::from_string(argv[1]);
    auto const port = static_cast<unsigned short>(std::atoi(argv[2]));
    std::string const doc_root = argv[3];
    auto const threads = std::max<std::size_t>(1, std::atoi(argv[3]));

    // The io_service is required for all I/O
    boost::asio::io_service ios{threads};

    // Create and launch a listening port
    std::make_shared<listener>(
        ios,
        tcp::endpoint{address, port},
        doc_root)->run();

    // Run the I/O service on the requested number of threads
    std::vector<std::thread> v;
    v.reserve(threads - 1);
    for(auto i = threads - 1; i > 0; --i)
        v.emplace_back(
        [&ios]
        {
            ios.run();
        });
    ios.run();

    return EXIT_SUCCESS;
}

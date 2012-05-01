
#include <boost/lexical_cast.hpp>

#include "server_connection.hpp"
#include "transfer.hpp"
#include "session_impl.hpp"
#include "log.hpp"
#include "alert_types.hpp"

namespace libed2k
{

    typedef boost::iostreams::basic_array_source<char> Device;

    server_connection::server_connection(aux::session_impl& ses):
        m_nClientId(0),
        m_name_lookup(ses.m_io_service),
        m_keep_alive(ses.m_io_service),
        m_ses(ses),
        m_nFilesCount(0),
        m_nUsersCount(0),
        m_nTCPFlags(0),
        m_nAuxPort(0),
        m_bInitialization(false),
        m_socket(ses.m_io_service),
        m_deadline(ses.m_io_service),
        m_udp_socket(ses.m_io_service),
        m_udp_name_lookup(ses.m_io_service)
    {
        m_deadline.expires_at(boost::posix_time::pos_infin);
    }

    server_connection::~server_connection()
    {
        boost::singleton_pool<boost::pool_allocator_tag, sizeof(char)>::release_memory();
    }

    void server_connection::start()
    {
        m_bInitialization = true; // start connecting
        const session_settings& settings = m_ses.settings();

        check_deadline();

        tcp::resolver::query q(settings.server_hostname,
                               boost::lexical_cast<std::string>(settings.server_port));

        // temporary off udp
        //udp::resolver::query uq(settings.server_hostname,
        //        boost::lexical_cast<std::string>(settings.server_port + 3)); // UDP port = TCP port + 3

        m_name_lookup.async_resolve(
            q, boost::bind(&server_connection::on_name_lookup, self(), _1, _2));


        //m_udp_name_lookup.async_resolve(
        //    uq, boost::bind(&server_connection::on_udp_name_lookup, self(), _1, _2));
    }

    void server_connection::close(const error_code& ec)
    {
        DBG("server_connection::close()");
        m_bInitialization = false;
        m_socket.close();
        m_deadline.cancel();
        m_name_lookup.cancel();
        m_keep_alive.cancel();
        m_udp_socket.close();
        m_ses.server_stopped(); // inform session
        m_ses.m_alerts.post_alert_should(server_connection_failed(ec));
    }

    bool server_connection::is_stopped() const
    {
        return (!m_socket.is_open());
    }

    bool server_connection::initializing() const
    {
        return (m_bInitialization);
    }

    const tcp::endpoint& server_connection::getServerEndpoint() const
    {
        return (m_target);
    }

    void server_connection::post_search_request(search_request& sr)
    {
        if (!is_stopped())
        {
            do_write(sr);
        }
    }

    void server_connection::post_sources_request(const md4_hash& hFile, boost::uint64_t nSize)
    {
        if (!is_stopped())
        {
            DBG("server_connection::post_sources_request(" << hFile.toString() << ", " << nSize << ")");
            get_file_sources gfs;
            gfs.m_hFile = hFile;
            gfs.m_file_size.nQuadPart = nSize;
            do_write(gfs);
        }
    }

    void server_connection::post_announce(offer_files_list& offer_list)
    {
        if (!is_stopped())
        {
            DBG("server_connection::post_announce: " << offer_list.m_collection.size());
            do_write(offer_list);
        }
    }

    void server_connection::on_name_lookup(
        const error_code& error, tcp::resolver::iterator i)
    {
        const session_settings& settings = m_ses.settings();

        if (error == boost::asio::error::operation_aborted)
        {
            close(error);
            return;
        }

        if (error || i == tcp::resolver::iterator())
        {
            ERR("server name: " << settings.server_hostname
                << ", resolve failed: " << error);
            close(error);
            return;
        }

        m_target = *i;

        DBG("server name resolved: " << libtorrent::print_endpoint(m_target));

        // prepare for connect
        // set timeout
        // execute connect
        m_deadline.expires_from_now(boost::posix_time::seconds(settings.peer_connect_timeout));
        m_socket.async_connect(m_target, boost::bind(&server_connection::on_connection_complete, self(), _1));
    }


    void server_connection::on_udp_name_lookup(
        const error_code& error, udp::resolver::iterator i)
    {
        const session_settings& settings = m_ses.settings();

        if (error == boost::asio::error::operation_aborted) return;

        if (error || i == udp::resolver::iterator())
        {
            ERR("server name: " << settings.server_hostname
                << ", resolve failed: " << error);
            return;
        }

        m_udp_target = *i;

        DBG("server name resolved: " << libtorrent::print_endpoint(m_udp_target));
        // start udp socket on out host
        m_udp_socket.open(udp::v4());

        //DBG("udp socket status: " << (m_udp_socket.is_open())?"opened":"closed");
    }

    // private callback methods
    void server_connection::on_connection_complete(error_code const& error)
    {
        DBG("server_connection::on_connection_complete");

        m_bInitialization = false;  // initialization complete

        if (is_stopped())
        {
            DBG("socket was closed");
            return;
        }

        if (error)
        {
            ERR("connection to: " << libtorrent::print_endpoint(m_target)
                << ", failed: " << error);
            close(error);
            return;
        }

        m_ses.settings().server_ip = m_target.address().to_v4().to_ulong();

        DBG("connect to server:" << libtorrent::print_endpoint(m_target) << ", successfully");

        const session_settings& settings = m_ses.settings();

        cs_login_request    login;
        //!< generate initial packet to server
        boost::uint32_t nVersion = 0x3c;
        boost::uint32_t nCapability = CAPABLE_AUXPORT | CAPABLE_NEWTAGS | CAPABLE_UNICODE | CAPABLE_LARGEFILES;
        boost::uint32_t nClientVersion  = (3 << 24) | (2 << 17) | (3 << 10) | (1 << 7);

        login.m_hClient                 = settings.client_hash;
        login.m_sNetIdentifier.m_nIP    = 0;
        login.m_sNetIdentifier.m_nPort  = settings.listen_port;

        login.m_list.add_tag(make_string_tag(std::string(settings.client_name), CT_NAME, true));
        login.m_list.add_tag(make_typed_tag(nVersion, CT_VERSION, true));
        login.m_list.add_tag(make_typed_tag(nCapability, CT_SERVER_FLAGS, true));
        login.m_list.add_tag(make_typed_tag(nClientVersion, CT_EMULE_VERSION, true));
        login.m_list.dump();

        // prepare server ping
        m_keep_alive.expires_from_now(boost::posix_time::seconds(settings.server_keep_alive_timeout));
        m_keep_alive.async_wait(boost::bind(&server_connection::write_server_keep_alive, self()));

        do_read();
        do_write(login);      // write login message
    }

    void server_connection::write_server_keep_alive()
    {
        // do nothing when server connection stopped
        if (is_stopped())
        {
            DBG("server_connection::write_server_keep_alive: stopped");
            return;
        }

        offer_files_list empty_list;
        DBG("server_connection::write_server_keep_alive: send server ping");

        do_write(empty_list);
        m_keep_alive.expires_from_now(boost::posix_time::seconds(m_ses.settings().server_keep_alive_timeout));
        m_keep_alive.async_wait(boost::bind(&server_connection::write_server_keep_alive, self()));
    }

    void server_connection::handle_write(const error_code& error, size_t nSize)
    {
        if (is_stopped()) return;

        if (!error)
        {
            m_write_order.pop_front();

            if (!m_write_order.empty())
            {
                // set deadline timer
                m_deadline.expires_from_now(boost::posix_time::seconds(m_ses.settings().server_timeout));

                std::vector<boost::asio::const_buffer> buffers;
                buffers.push_back(boost::asio::buffer(&m_write_order.front().first, header_size));
                buffers.push_back(boost::asio::buffer(m_write_order.front().second));
                boost::asio::async_write(m_socket, buffers, boost::bind(&server_connection::handle_write, self(),
                                    boost::asio::placeholders::error,
                                    boost::asio::placeholders::bytes_transferred));
            }
        }
        else
        {
            close(error);
        }
    }

    void server_connection::do_read()
    {
        m_deadline.expires_from_now(boost::posix_time::seconds(
                                        m_ses.settings().server_timeout));
        boost::asio::async_read(m_socket,
                       boost::asio::buffer(&m_in_header, header_size),
                       boost::bind(&server_connection::handle_read_header,
                               self(),
                               boost::asio::placeholders::error,
                               boost::asio::placeholders::bytes_transferred));
    }

    void server_connection::handle_read_header(const error_code& error, size_t nSize)
    {
        if (is_stopped()) return;

        if (!error)
        {
            switch(m_in_header.m_protocol)
            {
                case OP_EDONKEYPROT:
                case OP_EMULEPROT:
                {
                    m_in_container.resize(m_in_header.m_size - 1);
                    boost::asio::async_read(m_socket, boost::asio::buffer(&m_in_container[0], m_in_header.m_size - 1),
                            boost::bind(&server_connection::handle_read_packet, self(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
                    break;
                }
                case OP_PACKEDPROT:
                {
                    m_in_gzip_container.resize(m_in_header.m_size - 1);
                    boost::asio::async_read(m_socket, boost::asio::buffer(&m_in_gzip_container[0], m_in_header.m_size - 1),
                            boost::bind(&server_connection::handle_read_packet, self(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
                    break;
                }
                default:
                    close(errors::invalid_protocol_type);
                    break;
            }

        }
        else
        {
            close(error);
        }
    }

    void server_connection::handle_read_packet(const error_code& error, size_t nSize)
    {
        if (is_stopped()) return;

        if (!error)
        {
            DBG("server_connection::handle_read_packet(" << error.message() << ", " << nSize << ", " << packetToString(m_in_header.m_type));

            if (m_in_header.m_protocol == OP_PACKEDPROT)
            {
                // unzip data
                int nRet = inflate_gzip(m_in_gzip_container, m_in_container, LIBED2K_SERVER_CONN_MAX_SIZE);

                if (nRet != Z_STREAM_END)
                {
                    //unpack error - pass packet
                    do_read();
                    return;
                }
            }

            boost::iostreams::stream_buffer<Device> buffer(&m_in_container[0], m_in_container.size());
            std::istream in_array_stream(&buffer);
            archive::ed2k_iarchive ia(in_array_stream);

            try
            {
                // dispatch message
                switch (m_in_header.m_type)
                {
                    case OP_REJECT:
                        DBG("ignore");
                        break;
                    case OP_DISCONNECT:
                        DBG("ignore");
                        break;
                    case OP_SERVERMESSAGE:
                    {
                        server_message smsg;
                        ia >> smsg;
                        if (m_ses.m_alerts.should_post<server_message_alert>())
                                m_ses.m_alerts.post_alert(server_message_alert(smsg.m_strMessage));
                        break;
                    }
                    case OP_SERVERLIST:
                    {
                        server_list slist;
                        ia >> slist;

                        break;
                    }
                    case OP_SERVERSTATUS:
                    {
                        server_status sss;
                        ia >> sss;
                        m_nFilesCount = sss.m_nFilesCount;
                        m_nUsersCount = sss.m_nUserCount;

                        if (m_nClientId != 0)
                        {
                            // we already got client id it means
                            // server connection initialized
                            // notyfy session
                            m_ses.server_ready(m_nClientId, m_nFilesCount, m_nUsersCount, m_nTCPFlags, m_nAuxPort);
                            // do not send udp packets now
                            //global_server_state_req gssr;
                            //do_write_udp(gssr);
                            //do_read_udp();
                        }
                        break;
                    }
                    case OP_USERS_LIST:
                        DBG("ignore");
                        break;
                    case OP_IDCHANGE:
                    {
                        id_change idc(m_in_header.m_size - 1);
                        ia >> idc;

                        m_nClientId = idc.m_nClientId;
                        m_nTCPFlags = idc.m_nTCPFlags;
                        m_nAuxPort  = idc.m_nAuxPort;

                        DBG("Client id: " << m_nClientId << " tcp flags: " << idc.m_nTCPFlags << " aux port " << idc.m_nAuxPort);

                        if (m_nUsersCount != 0)
                        {
                            DBG("users count " << m_nUsersCount);
                            // if we got users count != 0 - at least 1 user must exists on server
                            // (our connection) - server connection initialized
                            // notify session
                            m_ses.server_ready(m_nClientId, m_nFilesCount, m_nUsersCount, m_nTCPFlags, m_nAuxPort);
                            // do now send udp packets now
                            //global_server_state_req gssr;
                            //do_write_udp(gssr);
                            //do_read_udp();
                        }
                        break;
                    }
                    case OP_SERVERIDENT:
                    {
                        server_info_entry se;
                        ia >> se;
                        break;
                    }
                    case OP_FOUNDSOURCES:
                    {
                        found_file_sources fs;
                        ia >> fs;
                        fs.dump();

                        // ok, search appropriate transfer
                        if (boost::shared_ptr<transfer> p = m_ses.find_transfer(fs.m_hFile).lock())
                        {
                            p->set_sources(fs);
                        }

                        break;
                    }
                    case OP_SEARCHRESULT:
                    {
                        search_file_list sfl;
                        ia >> sfl;
                        m_ses.m_alerts.post_alert_should(search_result_alert(sfl));
                        break;
                    }
                    case OP_CALLBACKREQUESTED:
                        break;
                    default:
                        DBG("ignore unhandled packet");
                        break;
                }

                m_in_gzip_container.clear();
                m_in_container.clear();

                do_read();

            }
            catch(libed2k_exception& e)
            {
                ERR("packet parse error");
                close(errors::decode_packet_error);
            }
        }
        else
        {
            close(error);
        }
    }


   void server_connection::check_deadline()
   {
       if (is_stopped())
           return;

       // Check whether the deadline has passed. We compare the deadline against
       // the current time since a new asynchronous operation may have moved the
       // deadline before this actor had a chance to run.

       if (m_deadline.expires_at() <= dtimer::traits_type::now())
       {
           DBG("server_connection::check_deadline(): deadline timer expired");

           // The deadline has passed. The socket is closed so that any outstanding
           // asynchronous operations are cancelled.
           close(errors::timed_out);
           // There is no longer an active deadline. The expiry is set to positive
           // infinity so that the actor takes no action until a new deadline is set.
           m_deadline.expires_at(boost::posix_time::pos_infin);
           boost::system::error_code ignored_ec;
       }

       // Put the actor back to sleep.
       m_deadline.async_wait(boost::bind(&server_connection::check_deadline, self()));
   }

   void server_connection::handle_write_udp(const error_code& error, size_t nSize)
   {
       if (is_stopped()) return;
       //if (!m_udp_socket.is_open()) return;

       if (!error)
       {
           m_udp_order.pop_front();

           if (!m_udp_order.empty())
           {
               std::vector<boost::asio::const_buffer> buffers;
               buffers.push_back(boost::asio::buffer(&m_udp_order.front().first, header_size));
               buffers.push_back(boost::asio::buffer(m_udp_order.front().second));
               m_udp_socket.async_send_to(buffers, m_udp_target, boost::bind(&server_connection::handle_write_udp, self(),
                                  boost::asio::placeholders::error,
                                  boost::asio::placeholders::bytes_transferred));
           }
       }
       else
       {
           close(error);

       }
   }

   void server_connection::do_read_udp()
   {
    //   if (m_udp_socket.is_open())
       {
           DBG("server_connection::do_read_udp()");
           m_udp_socket.async_receive(boost::asio::buffer(&m_in_udp_header, header_size),
                              boost::bind(&server_connection::handle_read_header_udp,
                                      self(),
                                      boost::asio::placeholders::error,
                                      boost::asio::placeholders::bytes_transferred));
       }
   }

   void server_connection::handle_read_header_udp(const error_code& error, size_t nSize)
   {
       if (is_stopped()) return;


       DBG("server_connection::handle_read_header_udp(" << error.message() << ")");

       if (!error)
       {
          if (m_in_udp_container.size() < m_in_udp_header.m_size - 1)
          {
              m_in_udp_container.resize(m_in_udp_header.m_size - 1);
          }

          m_udp_socket.async_receive(boost::asio::buffer(&m_in_udp_container[0], m_in_udp_header.m_size - 1),
                  boost::bind(&server_connection::handle_read_packet_udp, self(), boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
       }
       else
       {
           close(error);
       }
   }


   void server_connection::handle_read_packet_udp(const error_code& error, size_t nSize)
   {
       if (!error)
       {
           DBG("server_connection::handle_read_packet_udp(" << error.message() << ", " << nSize << ", "); // << packetToString(m_in_udp_header.m_type));
           boost::iostreams::stream_buffer<Device> buffer(&m_in_udp_container[0], m_in_udp_header.m_size - 1);
           std::istream in_array_stream(&buffer);
           archive::ed2k_iarchive ia(in_array_stream);

           try
           {
               switch(m_in_udp_header.m_type)
               {
                   case OP_GLOBSERVSTATRES:
                       {
                           DBG("receive: OP_GLOBSERVSTATRES");
                           global_server_state_res gres(m_in_udp_header.m_size - 1);
                           ia >> gres;
                           break;
                       }
                   default:
                       DBG("receive " << m_in_udp_header.m_type);
                       break;
               }

           }
           catch(libed2k_exception& e)
           {
               ERR("packet parse error");
           }

           do_read_udp();
       }
   }
}

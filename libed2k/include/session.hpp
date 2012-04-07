// ed2k session

#ifndef __LIBED2K_SESSION__
#define __LIBED2K_SESSION__

#include <string>
#include <boost/shared_ptr.hpp>

#include <libtorrent/storage.hpp>

#include "fingerprint.hpp"
#include "md4_hash.hpp"

namespace libed2k {

    typedef libtorrent::storage_constructor_type storage_constructor_type;

    namespace fs = boost::filesystem;

    namespace aux {
        class session_impl;
    }

    enum storage_mode_t
    {
        storage_mode_allocate = 0,
        storage_mode_sparse,
        storage_mode_compact
    };

    struct add_transfer_params
    {
        add_transfer_params(storage_constructor_type sc =
                            libtorrent::default_storage_constructor)
            : resume_data(0)
            , storage_mode(storage_mode_sparse)
            , duplicate_is_error(false)
            , storage(sc)
            , upload_mode(false)
        {}

        md4_hash info_hash;
        fs::path save_path;
        std::vector<char>* resume_data;
        storage_mode_t storage_mode;
        bool duplicate_is_error;
        storage_constructor_type storage;
        bool upload_mode;
    };


    // Once it's created, the session object will spawn the main thread
    // that will do all the work. The main thread will be idle as long 
    // it doesn't have any transfers to participate in.
    // TODO: should inherit the session_base interfase in future
    class session
    {
    public:
        session(int listen_port, const char* listen_interface,
                const fingerprint& id, const std::string& logpath = ".")
        {
            init(listen_port, listen_interface, id, logpath);
        }

    private:
        void init(int listen_port, const char* listen_interface,
                  const fingerprint& id, const std::string& logpath);

		// data shared between the main thread
		// and the working thread
        boost::shared_ptr<aux::session_impl> m_impl;
    };
}

#endif

// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef RPC_CONNECTIVITY_CLUSTER_HPP_
#define RPC_CONNECTIVITY_CLUSTER_HPP_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "arch/types.hpp"
#include "concurrency/auto_drainer.hpp"
#include "concurrency/mutex.hpp"
#include "concurrency/one_per_thread.hpp"
#include "concurrency/watchable.hpp"
#include "containers/archive/tcp_conn_stream.hpp"
#include "containers/map_sentries.hpp"
#include "perfmon/perfmon.hpp"
#include "rpc/connectivity/peer_id.hpp"
#include "utils.hpp"

namespace boost {
template <class> class optional;
}

class co_semaphore_t;

class cluster_message_handler_t;

class cluster_send_message_write_callback_t {
public:
    virtual ~cluster_send_message_write_callback_t() { }
    virtual void write(cluster_version_t cluster_version,
                       write_stream_t *stream) = 0;
};

/* `connectivity_cluster_t` is responsible for establishing connections with other
machines and communicating with them. It's the foundation of the entire clustering
system. However, it's very low-level; most code will instead use the directory or mailbox
mechanisms, which are built on top of `connectivity_cluster_t`.

Clustering is based around the concept of a "connection", as represented by
`connectivity_cluster_t::connection_t`. When the `cluster_t::run_t` is constructed, we
automatically create a `connection_t` to ourself, the "loopback connection". We also
accept TCP connections on some port. When we get a TCP connection, we perform a
handshake; if this succeeds, then we create a `connection_t` to represent the new
connection. Once a connection is established, messages can be sent across it in both
directions. Every message is guaranteed to eventually arrive unless the connection goes
down. Messages cannot be duplicated.

Can messages be reordered? I think the current implementation doesn't ever reorder
messages, but don't rely on this guarantee. However, some old code may rely on this
guarantee (I'm not sure) so don't break this property without checking first. */

class connectivity_cluster_t :
    public home_thread_mixin_debug_only_t
{
public:
    static const std::string cluster_proto_header;
    static const std::string cluster_version_string;
    static const std::string cluster_arch_bitsize;
    static const std::string cluster_build_mode;

    /* Every clustering message has a "tag", which determines what message handler on the
    destination machine will deal with it. Tags are a low-level concept, and there are
    only a few of them; for example, all directory-related messages have one tag, and all
    mailbox-related messages have another. Higher-level code uses the mailbox system for
    routing messages. */
    typedef uint8_t message_tag_t;
    static const int max_message_tag = 256;

    /* This tag is reserved exclusively for heartbeat messages. */
    static const message_tag_t heartbeat_tag = 'H';

    class run_t;

    /* `connection_t` represents an open connection to another machine. If we lose
    contact with another machine and then regain it, then a new `connection_t` will be
    created. Generally, any code that handles a `connection_t *` will also carry around a
    `auto_drainer_t::lock_t` that ensures the connection object isn't destroyed while in
    use. This doubles as a mechanism for finding out when the connection has been lost;
    if the connection dies, the `auto_drainer_t::lock_t` will pulse its
    `get_drain_signal()`. There will never be two `connection_t` objects that refer to
    the same peer.

    `connection_t` is completely thread-safe. You can pass connections from thread to
    thread and call the methods on any thread. */
    class connection_t : public home_thread_mixin_debug_only_t {
    public:
        /* Returns the peer ID of the other machine. Peer IDs change when a node
        restarts, but not when it loses and then regains contact. */
        peer_id_t get_peer_id() {
            return peer_id;
        }

        /* Returns the address of the other machine. */
        peer_address_t get_peer_address() {
            return peer_address;
        }

        /* Returns `true` if this is the loopback connection */
        bool is_loopback() {
            return conn == NULL;
        }

        /* Drops the connection. */
        void kill_connection();

    private:
        friend class connectivity_cluster_t;

        /* The constructor registers us in every thread's `connections` map, thereby
        notifying event subscribers. */
        connection_t(run_t *, peer_id_t, keepalive_tcp_conn_stream_t *,
                const peer_address_t &peer) THROWS_NOTHING;
        ~connection_t() THROWS_NOTHING;

        /* NULL for the loopback connection (i.e. our "connection" to ourself) */
        keepalive_tcp_conn_stream_t *conn;

        /* `connection_t` contains the addresses so that we can call
        `get_peers_list()` on any thread. Otherwise, we would have to go
        cross-thread to access the routing table. */
        peer_address_t peer_address;

        /* Unused for our connection to ourself */
        mutex_t send_mutex;

        perfmon_collection_t pm_collection;
        perfmon_sampler_t pm_bytes_sent;
        perfmon_membership_t pm_collection_membership, pm_bytes_sent_membership;

        /* We only hold this information so we can deregister ourself */
        run_t *parent;
        
        peer_id_t peer_id;

        one_per_thread_t<auto_drainer_t> drainers;
    };

    /* Construct one `run_t` for each `connectivity_cluster_t` after setting up the
    message handlers. The `run_t`'s constructor is what actually starts listening for
    connections from other nodes, and the destructor is what stops listening. This way,
    we use RAII to ensure that we stop sending messages to the message handlers before we
    destroy the message handlers. */
    class run_t {
    public:
        run_t(connectivity_cluster_t *parent,
              const std::set<ip_address_t> &local_addresses,
              const peer_address_t &canonical_addresses,
              int port, int client_port)
            THROWS_ONLY(address_in_use_exc_t, tcp_socket_exc_t);

        ~run_t();

        /* Attaches the cluster this node is part of to another existing
        cluster. May only be called on home thread. Returns immediately (it does
        its work in the background). */
        void join(const peer_address_t &address) THROWS_NOTHING;

        std::set<ip_and_port_t> get_ips() const;
        int get_port();

    private:
        friend class connectivity_cluster_t;

        /* Sets a variable to a value in its constructor; sets it to NULL in its
        destructor. This is kind of silly. The reason we need it is that we need
        the variable to be set before the constructors for some other fields of
        the `run_t` are constructed. */
        class variable_setter_t {
        public:
            variable_setter_t(run_t **var, run_t *val) : variable(var) , value(val) {
                guarantee(*variable == NULL);
                *variable = value;
            }

            ~variable_setter_t() THROWS_NOTHING {
                guarantee(*variable == value);
                *variable = NULL;
            }
        private:
            run_t **variable;
            run_t *value;
            DISABLE_COPYING(variable_setter_t);
        };

        void on_new_connection(const scoped_ptr_t<tcp_conn_descriptor_t> &nconn,
                auto_drainer_t::lock_t lock) THROWS_NOTHING;

        /* `connect_to_peer` is spawned for each known ip address of a peer which we want
        to connect to, all but one should fail */
        void connect_to_peer(const peer_address_t *addr,
                             int index,
                             boost::optional<peer_id_t> expected_id,
                             auto_drainer_t::lock_t drainer_lock,
                             bool *successful_join,
                             co_semaphore_t *rate_control) THROWS_NOTHING;

        /* `join_blocking()` is spawned in a new coroutine by `join()`. It's also run by
        `handle()` when we hear about a new peer from a peer we are connected to. */
        void join_blocking(const peer_address_t hosts,
                           boost::optional<peer_id_t>,
                           auto_drainer_t::lock_t) THROWS_NOTHING;

        // Normal routing table isn't serializable, so we send just the hosts/ports
        bool get_routing_table_to_send_and_add_peer(const peer_id_t &other_peer_id,
                                                    const peer_address_t &other_peer_addr,
                                                    object_buffer_t<map_insertion_sentry_t<peer_id_t, peer_address_t> > *routing_table_entry_sentry,
                                                    std::map<peer_id_t, std::set<host_and_port_t> > *result);

        /* `handle()` takes an `auto_drainer_t::lock_t` so that we never shut
        down while there are still running instances of `handle()`. It's
        responsible for the entire lifetime of an intra-cluster TCP connection.
        It handles the handshake, exchanging node maps, sending out the
        connect-notification, receiving messages from the peer until it
        disconnects or we are shut down, and sending out the
        disconnect-notification. */
        void handle(keepalive_tcp_conn_stream_t *c,
            boost::optional<peer_id_t> expected_id,
            boost::optional<peer_address_t> expected_address,
            auto_drainer_t::lock_t,
            bool *successful_join) THROWS_NOTHING;

        connectivity_cluster_t *parent;

        /* `attempt_table` is a table of all the host:port pairs we're currently
        trying to connect to or have connected to. If we are told to connect to
        an address already in this table, we'll just ignore it. That's important
        because when `client_port` is specified we will make all of our
        connections from the same source, and TCP might not be able to
        disambiguate between them. */
        peer_address_set_t attempt_table;
        mutex_assertion_t attempt_table_mutex;

        /* `routing_table` is all the peers we can currently access and their
        addresses. Peers that are in the process of connecting or disconnecting
        may be in `routing_table` but not in
        `parent->thread_info.get()->connection_map`. */
        std::map<peer_id_t, peer_address_t> routing_table;

        /* Writes to `routing_table` are protected by this mutex so we never get
        redundant connections to the same peer. */
        mutex_t new_connection_mutex;

        scoped_ptr_t<tcp_bound_socket_t> cluster_listener_socket;
        int cluster_listener_port;
        int cluster_client_port;

        variable_setter_t register_us_with_parent;

        map_insertion_sentry_t<peer_id_t, peer_address_t> routing_table_entry_for_ourself;
        connection_t connection_to_ourself;

        /* For picking random threads */
        rng_t rng;

        auto_drainer_t drainer;

        /* This must be destroyed before `drainer` is. */
        scoped_ptr_t<tcp_listener_t> listener;
    };

    connectivity_cluster_t() THROWS_NOTHING;
    ~connectivity_cluster_t() THROWS_NOTHING;

    peer_id_t get_me() THROWS_NOTHING;

    /* This returns a watchable table of every active connection. The returned
    `watchable_t` will be valid for the thread that `get_connections()` was called on. */
    typedef std::map<peer_id_t, std::pair<connection_t *, auto_drainer_t::lock_t> >
            connection_map_t;
    clone_ptr_t<watchable_t<connection_map_t> > get_connections() THROWS_NOTHING;

    /* Shortcut if you just want to access one connection, which is by far the most
    common case. Returns `NULL` if there is no active connection to the given peer. */
    connection_t *get_connection(peer_id_t peer,
            auto_drainer_t::lock_t *keepalive_out) THROWS_NOTHING;

    /* Sends a message to the other machine. The message is associated with a "tag",
    which determines which message handler on the other machine will receive the message.
    */
    void send_message(connection_t *connection,
                      auto_drainer_t::lock_t connection_keepalive,
                      message_tag_t tag,
                      cluster_send_message_write_callback_t *callback);

private:
    friend class cluster_message_handler_t;
    friend class run_t;

    class heartbeat_manager_t;

    /* `me` is our `peer_id_t`. */
    const peer_id_t me;

    /* `connections` holds open connections to other peers. It's the same on every
    thread. It has an entry for every peer we are fully and officially connected to,
    including us. That means it's a subset of the entries in `run_t::routing_table`. It
    also holds an `auto_drainer_t::lock_t` for each connection; that way, the connection
    can make sure nobody acquires a lock on its `auto_drainer_t` after it removes itself
    from `connections`. */
    one_per_thread_t<watchable_variable_t<connection_map_t> > connections;

    cluster_message_handler_t *message_handlers[max_message_tag];

#ifndef NDEBUG
    rng_t debug_rng;
#endif

    run_t *current_run;

    perfmon_collection_t connectivity_collection;
    perfmon_membership_t stats_membership;

    DISABLE_COPYING(connectivity_cluster_t);
};

/* Subclass `cluster_message_handler_t` to handle messages received over the network. The
`cluster_message_handler_t` constructor will automatically register it to handle
messages. You can only register and unregister message handlers when there is no `run_t`
in existence. */
class cluster_message_handler_t {
public:
    connectivity_cluster_t *get_connectivity_cluster() {
        return connectivity_cluster;
    }
    connectivity_cluster_t::message_tag_t get_message_tag() { return tag; }

protected:
    /* Registers the message handler with the cluster */
    cluster_message_handler_t(connectivity_cluster_t *connectivity_cluster,
                              connectivity_cluster_t::message_tag_t tag);
    virtual ~cluster_message_handler_t();

    /* This can be called on any thread. */
    virtual void on_message(connectivity_cluster_t::connection_t *conn,
                            auto_drainer_t::lock_t keepalive,
                            cluster_version_t version,
                            read_stream_t *) = 0;

    /* The default implementation constructs a stream reading from `data` and then
    calls `on_message()`. Override to optimize for the local case. */
    virtual void on_local_message(connectivity_cluster_t::connection_t *conn,
                                  auto_drainer_t::lock_t keepalive,
                                  cluster_version_t version,
                                  std::vector<char> &&data);

private:
    friend class connectivity_cluster_t;
    connectivity_cluster_t *connectivity_cluster;
    const connectivity_cluster_t::message_tag_t tag;
};

#endif /* RPC_CONNECTIVITY_CLUSTER_HPP_ */


// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <net_processing.h>

#include <addrman.h>
#include <avalanche/avalanche.h>
#include <avalanche/compactproofs.h>
#include <avalanche/peermanager.h>
#include <avalanche/processor.h>
#include <avalanche/proof.h>
#include <avalanche/statistics.h>
#include <avalanche/validation.h>
#include <banman.h>
#include <blockencodings.h>
#include <blockfilter.h>
#include <blockvalidity.h>
#include <chain.h>
#include <chainparams.h>
#include <config.h>
#include <consensus/amount.h>
#include <consensus/validation.h>
#include <hash.h>
#include <index/blockfilterindex.h>
#include <invrequest.h>
#include <merkleblock.h>
#include <netbase.h>
#include <netmessagemaker.h>
#include <node/blockstorage.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <random.h>
#include <reverse_iterator.h>
#include <scheduler.h>
#include <streams.h>
#include <tinyformat.h>
#include <txmempool.h>
#include <txorphanage.h>
#include <util/check.h> // For NDEBUG compile time check
#include <util/strencodings.h>
#include <util/system.h>
#include <util/trace.h>
#include <validation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <typeinfo>

using node::fImporting;
using node::fPruneMode;
using node::fReindex;
using node::ReadBlockFromDisk;

/** How long to cache transactions in mapRelay for normal relay */
static constexpr auto RELAY_TX_CACHE_TIME = 15min;
/**
 * How long a transaction has to be in the mempool before it can
 * unconditionally be relayed (even when not in mapRelay).
 */
static constexpr auto UNCONDITIONAL_RELAY_DELAY = 2min;
/**
 * Headers download timeout.
 * Timeout = base + per_header * (expected number of headers)
 */
static constexpr auto HEADERS_DOWNLOAD_TIMEOUT_BASE = 15min;
static constexpr auto HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER = 1ms;
/**
 * Protect at least this many outbound peers from disconnection due to
 * slow/behind headers chain.
 */
static constexpr int32_t MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT = 4;
/**
 * Timeout for (unprotected) outbound peers to sync to our chainwork, in
 * seconds.
 */
// 20 minutes
static constexpr int64_t CHAIN_SYNC_TIMEOUT = 20 * 60;
/** How frequently to check for stale tips */
static constexpr auto STALE_CHECK_INTERVAL{10min};
/** How frequently to check for extra outbound peers and disconnect. */
static constexpr auto EXTRA_PEER_CHECK_INTERVAL{45s};
/**
 * Minimum time an outbound-peer-eviction candidate must be connected for, in
 * order to evict, in seconds.
 */
static constexpr std::chrono::seconds MINIMUM_CONNECT_TIME{30};
/** SHA256("main address relay")[0:8] */
static constexpr uint64_t RANDOMIZER_ID_ADDRESS_RELAY = 0x3cac0035b5866b90ULL;
/// Age after which a stale block will no longer be served if requested as
/// protection against fingerprinting. Set to one month, denominated in seconds.
static constexpr int STALE_RELAY_AGE_LIMIT = 30 * 24 * 60 * 60;
/// Age after which a block is considered historical for purposes of rate
/// limiting block relay. Set to one week, denominated in seconds.
static constexpr int HISTORICAL_BLOCK_AGE = 7 * 24 * 60 * 60;
/**
 * Time between pings automatically sent out for latency probing and keepalive.
 */
static constexpr std::chrono::minutes PING_INTERVAL{2};
/** The maximum number of entries in a locator */
static const unsigned int MAX_LOCATOR_SZ = 101;
/** The maximum number of entries in an 'inv' protocol message */
static const unsigned int MAX_INV_SZ = 50000;
static_assert(MAX_PROTOCOL_MESSAGE_LENGTH > MAX_INV_SZ * sizeof(CInv),
              "Max protocol message length must be greater than largest "
              "possible INV message");

/** Minimum time between 2 successives getavaaddr messages from the same peer */
static constexpr std::chrono::minutes GETAVAADDR_INTERVAL{2};

/**
 * If no proof was requested from a compact proof message after this timeout
 * expired, the proof radix tree can be cleaned up.
 */
static constexpr std::chrono::minutes AVALANCHE_AVAPROOFS_TIMEOUT{2};

struct DataRequestParameters {
    /**
     * Maximum number of in-flight data requests from a peer. It is not a hard
     * limit, but the threshold at which point the overloaded_peer_delay kicks
     * in.
     */
    const size_t max_peer_request_in_flight;

    /**
     * Maximum number of inventories to consider for requesting, per peer. It
     * provides a reasonable DoS limit to per-peer memory usage spent on
     * announcements, while covering peers continuously sending INVs at the
     * maximum rate (by our own policy, see INVENTORY_BROADCAST_PER_SECOND) for
     * several minutes, while not receiving the actual data (from any peer) in
     * response to requests for them.
     */
    const size_t max_peer_announcements;

    /** How long to delay requesting data from non-preferred peers */
    const std::chrono::seconds nonpref_peer_delay;

    /**
     * How long to delay requesting data from overloaded peers (see
     * max_peer_request_in_flight).
     */
    const std::chrono::seconds overloaded_peer_delay;

    /**
     * How long to wait (in microseconds) before a data request from an
     * additional peer.
     */
    const std::chrono::microseconds getdata_interval;

    /**
     * Permission flags a peer requires to bypass the request limits tracking
     * limits and delay penalty.
     */
    const NetPermissionFlags bypass_request_limits_permissions;
};

static constexpr DataRequestParameters TX_REQUEST_PARAMS{
    100,                       // max_peer_request_in_flight
    5000,                      // max_peer_announcements
    std::chrono::seconds(2),   // nonpref_peer_delay
    std::chrono::seconds(2),   // overloaded_peer_delay
    std::chrono::seconds(60),  // getdata_interval
    NetPermissionFlags::Relay, // bypass_request_limits_permissions
};

static constexpr DataRequestParameters PROOF_REQUEST_PARAMS{
    100,                      // max_peer_request_in_flight
    5000,                     // max_peer_announcements
    std::chrono::seconds(2),  // nonpref_peer_delay
    std::chrono::seconds(2),  // overloaded_peer_delay
    std::chrono::seconds(60), // getdata_interval
    NetPermissionFlags::
        BypassProofRequestLimits, // bypass_request_limits_permissions
};

/**
 * Limit to avoid sending big packets. Not used in processing incoming GETDATA
 * for compatibility.
 */
static const unsigned int MAX_GETDATA_SZ = 1000;
/**
 * Number of blocks that can be requested at any given time from a single peer.
 */
static const int MAX_BLOCKS_IN_TRANSIT_PER_PEER = 16;
/**
 * Time during which a peer must stall block download progress before being
 * disconnected.
 */
static constexpr auto BLOCK_STALLING_TIMEOUT = 2s;
/**
 * Number of headers sent in one getheaders result. We rely on the assumption
 * that if a peer sends
 *  less than this number, we reached its tip. Changing this value is a protocol
 * upgrade.
 */
static const unsigned int MAX_HEADERS_RESULTS = 2000;
/**
 * Maximum depth of blocks we're willing to serve as compact blocks to peers
 *  when requested. For older blocks, a regular BLOCK response will be sent.
 */
static const int MAX_CMPCTBLOCK_DEPTH = 5;
/**
 * Maximum depth of blocks we're willing to respond to GETBLOCKTXN requests
 * for.
 */
static const int MAX_BLOCKTXN_DEPTH = 10;
/**
 * Size of the "block download window": how far ahead of our current height do
 * we fetch? Larger windows tolerate larger download speed differences between
 * peer, but increase the potential degree of disordering of blocks on disk
 * (which make reindexing and pruning harder). We'll probably
 *  want to make this a per-peer adaptive value at some point.
 */
static const unsigned int BLOCK_DOWNLOAD_WINDOW = 1024;
/**
 * Block download timeout base, expressed in multiples of the block interval
 * (i.e. 10 min)
 */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_BASE = 1;
/**
 * Additional block download timeout per parallel downloading peer (i.e. 5 min)
 */
static constexpr double BLOCK_DOWNLOAD_TIMEOUT_PER_PEER = 0.5;
/**
 * Maximum number of headers to announce when relaying blocks with headers
 * message.
 */
static const unsigned int MAX_BLOCKS_TO_ANNOUNCE = 8;
/** Maximum number of unconnecting headers announcements before DoS score */
static const int MAX_UNCONNECTING_HEADERS = 10;
/** Minimum blocks required to signal NODE_NETWORK_LIMITED */
static const unsigned int NODE_NETWORK_LIMITED_MIN_BLOCKS = 288;
/**
 * Average delay between local address broadcasts.
 */
static constexpr auto AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL = 24h;
/**
 * Average delay between peer address broadcasts.
 */
static constexpr auto AVG_ADDRESS_BROADCAST_INTERVAL = 30s;
/**
 * Average delay between trickled inventory transmissions for inbound peers.
 * Blocks and peers with NetPermissionFlags::NoBan permission bypass this.
 */
static constexpr auto INBOUND_INVENTORY_BROADCAST_INTERVAL = 5s;
/**
 * Maximum rate of inventory items to send per second.
 * Limits the impact of low-fee transaction floods.
 */
static constexpr unsigned int INVENTORY_BROADCAST_PER_SECOND = 7;
/** Maximum number of inventory items to send per transmission. */
static constexpr unsigned int INVENTORY_BROADCAST_MAX_PER_MB =
    INVENTORY_BROADCAST_PER_SECOND *
    count_seconds(INBOUND_INVENTORY_BROADCAST_INTERVAL);
/** The number of most recently announced transactions a peer can request. */
static constexpr unsigned int INVENTORY_MAX_RECENT_RELAY = 3500;
/**
 * Verify that INVENTORY_MAX_RECENT_RELAY is enough to cache everything
 * typically relayed before unconditional relay from the mempool kicks in. This
 * is only a lower bound, and it should be larger to account for higher inv rate
 * to outbound peers, and random variations in the broadcast mechanism.
 */
static_assert(INVENTORY_MAX_RECENT_RELAY >= INVENTORY_BROADCAST_PER_SECOND *
                                                UNCONDITIONAL_RELAY_DELAY /
                                                std::chrono::seconds{1},
              "INVENTORY_RELAY_MAX too low");

/**
 * Average delay between feefilter broadcasts in seconds.
 */
static constexpr auto AVG_FEEFILTER_BROADCAST_INTERVAL = 10min;
/**
 * Maximum feefilter broadcast delay after significant change.
 */
static constexpr auto MAX_FEEFILTER_CHANGE_DELAY = 5min;
/**
 * Maximum number of compact filters that may be requested with one
 * getcfilters. See BIP 157.
 */
static constexpr uint32_t MAX_GETCFILTERS_SIZE = 1000;
/**
 * Maximum number of cf hashes that may be requested with one getcfheaders. See
 * BIP 157.
 */
static constexpr uint32_t MAX_GETCFHEADERS_SIZE = 2000;
/**
 * the maximum percentage of addresses from our addrman to return in response
 * to a getaddr message.
 */
static constexpr size_t MAX_PCT_ADDR_TO_SEND = 23;
/** The maximum number of address records permitted in an ADDR message. */
static constexpr size_t MAX_ADDR_TO_SEND{1000};
/**
 * The maximum rate of address records we're willing to process on average. Can
 * be bypassed using the NetPermissionFlags::Addr permission.
 */
static constexpr double MAX_ADDR_RATE_PER_SECOND{0.1};
/**
 * The soft limit of the address processing token bucket (the regular
 * MAX_ADDR_RATE_PER_SECOND based increments won't go above this, but the
 * MAX_ADDR_TO_SEND increment following GETADDR is exempt from this limit).
 */
static constexpr size_t MAX_ADDR_PROCESSING_TOKEN_BUCKET{MAX_ADDR_TO_SEND};

inline size_t GetMaxAddrToSend() {
    return gArgs.GetIntArg("-maxaddrtosend", MAX_ADDR_TO_SEND);
}

// Internal stuff
namespace {
/**
 * Blocks that are in flight, and that are in the queue to be downloaded.
 */
struct QueuedBlock {
    /**
     * BlockIndex. We must have this since we only request blocks when we've
     * already validated the header.
     */
    const CBlockIndex *pindex;
    /** Optional, used for CMPCTBLOCK downloads */
    std::unique_ptr<PartiallyDownloadedBlock> partialBlock;
};

/**
 * Data structure for an individual peer. This struct is not protected by
 * cs_main since it does not contain validation-critical data.
 *
 * Memory is owned by shared pointers and this object is destructed when
 * the refcount drops to zero.
 *
 * Mutexes inside this struct must not be held when locking m_peer_mutex.
 *
 * TODO: move most members from CNodeState to this structure.
 * TODO: move remaining application-layer data members from CNode to this
 * structure.
 */
struct Peer {
    /** Same id as the CNode object for this peer */
    const NodeId m_id{0};

    /** Protects misbehavior data members */
    Mutex m_misbehavior_mutex;
    /** Accumulated misbehavior score for this peer */
    int m_misbehavior_score GUARDED_BY(m_misbehavior_mutex){0};
    /** Whether this peer should be disconnected and marked as discouraged
     * (unless it has NetPermissionFlags::NoBan permission). */
    bool m_should_discourage GUARDED_BY(m_misbehavior_mutex){false};

    /** Protects block inventory data members */
    Mutex m_block_inv_mutex;
    /**
     * List of blocks that we'll anounce via an `inv` message.
     * There is no final sorting before sending, as they are always sent
     * immediately and in the order requested.
     */
    std::vector<BlockHash> m_blocks_for_inv_relay GUARDED_BY(m_block_inv_mutex);
    /**
     * Unfiltered list of blocks that we'd like to announce via a `headers`
     * message. If we can't announce via a `headers` message, we'll fall back to
     * announcing via `inv`.
     */
    std::vector<BlockHash>
        m_blocks_for_headers_relay GUARDED_BY(m_block_inv_mutex);

    /**
     * The final block hash that we sent in an `inv` message to this peer.
     * When the peer requests this block, we send an `inv` message to trigger
     * the peer to request the next sequence of block hashes.
     * Most peers use headers-first syncing, which doesn't use this mechanism
     */
    BlockHash m_continuation_block GUARDED_BY(m_block_inv_mutex){};

    /** This peer's reported block height when we connected */
    std::atomic<int> m_starting_height{-1};

    /** The pong reply we're expecting, or 0 if no pong expected. */
    std::atomic<uint64_t> m_ping_nonce_sent{0};
    /** When the last ping was sent, or 0 if no ping was ever sent */
    std::atomic<std::chrono::microseconds> m_ping_start{0us};
    /** Whether a ping has been requested by the user */
    std::atomic<bool> m_ping_queued{false};

    /**
     * A vector of addresses to send to the peer, limited to MAX_ADDR_TO_SEND.
     */
    std::vector<CAddress> m_addrs_to_send;
    /**
     * Probabilistic filter to track recent addr messages relayed with this
     * peer. Used to avoid relaying redundant addresses to this peer.
     *
     *  We initialize this filter for outbound peers (other than
     *  block-relay-only connections) or when an inbound peer sends us an
     *  address related message (ADDR, ADDRV2, GETADDR).
     *
     *  Presence of this filter must correlate with m_addr_relay_enabled.
     **/
    std::unique_ptr<CRollingBloomFilter> m_addr_known;
    /**
     * Whether we are participating in address relay with this connection.
     *
     * We set this bool to true for outbound peers (other than
     * block-relay-only connections), or when an inbound peer sends us an
     * address related message (ADDR, ADDRV2, GETADDR).
     *
     * We use this bool to decide whether a peer is eligible for gossiping
     * addr messages. This avoids relaying to peers that are unlikely to
     * forward them, effectively blackholing self announcements. Reasons
     * peers might support addr relay on the link include that they connected
     * to us as a block-relay-only peer or they are a light client.
     *
     * This field must correlate with whether m_addr_known has been
     * initialized.
     */
    std::atomic_bool m_addr_relay_enabled{false};
    /** Whether a getaddr request to this peer is outstanding. */
    bool m_getaddr_sent{false};
    /** Guards address sending timers. */
    mutable Mutex m_addr_send_times_mutex;
    /** Time point to send the next ADDR message to this peer. */
    std::chrono::microseconds
        m_next_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /** Time point to possibly re-announce our local address to this peer. */
    std::chrono::microseconds
        m_next_local_addr_send GUARDED_BY(m_addr_send_times_mutex){0};
    /**
     * Whether the peer has signaled support for receiving ADDRv2 (BIP155)
     * messages, indicating a preference to receive ADDRv2 instead of ADDR ones.
     */
    std::atomic_bool m_wants_addrv2{false};
    /** Whether this peer has already sent us a getaddr message. */
    bool m_getaddr_recvd{false};
    /** Guards m_addr_token_bucket */
    mutable Mutex m_addr_token_bucket_mutex;
    /**
     * Number of addresses that can be processed from this peer. Start at 1
     * to permit self-announcement.
     */
    double m_addr_token_bucket GUARDED_BY(m_addr_token_bucket_mutex){1.0};
    /** When m_addr_token_bucket was last updated */
    std::chrono::microseconds m_addr_token_timestamp{
        GetTime<std::chrono::microseconds>()};
    /** Total number of addresses that were dropped due to rate limiting. */
    std::atomic<uint64_t> m_addr_rate_limited{0};
    /**
     * Total number of addresses that were processed (excludes rate-limited
     * ones).
     */
    std::atomic<uint64_t> m_addr_processed{0};

    /**
     * Set of txids to reconsider once their parent transactions have been
     * accepted
     */
    std::set<TxId> m_orphan_work_set GUARDED_BY(g_cs_orphans);

    /** Protects m_getdata_requests **/
    Mutex m_getdata_requests_mutex;
    /** Work queue of items requested by this peer **/
    std::deque<CInv> m_getdata_requests GUARDED_BY(m_getdata_requests_mutex);

    explicit Peer(NodeId id) : m_id(id) {}
};

using PeerRef = std::shared_ptr<Peer>;

class PeerManagerImpl final : public PeerManager {
public:
    PeerManagerImpl(const CChainParams &chainparams, CConnman &connman,
                    AddrMan &addrman, BanMan *banman,
                    ChainstateManager &chainman, CTxMemPool &pool,
                    bool ignore_incoming_txs);

    /** Overridden from CValidationInterface. */
    void BlockConnected(const std::shared_ptr<const CBlock> &pblock,
                        const CBlockIndex *pindexConnected) override;
    void BlockDisconnected(const std::shared_ptr<const CBlock> &block,
                           const CBlockIndex *pindex) override;
    void UpdatedBlockTip(const CBlockIndex *pindexNew,
                         const CBlockIndex *pindexFork,
                         bool fInitialDownload) override;
    void BlockChecked(const CBlock &block,
                      const BlockValidationState &state) override;
    void NewPoWValidBlock(const CBlockIndex *pindex,
                          const std::shared_ptr<const CBlock> &pblock) override;

    /** Implement NetEventsInterface */
    void InitializeNode(const Config &config, CNode *pnode) override;
    void FinalizeNode(const Config &config, const CNode &node) override;
    bool ProcessMessages(const Config &config, CNode *pfrom,
                         std::atomic<bool> &interrupt) override;
    bool SendMessages(const Config &config, CNode *pto) override
        EXCLUSIVE_LOCKS_REQUIRED(pto->cs_sendProcessing);

    /** Implement PeerManager */
    void StartScheduledTasks(CScheduler &scheduler) override;
    void CheckForStaleTipAndEvictPeers() override;
    std::optional<std::string>
    FetchBlock(const Config &config, NodeId peer_id,
               const CBlockIndex &block_index) override;
    bool GetNodeStateStats(NodeId nodeid,
                           CNodeStateStats &stats) const override;
    bool IgnoresIncomingTxs() override { return m_ignore_incoming_txs; }
    void SendPings() override;
    void RelayTransaction(const TxId &txid) override;
    void RelayProof(const avalanche::ProofId &proofid) override;
    void SetBestHeight(int height) override { m_best_height = height; };
    void Misbehaving(const NodeId pnode, const int howmuch,
                     const std::string &message) override;
    void ProcessMessage(const Config &config, CNode &pfrom,
                        const std::string &msg_type, CDataStream &vRecv,
                        const std::chrono::microseconds time_received,
                        const std::atomic<bool> &interruptMsgProc) override;

private:
    /**
     * Consider evicting an outbound peer based on the amount of time they've
     * been behind our tip.
     */
    void ConsiderEviction(CNode &pto, int64_t time_in_seconds)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /**
     * If we have extra outbound peers, try to disconnect the one with the
     * oldest block announcement.
     */
    void EvictExtraOutboundPeers(std::chrono::seconds now)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Retrieve unbroadcast transactions from the mempool and reattempt
     * sending to peers
     */
    void ReattemptInitialBroadcast(CScheduler &scheduler);

    /**
     * Update the avalanche statistics for all the nodes
     */
    void UpdateAvalancheStatistics() const;

    /**
     * Process periodic avalanche network messaging and cleanups.
     */
    void AvalanchePeriodicNetworking(CScheduler &scheduler) const;

    /**
     * Get a shared pointer to the Peer object.
     * May return an empty shared_ptr if the Peer object can't be found.
     */
    PeerRef GetPeerRef(NodeId id) const;

    /**
     * Get a shared pointer to the Peer object and remove it from m_peer_map.
     * May return an empty shared_ptr if the Peer object can't be found.
     */
    PeerRef RemovePeer(NodeId id);

    // overloaded variant of above to operate on CNode*s
    void Misbehaving(const CNode &node, int howmuch,
                     const std::string &message) {
        Misbehaving(node.GetId(), howmuch, message);
    }

    /**
     * Potentially mark a node discouraged based on the contents of a
     * BlockValidationState object
     *
     * @param[in] via_compact_block this bool is passed in because
     * net_processing should punish peers differently depending on whether the
     * data was provided in a compact block message or not. If the compact block
     * had a valid header, but contained invalid txs, the peer should not be
     * punished. See BIP 152.
     *
     * @return Returns true if the peer was punished (probably disconnected)
     */
    bool MaybePunishNodeForBlock(NodeId nodeid,
                                 const BlockValidationState &state,
                                 bool via_compact_block,
                                 const std::string &message = "");

    /**
     * Potentially disconnect and discourage a node based on the contents of a
     * TxValidationState object
     *
     * @return Returns true if the peer was punished (probably disconnected)
     */
    bool MaybePunishNodeForTx(NodeId nodeid, const TxValidationState &state,
                              const std::string &message = "");

    /**
     * Maybe disconnect a peer and discourage future connections from its
     * address.
     *
     * @param[in]   pnode     The node to check.
     * @param[in]   peer      The peer object to check.
     * @return                True if the peer was marked for disconnection in
     * this function
     */
    bool MaybeDiscourageAndDisconnect(CNode &pnode, Peer &peer);

    void ProcessOrphanTx(const Config &config, std::set<TxId> &orphan_work_set)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main, g_cs_orphans);
    /** Process a single headers message from a peer. */
    void ProcessHeadersMessage(const Config &config, CNode &pfrom,
                               const Peer &peer,
                               const std::vector<CBlockHeader> &headers,
                               bool via_compact_block);

    void SendBlockTransactions(CNode &pfrom, const CBlock &block,
                               const BlockTransactionsRequest &req);

    /**
     * Register with InvRequestTracker that a TX INV has been received from a
     * peer. The announcement parameters are decided in PeerManager and then
     * passed to InvRequestTracker.
     */
    void AddTxAnnouncement(const CNode &node, const TxId &txid,
                           std::chrono::microseconds current_time)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Register with InvRequestTracker that a PROOF INV has been received from a
     * peer. The announcement parameters are decided in PeerManager and then
     * passed to InvRequestTracker.
     */
    void
    AddProofAnnouncement(const CNode &node, const avalanche::ProofId &proofid,
                         std::chrono::microseconds current_time, bool preferred)
        EXCLUSIVE_LOCKS_REQUIRED(cs_proofrequest);

    /** Send a version message to a peer */
    void PushNodeVersion(const Config &config, CNode &pnode);

    /**
     * Send a ping message every PING_INTERVAL or if requested via RPC. May mark
     * the peer to be disconnected if a ping has timed out.
     * We use mockable time for ping timeouts, so setmocktime may cause pings
     * to time out.
     */
    void MaybeSendPing(CNode &node_to, Peer &peer,
                       std::chrono::microseconds now);

    /** Send `addr` messages on a regular schedule. */
    void MaybeSendAddr(CNode &node, Peer &peer,
                       std::chrono::microseconds current_time);

    /** Send `feefilter` message. */
    void MaybeSendFeefilter(CNode &node,
                            std::chrono::microseconds current_time);

    /**
     * Relay (gossip) an address to a few randomly chosen nodes.
     *
     * @param[in] originator   The id of the peer that sent us the address. We
     *                         don't want to relay it back.
     * @param[in] addr         Address to relay.
     * @param[in] fReachable   Whether the address' network is reachable. We
     *                         relay unreachable addresses less.
     */
    void RelayAddress(NodeId originator, const CAddress &addr, bool fReachable);

    const CChainParams &m_chainparams;
    CConnman &m_connman;
    AddrMan &m_addrman;
    /**
     * Pointer to this node's banman. May be nullptr - check existence before
     * dereferencing.
     */
    BanMan *const m_banman;
    ChainstateManager &m_chainman;
    CTxMemPool &m_mempool;
    InvRequestTracker<TxId> m_txrequest GUARDED_BY(::cs_main);

    Mutex cs_proofrequest;
    InvRequestTracker<avalanche::ProofId>
        m_proofrequest GUARDED_BY(cs_proofrequest);

    /** The height of the best chain */
    std::atomic<int> m_best_height{-1};

    /** Next time to check for stale tip */
    std::chrono::seconds m_stale_tip_check_time{0s};

    /** Whether this node is running in blocks only mode */
    const bool m_ignore_incoming_txs;

    /**
     * Whether we've completed initial sync yet, for determining when to turn
     * on extra block-relay-only peers.
     */
    bool m_initial_sync_finished{false};

    /**
     * Protects m_peer_map. This mutex must not be locked while holding a lock
     * on any of the mutexes inside a Peer object.
     */
    mutable Mutex m_peer_mutex;
    /**
     * Map of all Peer objects, keyed by peer id. This map is protected
     * by the m_peer_mutex. Once a shared pointer reference is
     * taken, the lock may be released. Individual fields are protected by
     * their own locks.
     */
    std::map<NodeId, PeerRef> m_peer_map GUARDED_BY(m_peer_mutex);

    /** Number of nodes with fSyncStarted. */
    int nSyncStarted GUARDED_BY(cs_main) = 0;

    /**
     * Sources of received blocks, saved to be able to punish them when
     * processing happens afterwards.
     * Set mapBlockSource[hash].second to false if the node should not be
     * punished if the block is invalid.
     */
    std::map<BlockHash, std::pair<NodeId, bool>>
        mapBlockSource GUARDED_BY(cs_main);

    /** Number of outbound peers with m_chain_sync.m_protect. */
    int m_outbound_peers_with_protect_from_disconnect GUARDED_BY(cs_main) = 0;

    bool AlreadyHaveTx(const TxId &txid) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Filter for transactions that were recently rejected by the mempool.
     * These are not rerequested until the chain tip changes, at which point
     * the entire filter is reset.
     *
     * Without this filter we'd be re-requesting txs from each of our peers,
     * increasing bandwidth consumption considerably. For instance, with 100
     * peers, half of which relay a tx we don't accept, that might be a 50x
     * bandwidth increase. A flooding attacker attempting to roll-over the
     * filter using minimum-sized, 60byte, transactions might manage to send
     * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
     * two minute window to send invs to us.
     *
     * Decreasing the false positive rate is fairly cheap, so we pick one in a
     * million to make it highly unlikely for users to have issues with this
     * filter.
     *
     * Memory used: 1.3 MB
     */
    std::unique_ptr<CRollingBloomFilter> recentRejects GUARDED_BY(cs_main);
    uint256 hashRecentRejectsChainTip GUARDED_BY(cs_main);

    /**
     * Filter for transactions that have been recently confirmed.
     * We use this to avoid requesting transactions that have already been
     * confirmed.
     */
    mutable Mutex m_recent_confirmed_transactions_mutex;
    std::unique_ptr<CRollingBloomFilter> m_recent_confirmed_transactions
        GUARDED_BY(m_recent_confirmed_transactions_mutex);

    /** Have we requested this block from a peer */
    bool IsBlockRequested(const BlockHash &hash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Remove this block from our tracked requested blocks. Called if:
     *  - the block has been received from a peer
     *  - the request for the block has timed out
     */
    void RemoveBlockRequest(const BlockHash &hash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Mark a block as in flight
     * Returns false, still setting pit, if the block was already in flight from
     * the same peer pit will only be valid as long as the same cs_main lock is
     * being held
     */
    bool BlockRequested(const Config &config, NodeId nodeid,
                        const CBlockIndex &block,
                        std::list<QueuedBlock>::iterator **pit = nullptr)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    bool TipMayBeStale() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Update pindexLastCommonBlock and add not-in-flight missing successors to
     * vBlocks, until it has at most count entries.
     */
    void FindNextBlocksToDownload(NodeId nodeid, unsigned int count,
                                  std::vector<const CBlockIndex *> &vBlocks,
                                  NodeId &nodeStaller)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    std::map<BlockHash, std::pair<NodeId, std::list<QueuedBlock>::iterator>>
        mapBlocksInFlight GUARDED_BY(cs_main);

    /** When our tip was last updated. */
    std::atomic<std::chrono::seconds> m_last_tip_update{0s};

    /**
     * Determine whether or not a peer can request a transaction, and return it
     * (or nullptr if not found or not allowed).
     */
    CTransactionRef FindTxForGetData(const CNode &peer, const TxId &txid,
                                     const std::chrono::seconds mempool_req,
                                     const std::chrono::seconds now)
        LOCKS_EXCLUDED(cs_main);

    void ProcessGetData(const Config &config, CNode &pfrom, Peer &peer,
                        const std::atomic<bool> &interruptMsgProc)
        EXCLUSIVE_LOCKS_REQUIRED(peer.m_getdata_requests_mutex)
            LOCKS_EXCLUDED(cs_main);

    /** Process a new block. Perform any post-processing housekeeping */
    void ProcessBlock(const Config &config, CNode &node,
                      const std::shared_ptr<const CBlock> &block,
                      bool force_processing);

    /** Relay map. */
    typedef std::map<TxId, CTransactionRef> MapRelay;
    MapRelay mapRelay GUARDED_BY(cs_main);

    /**
     * Expiration-time ordered list of (expire time, relay map entry) pairs,
     * protected by cs_main).
     */
    std::deque<std::pair<std::chrono::microseconds, MapRelay::iterator>>
        g_relay_expiration GUARDED_BY(cs_main);

    /**
     * When a peer sends us a valid block, instruct it to announce blocks to us
     * using CMPCTBLOCK if possible by adding its nodeid to the end of
     * lNodesAnnouncingHeaderAndIDs, and keeping that list under a certain size
     * by removing the first element if necessary.
     */
    void MaybeSetPeerAsAnnouncingHeaderAndIDs(NodeId nodeid)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Stack of nodes which we have set to announce using compact blocks */
    std::list<NodeId> lNodesAnnouncingHeaderAndIDs GUARDED_BY(cs_main);

    /** Number of peers from which we're downloading blocks. */
    int m_peers_downloading_from GUARDED_BY(cs_main) = 0;

    /** Storage for orphan information */
    TxOrphanage m_orphanage;

    void AddToCompactExtraTransactions(const CTransactionRef &tx)
        EXCLUSIVE_LOCKS_REQUIRED(g_cs_orphans);

    /**
     * Orphan/conflicted/etc transactions that are kept for compact block
     * reconstruction.
     * The last
     * -blockreconstructionextratxn/DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN of
     * these are kept in a ring buffer
     */
    std::vector<std::pair<TxHash, CTransactionRef>>
        vExtraTxnForCompact GUARDED_BY(g_cs_orphans);
    /** Offset into vExtraTxnForCompact to insert the next tx */
    size_t vExtraTxnForCompactIt GUARDED_BY(g_cs_orphans) = 0;

    /**
     * Check whether the last unknown block a peer advertised is not yet known.
     */
    void ProcessBlockAvailability(NodeId nodeid)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /**
     * Update tracking information about which blocks a peer is assumed to have.
     */
    void UpdateBlockAvailability(NodeId nodeid, const BlockHash &hash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool CanDirectFetch() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * To prevent fingerprinting attacks, only send blocks/headers outside of
     * the active chain if they are no more than a month older (both in time,
     * and in best equivalent proof of work) than the best header chain we know
     * about and we fully-validated them at some point.
     */
    bool BlockRequestAllowed(const CBlockIndex *pindex)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool AlreadyHaveBlock(const BlockHash &block_hash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool AlreadyHaveProof(const avalanche::ProofId &proofid);
    void ProcessGetBlockData(const Config &config, CNode &pfrom, Peer &peer,
                             const CInv &inv);

    /**
     * Validation logic for compact filters request handling.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   filter_type     The filter type the request is for. Must be
     *                              basic filters.
     * @param[in]   start_height    The start height for the request
     * @param[in]   stop_hash       The stop_hash for the request
     * @param[in]   max_height_diff The maximum number of items permitted to
     *                              request, as specified in BIP 157
     * @param[out]  stop_index      The CBlockIndex for the stop_hash block, if
     *                              the request can be serviced.
     * @param[out]  filter_index    The filter index, if the request can be
     *                              serviced.
     * @return                      True if the request can be serviced.
     */
    bool PrepareBlockFilterRequest(CNode &peer, BlockFilterType filter_type,
                                   uint32_t start_height,
                                   const BlockHash &stop_hash,
                                   uint32_t max_height_diff,
                                   const CBlockIndex *&stop_index,
                                   BlockFilterIndex *&filter_index);

    /**
     * Handle a cfilters request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFilters(CNode &peer, CDataStream &vRecv);
    /**
     * Handle a cfheaders request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFHeaders(CNode &peer, CDataStream &vRecv);

    /**
     * Handle a getcfcheckpt request.
     *
     * May disconnect from the peer in the case of a bad request.
     *
     * @param[in]   peer            The peer that we received the request from
     * @param[in]   vRecv           The raw message received
     */
    void ProcessGetCFCheckPt(CNode &peer, CDataStream &vRecv);

    /**
     * Decide a response for an Avalanche poll about the given block.
     *
     * @param[in]   hash            The hash of the block being polled for
     * @return                      Our current vote for the block
     */
    uint32_t GetAvalancheVoteForBlock(const BlockHash &hash) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Decide a response for an Avalanche poll about the given transaction.
     *
     * @param[in] id       The id of the transaction being polled for
     * @return             Our current vote for the transaction
     */
    uint32_t GetAvalancheVoteForTx(const TxId &id) const
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Checks if address relay is permitted with peer. If needed, initializes
     * the m_addr_known bloom filter and sets m_addr_relay_enabled to true.
     *
     *  @return   True if address relay is enabled with peer
     *            False if address relay is disallowed
     */
    bool SetupAddressRelay(const CNode &node, Peer &peer);

    /**
     * Manage reception of an avalanche proof.
     *
     * @return   False if the peer is misbehaving, true otherwise
     */
    bool ReceivedAvalancheProof(CNode &peer, const avalanche::ProofRef &proof);
};
} // namespace

namespace {
/**
 * Filter for proofs that are consensus-invalid or were recently invalidated
 * by avalanche (finalized rejection). These are not rerequested until they are
 * rolled out of the filter.
 *
 * Without this filter we'd be re-requesting proofs from each of our peers,
 * increasing bandwidth consumption considerably.
 *
 * Decreasing the false positive rate is fairly cheap, so we pick one in a
 * million to make it highly unlikely for users to have issues with this filter.
 */
Mutex cs_invalidProofs;
std::unique_ptr<CRollingBloomFilter> invalidProofs GUARDED_BY(cs_invalidProofs);

/** Number of preferable block download peers. */
int nPreferredDownload GUARDED_BY(cs_main) = 0;
} // namespace

namespace {
/**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The best known block we know this peer has announced.
    const CBlockIndex *pindexBestKnownBlock{nullptr};
    //! The hash of the last unknown block this peer has announced.
    BlockHash hashLastUnknownBlock{};
    //! The last full block we both have.
    const CBlockIndex *pindexLastCommonBlock{nullptr};
    //! The best header we have sent our peer.
    const CBlockIndex *pindexBestHeaderSent{nullptr};
    //! Length of current-streak of unconnecting headers announcements
    int nUnconnectingHeaders{0};
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted{false};
    //! When to potentially disconnect peer for stalling headers download
    std::chrono::microseconds m_headers_sync_timeout{0us};
    //! Since when we're stalling block download progress (in microseconds), or
    //! 0.
    std::chrono::microseconds m_stalling_since{0us};
    std::list<QueuedBlock> vBlocksInFlight;
    //! When the first entry in vBlocksInFlight started downloading. Don't care
    //! when vBlocksInFlight is empty.
    std::chrono::microseconds m_downloading_since{0us};
    int nBlocksInFlight{0};
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload{false};
    //! Whether this peer wants invs or headers (when possible) for block
    //! announcements.
    bool fPreferHeaders{false};
    //! Whether this peer wants invs or cmpctblocks (when possible) for block
    //! announcements.
    bool fPreferHeaderAndIDs{false};
    /**
     * Whether this peer will send us cmpctblocks if we request them.
     * This is not used to gate request logic, as we really only care about
     * fSupportsDesiredCmpctVersion, but is used as a flag to "lock in" the
     * version of compact blocks we send.
     */
    bool fProvidesHeaderAndIDs{false};
    /**
     * If we've announced NODE_WITNESS to this peer: whether the peer sends
     * witnesses in cmpctblocks/blocktxns, otherwise: whether this peer sends
     * non-witnesses in cmpctblocks/blocktxns.
     */
    bool fSupportsDesiredCmpctVersion{false};

    /**
     * State used to enforce CHAIN_SYNC_TIMEOUT and EXTRA_PEER_CHECK_INTERVAL
     * logic.
     *
     * Both are only in effect for outbound, non-manual, non-protected
     * connections. Any peer protected (m_protect = true) is not chosen for
     * eviction. A peer is marked as protected if all of these are true:
     *   - its connection type is IsBlockOnlyConn() == false
     *   - it gave us a valid connecting header
     *   - we haven't reached MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT yet
     *   - it has a better chain than we have
     *
     * CHAIN_SYNC_TIMEOUT:  if a peer's best known block has less work than our
     * tip, set a timeout CHAIN_SYNC_TIMEOUT seconds in the future:
     *   - If at timeout their best known block now has more work than our tip
     * when the timeout was set, then either reset the timeout or clear it
     * (after comparing against our current tip's work)
     *   - If at timeout their best known block still has less work than our tip
     * did when the timeout was set, then send a getheaders message, and set a
     * shorter timeout, HEADERS_RESPONSE_TIME seconds in future. If their best
     * known block is still behind when that new timeout is reached, disconnect.
     *
     * EXTRA_PEER_CHECK_INTERVAL: after each interval, if we have too many
     * outbound peers, drop the outbound one that least recently announced us a
     * new block.
     */
    struct ChainSyncTimeoutState {
        //! A timeout used for checking whether our peer has sufficiently
        //! synced.
        int64_t m_timeout{0};
        //! A header with the work we require on our peer's chain.
        const CBlockIndex *m_work_header{nullptr};
        //! After timeout is reached, set to true after sending getheaders.
        bool m_sent_getheaders{false};
        //! Whether this peer is protected from disconnection due to a bad/slow
        //! chain.
        bool m_protect{false};
    };

    ChainSyncTimeoutState m_chain_sync;

    //! Time of last new block announcement
    int64_t m_last_block_announcement{0};

    struct AvalancheState {
        std::chrono::time_point<std::chrono::steady_clock> last_poll;
    };

    AvalancheState m_avalanche_state;

    //! Whether this peer is an inbound connection
    const bool m_is_inbound;

    //! A rolling bloom filter of all announced tx CInvs to this peer.
    CRollingBloomFilter m_recently_announced_invs =
        CRollingBloomFilter{INVENTORY_MAX_RECENT_RELAY, 0.000001};

    //! A rolling bloom filter of all announced Proofs CInvs to this peer.
    CRollingBloomFilter m_recently_announced_proofs =
        CRollingBloomFilter{INVENTORY_MAX_RECENT_RELAY, 0.000001};

    CNodeState(bool is_inbound) : m_is_inbound(is_inbound) {}
};

/** Map maintaining per-node state. */
static std::map<NodeId, CNodeState> mapNodeState GUARDED_BY(cs_main);

static CNodeState *State(NodeId pnode) EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    std::map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end()) {
        return nullptr;
    }

    return &it->second;
}

/**
 * Whether the peer supports the address. For example, a peer that does not
 * implement BIP155 cannot receive Tor v3 addresses because it requires
 * ADDRv2 (BIP155) encoding.
 */
static bool IsAddrCompatible(const Peer &peer, const CAddress &addr) {
    return peer.m_wants_addrv2 || addr.IsAddrV1Compatible();
}

static void AddAddressKnown(Peer &peer, const CAddress &addr) {
    assert(peer.m_addr_known);
    peer.m_addr_known->insert(addr.GetKey());
}

static void PushAddress(Peer &peer, const CAddress &addr,
                        FastRandomContext &insecure_rand) {
    // Known checking here is only to save space from duplicates.
    // Before sending, we'll filter it again for known addresses that were
    // added after addresses were pushed.
    assert(peer.m_addr_known);
    if (addr.IsValid() && !peer.m_addr_known->contains(addr.GetKey()) &&
        IsAddrCompatible(peer, addr)) {
        if (peer.m_addrs_to_send.size() >= GetMaxAddrToSend()) {
            peer.m_addrs_to_send[insecure_rand.randrange(
                peer.m_addrs_to_send.size())] = addr;
        } else {
            peer.m_addrs_to_send.push_back(addr);
        }
    }
}

static bool isPreferredDownloadPeer(const CNode &pfrom) {
    LOCK(cs_main);
    const CNodeState *state = State(pfrom.GetId());
    return state && state->fPreferredDownload;
}

static void UpdatePreferredDownload(const CNode &node, CNodeState *state)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload =
        (!node.IsInboundConn() ||
         node.HasPermission(NetPermissionFlags::NoBan)) &&
        !node.IsAddrFetchConn() && !node.fClient;

    nPreferredDownload += state->fPreferredDownload;
}

bool PeerManagerImpl::IsBlockRequested(const BlockHash &hash) {
    return mapBlocksInFlight.find(hash) != mapBlocksInFlight.end();
}

void PeerManagerImpl::RemoveBlockRequest(const BlockHash &hash) {
    auto it = mapBlocksInFlight.find(hash);

    if (it == mapBlocksInFlight.end()) {
        // Block was not requested
        return;
    }

    auto [node_id, list_it] = it->second;
    CNodeState *state = State(node_id);
    assert(state != nullptr);

    if (state->vBlocksInFlight.begin() == list_it) {
        // First block on the queue was received, update the start download time
        // for the next one
        state->m_downloading_since = std::max(
            state->m_downloading_since, GetTime<std::chrono::microseconds>());
    }
    state->vBlocksInFlight.erase(list_it);

    state->nBlocksInFlight--;
    if (state->nBlocksInFlight == 0) {
        // Last validated block on the queue was received.
        m_peers_downloading_from--;
    }
    state->m_stalling_since = 0us;
    mapBlocksInFlight.erase(it);
}

bool PeerManagerImpl::BlockRequested(const Config &config, NodeId nodeid,
                                     const CBlockIndex &block,
                                     std::list<QueuedBlock>::iterator **pit) {
    const BlockHash &hash{block.GetBlockHash()};

    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    // Short-circuit most stuff in case it is from the same node.
    std::map<BlockHash,
             std::pair<NodeId, std::list<QueuedBlock>::iterator>>::iterator
        itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end() &&
        itInFlight->second.first == nodeid) {
        if (pit) {
            *pit = &itInFlight->second.second;
        }
        return false;
    }

    // Make sure it's not listed somewhere already.
    RemoveBlockRequest(hash);

    std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(
        state->vBlocksInFlight.end(),
        {&block, std::unique_ptr<PartiallyDownloadedBlock>(
                     pit ? new PartiallyDownloadedBlock(config, &m_mempool)
                         : nullptr)});
    state->nBlocksInFlight++;
    if (state->nBlocksInFlight == 1) {
        // We're starting a block download (batch) from this peer.
        state->m_downloading_since = GetTime<std::chrono::microseconds>();
        m_peers_downloading_from++;
    }

    itInFlight = mapBlocksInFlight
                     .insert(std::make_pair(hash, std::make_pair(nodeid, it)))
                     .first;

    if (pit) {
        *pit = &itInFlight->second.second;
    }

    return true;
}

void PeerManagerImpl::MaybeSetPeerAsAnnouncingHeaderAndIDs(NodeId nodeid) {
    AssertLockHeld(cs_main);
    CNodeState *nodestate = State(nodeid);
    if (!nodestate) {
        LogPrint(BCLog::NET, "node state unavailable: peer=%d\n", nodeid);
        return;
    }
    if (!nodestate->fProvidesHeaderAndIDs) {
        return;
    }
    int num_outbound_hb_peers = 0;
    for (std::list<NodeId>::iterator it = lNodesAnnouncingHeaderAndIDs.begin();
         it != lNodesAnnouncingHeaderAndIDs.end(); it++) {
        if (*it == nodeid) {
            lNodesAnnouncingHeaderAndIDs.erase(it);
            lNodesAnnouncingHeaderAndIDs.push_back(nodeid);
            return;
        }
        CNodeState *state = State(*it);
        if (state != nullptr && !state->m_is_inbound) {
            ++num_outbound_hb_peers;
        }
    }
    if (nodestate->m_is_inbound) {
        // If we're adding an inbound HB peer, make sure we're not removing
        // our last outbound HB peer in the process.
        if (lNodesAnnouncingHeaderAndIDs.size() >= 3 &&
            num_outbound_hb_peers == 1) {
            CNodeState *remove_node =
                State(lNodesAnnouncingHeaderAndIDs.front());
            if (remove_node != nullptr && !remove_node->m_is_inbound) {
                // Put the HB outbound peer in the second slot, so that it
                // doesn't get removed.
                std::swap(lNodesAnnouncingHeaderAndIDs.front(),
                          *std::next(lNodesAnnouncingHeaderAndIDs.begin()));
            }
        }
    }
    m_connman.ForNode(nodeid, [this](CNode *pfrom) EXCLUSIVE_LOCKS_REQUIRED(
                                  ::cs_main) {
        AssertLockHeld(::cs_main);
        uint64_t nCMPCTBLOCKVersion = 1;
        if (lNodesAnnouncingHeaderAndIDs.size() >= 3) {
            // As per BIP152, we only get 3 of our peers to announce
            // blocks using compact encodings.
            m_connman.ForNode(
                lNodesAnnouncingHeaderAndIDs.front(),
                [this, nCMPCTBLOCKVersion](CNode *pnodeStop) {
                    m_connman.PushMessage(
                        pnodeStop, CNetMsgMaker(pnodeStop->GetCommonVersion())
                                       .Make(NetMsgType::SENDCMPCT,
                                             /*fAnnounceUsingCMPCTBLOCK=*/false,
                                             nCMPCTBLOCKVersion));
                    // save BIP152 bandwidth state: we select peer to be
                    // low-bandwidth
                    pnodeStop->m_bip152_highbandwidth_to = false;
                    return true;
                });
            lNodesAnnouncingHeaderAndIDs.pop_front();
        }
        m_connman.PushMessage(pfrom,
                              CNetMsgMaker(pfrom->GetCommonVersion())
                                  .Make(NetMsgType::SENDCMPCT,
                                        /*fAnnounceUsingCMPCTBLOCK=*/true,
                                        nCMPCTBLOCKVersion));
        // save BIP152 bandwidth state: we select peer to be high-bandwidth
        pfrom->m_bip152_highbandwidth_to = true;
        lNodesAnnouncingHeaderAndIDs.push_back(pfrom->GetId());
        return true;
    });
}

bool PeerManagerImpl::TipMayBeStale() {
    AssertLockHeld(cs_main);
    const Consensus::Params &consensusParams = m_chainparams.GetConsensus();
    if (count_seconds(m_last_tip_update) == 0) {
        m_last_tip_update = GetTime<std::chrono::seconds>();
    }
    return count_seconds(m_last_tip_update) <
               GetTime() - consensusParams.nPowTargetSpacing * 3 &&
           mapBlocksInFlight.empty();
}

bool PeerManagerImpl::CanDirectFetch() {
    return m_chainman.ActiveChain().Tip()->GetBlockTime() >
           GetAdjustedTime() -
               m_chainparams.GetConsensus().nPowTargetSpacing * 20;
}

static bool PeerHasHeader(CNodeState *state, const CBlockIndex *pindex)
    EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
    if (state->pindexBestKnownBlock &&
        pindex == state->pindexBestKnownBlock->GetAncestor(pindex->nHeight)) {
        return true;
    }
    if (state->pindexBestHeaderSent &&
        pindex == state->pindexBestHeaderSent->GetAncestor(pindex->nHeight)) {
        return true;
    }
    return false;
}

void PeerManagerImpl::ProcessBlockAvailability(NodeId nodeid) {
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    if (!state->hashLastUnknownBlock.IsNull()) {
        const CBlockIndex *pindex =
            m_chainman.m_blockman.LookupBlockIndex(state->hashLastUnknownBlock);
        if (pindex && pindex->nChainWork > 0) {
            if (state->pindexBestKnownBlock == nullptr ||
                pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork) {
                state->pindexBestKnownBlock = pindex;
            }
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

void PeerManagerImpl::UpdateBlockAvailability(NodeId nodeid,
                                              const BlockHash &hash) {
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    ProcessBlockAvailability(nodeid);

    const CBlockIndex *pindex = m_chainman.m_blockman.LookupBlockIndex(hash);
    if (pindex && pindex->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == nullptr ||
            pindex->nChainWork >= state->pindexBestKnownBlock->nChainWork) {
            state->pindexBestKnownBlock = pindex;
        }
    } else {
        // An unknown block was announced; just assume that the latest one is
        // the best one.
        state->hashLastUnknownBlock = hash;
    }
}

void PeerManagerImpl::FindNextBlocksToDownload(
    NodeId nodeid, unsigned int count,
    std::vector<const CBlockIndex *> &vBlocks, NodeId &nodeStaller) {
    if (count == 0) {
        return;
    }

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState *state = State(nodeid);
    assert(state != nullptr);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == nullptr ||
        state->pindexBestKnownBlock->nChainWork <
            m_chainman.ActiveChain().Tip()->nChainWork ||
        state->pindexBestKnownBlock->nChainWork < nMinimumChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == nullptr) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking
        // point. Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock =
            m_chainman
                .ActiveChain()[std::min(state->pindexBestKnownBlock->nHeight,
                                        m_chainman.ActiveChain().Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an
    // ancestor of its current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(
        state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock) {
        return;
    }

    std::vector<const CBlockIndex *> vToFetch;
    const CBlockIndex *pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more
    // than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last linked block we have in
    // common with this peer. The +1 is so we can detect stalling, namely if we
    // would be able to download that next block if the window were 1 larger.
    int nWindowEnd =
        state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight =
        std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed)
        // successors of pindexWalk (towards pindexBestKnownBlock) into
        // vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as
        // expensive as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight,
                                std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(
            pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding
        // the ones that are not yet downloaded and not in flight to vBlocks. In
        // the meantime, update pindexLastCommonBlock as long as all ancestors
        // are already downloaded, or if it's already part of our chain (and
        // therefore don't need it even if pruned).
        for (const CBlockIndex *pindex : vToFetch) {
            if (!pindex->IsValid(BlockValidity::TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (pindex->nStatus.hasData() ||
                m_chainman.ActiveChain().Contains(pindex)) {
                if (pindex->HaveTxsDownloaded()) {
                    state->pindexLastCommonBlock = pindex;
                }
            } else if (!IsBlockRequested(pindex->GetBlockHash())) {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd) {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != nodeid) {
                        // We aren't able to fetch anything, but we would be if
                        // the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count) {
                    return;
                }
            } else if (waitingfor == -1) {
                // This is the first already-in-flight block.
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
            }
        }
    }
}

} // namespace

template <class InvId>
static bool TooManyAnnouncements(const CNode &node,
                                 const InvRequestTracker<InvId> &requestTracker,
                                 const DataRequestParameters &requestParams) {
    return !node.HasPermission(
               requestParams.bypass_request_limits_permissions) &&
           requestTracker.Count(node.GetId()) >=
               requestParams.max_peer_announcements;
}

/**
 * Compute the request time for this announcement, current time plus delays for:
 *   - nonpref_peer_delay for announcements from non-preferred connections
 *   - overloaded_peer_delay for announcements from peers which have at least
 *     max_peer_request_in_flight requests in flight (and don't have
 * NetPermissionFlags::Relay).
 */
template <class InvId>
static std::chrono::microseconds
ComputeRequestTime(const CNode &node,
                   const InvRequestTracker<InvId> &requestTracker,
                   const DataRequestParameters &requestParams,
                   std::chrono::microseconds current_time, bool preferred) {
    auto delay = std::chrono::microseconds{0};

    if (!preferred) {
        delay += requestParams.nonpref_peer_delay;
    }

    if (!node.HasPermission(requestParams.bypass_request_limits_permissions) &&
        requestTracker.CountInFlight(node.GetId()) >=
            requestParams.max_peer_request_in_flight) {
        delay += requestParams.overloaded_peer_delay;
    }

    return current_time + delay;
}

void PeerManagerImpl::PushNodeVersion(const Config &config, CNode &pnode) {
    // Note that pnode.GetLocalServices() is a reflection of the local
    // services we were offering when the CNode object was created for this
    // peer.
    ServiceFlags nLocalNodeServices = pnode.GetLocalServices();
    const int64_t nTime{count_seconds(GetTime<std::chrono::seconds>())};
    uint64_t nonce = pnode.GetLocalNonce();
    const int nNodeStartingHeight{m_best_height};
    NodeId nodeid = pnode.GetId();
    CAddress addr = pnode.addr;
    uint64_t extraEntropy = pnode.GetLocalExtraEntropy();

    CAddress addrYou =
        addr.IsRoutable() && !IsProxy(addr) && addr.IsAddrV1Compatible()
            ? addr
            : CAddress(CService(), addr.nServices);
    CAddress addrMe = CAddress(CService(), nLocalNodeServices);

    const bool tx_relay = !m_ignore_incoming_txs &&
                          pnode.m_tx_relay != nullptr && !pnode.IsFeelerConn();
    m_connman.PushMessage(
        &pnode, CNetMsgMaker(INIT_PROTO_VERSION)
                    .Make(NetMsgType::VERSION, PROTOCOL_VERSION,
                          uint64_t(nLocalNodeServices), nTime, addrYou, addrMe,
                          nonce, userAgent(config), nNodeStartingHeight,
                          tx_relay, extraEntropy));

    if (fLogIPs) {
        LogPrint(BCLog::NET,
                 "send version message: version %d, blocks=%d, us=%s, them=%s, "
                 "txrelay=%d, peer=%d\n",
                 PROTOCOL_VERSION, nNodeStartingHeight, addrMe.ToString(),
                 addrYou.ToString(), tx_relay, nodeid);
    } else {
        LogPrint(BCLog::NET,
                 "send version message: version %d, blocks=%d, us=%s, "
                 "txrelay=%d, peer=%d\n",
                 PROTOCOL_VERSION, nNodeStartingHeight, addrMe.ToString(),
                 tx_relay, nodeid);
    }
}

void PeerManagerImpl::AddTxAnnouncement(
    const CNode &node, const TxId &txid,
    std::chrono::microseconds current_time) {
    // For m_txrequest and state
    AssertLockHeld(::cs_main);

    if (TooManyAnnouncements(node, m_txrequest, TX_REQUEST_PARAMS)) {
        return;
    }

    const bool preferred = isPreferredDownloadPeer(node);
    auto reqtime = ComputeRequestTime(node, m_txrequest, TX_REQUEST_PARAMS,
                                      current_time, preferred);

    m_txrequest.ReceivedInv(node.GetId(), txid, preferred, reqtime);
}

void PeerManagerImpl::AddProofAnnouncement(
    const CNode &node, const avalanche::ProofId &proofid,
    std::chrono::microseconds current_time, bool preferred) {
    // For m_proofrequest
    AssertLockHeld(cs_proofrequest);

    if (TooManyAnnouncements(node, m_proofrequest, PROOF_REQUEST_PARAMS)) {
        return;
    }

    auto reqtime = ComputeRequestTime(
        node, m_proofrequest, PROOF_REQUEST_PARAMS, current_time, preferred);

    m_proofrequest.ReceivedInv(node.GetId(), proofid, preferred, reqtime);
}

// This function is used for testing the stale tip eviction logic, see
// denialofservice_tests.cpp
void UpdateLastBlockAnnounceTime(NodeId node, int64_t time_in_seconds) {
    LOCK(cs_main);
    CNodeState *state = State(node);
    if (state) {
        state->m_last_block_announcement = time_in_seconds;
    }
}

void PeerManagerImpl::InitializeNode(const Config &config, CNode *pnode) {
    NodeId nodeid = pnode->GetId();
    {
        LOCK(cs_main);
        mapNodeState.emplace_hint(
            mapNodeState.end(), std::piecewise_construct,
            std::forward_as_tuple(nodeid),
            std::forward_as_tuple(pnode->IsInboundConn()));
        assert(m_txrequest.Count(nodeid) == 0);
    }
    {
        PeerRef peer = std::make_shared<Peer>(nodeid);
        LOCK(m_peer_mutex);
        m_peer_map.emplace_hint(m_peer_map.end(), nodeid, std::move(peer));
    }
    if (!pnode->IsInboundConn()) {
        PushNodeVersion(config, *pnode);
    }
}

void PeerManagerImpl::ReattemptInitialBroadcast(CScheduler &scheduler) {
    std::set<TxId> unbroadcast_txids = m_mempool.GetUnbroadcastTxs();

    for (const TxId &txid : unbroadcast_txids) {
        // Sanity check: all unbroadcast txns should exist in the mempool
        if (m_mempool.exists(txid)) {
            RelayTransaction(txid);
        } else {
            m_mempool.RemoveUnbroadcastTx(txid, true);
        }
    }

    if (g_avalanche && isAvalancheEnabled(gArgs)) {
        // Get and sanitize the list of proofids to broadcast. The RelayProof
        // call is done in a second loop to avoid locking cs_vNodes while
        // cs_peerManager is locked which would cause a potential deadlock due
        // to reversed lock order.
        auto unbroadcasted_proofids =
            g_avalanche->withPeerManager([&](avalanche::PeerManager &pm) {
                auto unbroadcasted_proofids = pm.getUnbroadcastProofs();

                auto it = unbroadcasted_proofids.begin();
                while (it != unbroadcasted_proofids.end()) {
                    // Sanity check: all unbroadcast proofs should be bound to a
                    // peer in the peermanager
                    if (!pm.isBoundToPeer(*it)) {
                        pm.removeUnbroadcastProof(*it);
                        it = unbroadcasted_proofids.erase(it);
                        continue;
                    }

                    ++it;
                }

                return unbroadcasted_proofids;
            });

        // Remaining proofids are the ones to broadcast
        for (const auto &proofid : unbroadcasted_proofids) {
            RelayProof(proofid);
        }
    }

    // Schedule next run for 10-15 minutes in the future.
    // We add randomness on every cycle to avoid the possibility of P2P
    // fingerprinting.
    const auto reattemptBroadcastInterval = 10min + GetRandMillis(5min);
    scheduler.scheduleFromNow([&] { ReattemptInitialBroadcast(scheduler); },
                              reattemptBroadcastInterval);
}

void PeerManagerImpl::UpdateAvalancheStatistics() const {
    m_connman.ForEachNode([](CNode *pnode) {
        pnode->updateAvailabilityScore(AVALANCHE_STATISTICS_DECAY_FACTOR);
    });

    if (!g_avalanche) {
        // Not enabled or not ready yet
        return;
    }

    // Generate a peer availability score by computing an exponentially
    // weighted moving average of the average of node availability scores.
    // This ensures the peer score is bound to the lifetime of its proof which
    // incentivizes stable network activity.
    g_avalanche->withPeerManager([&](avalanche::PeerManager &pm) {
        pm.updateAvailabilityScores(
            AVALANCHE_STATISTICS_DECAY_FACTOR, [&](NodeId nodeid) -> double {
                double score{0.0};
                m_connman.ForNode(nodeid, [&](CNode *pavanode) {
                    score = pavanode->getAvailabilityScore();
                    return true;
                });
                return score;
            });
    });
}

void PeerManagerImpl::AvalanchePeriodicNetworking(CScheduler &scheduler) const {
    const auto now = GetTime<std::chrono::seconds>();
    std::vector<NodeId> avanode_ids;
    bool fQuorumEstablished;
    bool fShouldRequestMoreNodes;

    if (!g_avalanche) {
        // Not enabled or not ready yet, retry later
        goto scheduleLater;
    }

    g_avalanche->sendDelayedAvahello();

    fQuorumEstablished = g_avalanche->isQuorumEstablished();
    fShouldRequestMoreNodes =
        g_avalanche->withPeerManager([&](avalanche::PeerManager &pm) {
            return pm.shouldRequestMoreNodes();
        });

    m_connman.ForEachNode([&](CNode *pnode) {
        // Build a list of the avalanche peers nodeids
        if (pnode->m_avalanche_enabled &&
            (!fQuorumEstablished || !pnode->IsInboundConn())) {
            avanode_ids.push_back(pnode->GetId());
        }

        // If a proof radix tree timed out, cleanup
        if (pnode->m_proof_relay &&
            now > (pnode->m_proof_relay->lastSharedProofsUpdate.load() +
                   AVALANCHE_AVAPROOFS_TIMEOUT)) {
            pnode->m_proof_relay->sharedProofs = {};
        }
    });

    if (avanode_ids.empty()) {
        // No node is available for messaging, retry later
        goto scheduleLater;
    }

    Shuffle(avanode_ids.begin(), avanode_ids.end(), FastRandomContext());

    // Request avalanche addresses from our peers
    for (NodeId avanodeId : avanode_ids) {
        m_connman.ForNode(avanodeId, [&](CNode *pavanode) {
            LogPrint(BCLog::AVALANCHE,
                     "Requesting more avalanche addresses from peer %d\n",
                     avanodeId);
            m_connman.PushMessage(pavanode,
                                  CNetMsgMaker(pavanode->GetCommonVersion())
                                      .Make(NetMsgType::GETAVAADDR));
            PeerRef peer = GetPeerRef(avanodeId);
            WITH_LOCK(peer->m_addr_token_bucket_mutex,
                      peer->m_addr_token_bucket += GetMaxAddrToSend());
            return true;
        });

        // If we have no reason to believe that we need more nodes, only request
        // addresses from one of our peers.
        if (fQuorumEstablished && !fShouldRequestMoreNodes) {
            break;
        }
    }

    if (m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
        // Don't request proofs while in IBD. We're likely to orphan them
        // because we don't have the UTXOs.
        goto scheduleLater;
    }

    // If we never had an avaproofs message yet, be kind and only request to a
    // subset of our peers as we expect a ton of avaproofs message in the
    // process.
    if (g_avalanche->getAvaproofsNodeCounter() == 0) {
        avanode_ids.resize(std::min<size_t>(avanode_ids.size(), 3));
    }

    for (NodeId nodeid : avanode_ids) {
        // Send a getavaproofs to one of our peers
        m_connman.ForNode(nodeid, [&](CNode *pavanode) {
            LogPrint(BCLog::AVALANCHE,
                     "Requesting compact proofs from peer %d\n",
                     pavanode->GetId());
            if (pavanode->m_proof_relay) {
                m_connman.PushMessage(pavanode,
                                      CNetMsgMaker(pavanode->GetCommonVersion())
                                          .Make(NetMsgType::GETAVAPROOFS));

                pavanode->m_proof_relay->compactproofs_requested = true;
            }
            return true;
        });
    }

scheduleLater:
    // Schedule next run for 2-5 minutes in the future.
    // We add randomness on every cycle to avoid the possibility of P2P
    // fingerprinting.
    const auto avalanchePeriodicNetworkingInterval = 2min + GetRandMillis(3min);
    scheduler.scheduleFromNow([&] { AvalanchePeriodicNetworking(scheduler); },
                              avalanchePeriodicNetworkingInterval);
}

void PeerManagerImpl::FinalizeNode(const Config &config, const CNode &node) {
    NodeId nodeid = node.GetId();
    int misbehavior{0};
    {
        LOCK(cs_main);
        {
            // We remove the PeerRef from g_peer_map here, but we don't always
            // destruct the Peer. Sometimes another thread is still holding a
            // PeerRef, so the refcount is >= 1. Be careful not to do any
            // processing here that assumes Peer won't be changed before it's
            // destructed.
            PeerRef peer = RemovePeer(nodeid);
            assert(peer != nullptr);
            misbehavior = WITH_LOCK(peer->m_misbehavior_mutex,
                                    return peer->m_misbehavior_score);
            LOCK(m_peer_mutex);
            m_peer_map.erase(nodeid);
        }
        CNodeState *state = State(nodeid);
        assert(state != nullptr);

        if (state->fSyncStarted) {
            nSyncStarted--;
        }

        for (const QueuedBlock &entry : state->vBlocksInFlight) {
            mapBlocksInFlight.erase(entry.pindex->GetBlockHash());
        }
        WITH_LOCK(g_cs_orphans, m_orphanage.EraseForPeer(nodeid));
        m_txrequest.DisconnectedPeer(nodeid);
        nPreferredDownload -= state->fPreferredDownload;
        m_peers_downloading_from -= (state->nBlocksInFlight != 0);
        assert(m_peers_downloading_from >= 0);
        m_outbound_peers_with_protect_from_disconnect -=
            state->m_chain_sync.m_protect;
        assert(m_outbound_peers_with_protect_from_disconnect >= 0);

        mapNodeState.erase(nodeid);

        if (mapNodeState.empty()) {
            // Do a consistency check after the last peer is removed.
            assert(mapBlocksInFlight.empty());
            assert(nPreferredDownload == 0);
            assert(m_peers_downloading_from == 0);
            assert(m_outbound_peers_with_protect_from_disconnect == 0);
            assert(m_txrequest.Size() == 0);
        }
    }

    if (node.fSuccessfullyConnected && misbehavior == 0 &&
        !node.IsBlockOnlyConn() && !node.IsInboundConn()) {
        // Only change visible addrman state for full outbound peers. We don't
        // call Connected() for feeler connections since they don't have
        // fSuccessfullyConnected set.
        m_addrman.Connected(node.addr);
    }

    WITH_LOCK(cs_proofrequest, m_proofrequest.DisconnectedPeer(nodeid));

    LogPrint(BCLog::NET, "Cleared nodestate for peer=%d\n", nodeid);
}

PeerRef PeerManagerImpl::GetPeerRef(NodeId id) const {
    LOCK(m_peer_mutex);
    auto it = m_peer_map.find(id);
    return it != m_peer_map.end() ? it->second : nullptr;
}

PeerRef PeerManagerImpl::RemovePeer(NodeId id) {
    PeerRef ret;
    LOCK(m_peer_mutex);
    auto it = m_peer_map.find(id);
    if (it != m_peer_map.end()) {
        ret = std::move(it->second);
        m_peer_map.erase(it);
    }
    return ret;
}

bool PeerManagerImpl::GetNodeStateStats(NodeId nodeid,
                                        CNodeStateStats &stats) const {
    {
        LOCK(cs_main);
        CNodeState *state = State(nodeid);
        if (state == nullptr) {
            return false;
        }
        stats.nSyncHeight = state->pindexBestKnownBlock
                                ? state->pindexBestKnownBlock->nHeight
                                : -1;
        stats.nCommonHeight = state->pindexLastCommonBlock
                                  ? state->pindexLastCommonBlock->nHeight
                                  : -1;
        for (const QueuedBlock &queue : state->vBlocksInFlight) {
            if (queue.pindex) {
                stats.vHeightInFlight.push_back(queue.pindex->nHeight);
            }
        }
    }

    PeerRef peer = GetPeerRef(nodeid);
    if (peer == nullptr) {
        return false;
    }
    stats.m_starting_height = peer->m_starting_height;
    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node
    // was still responsive, since pingtime does not update until the ping is
    // complete, which might take a while. So, if a ping is taking an unusually
    // long time in flight, the caller can immediately detect that this is
    // happening.
    std::chrono::microseconds ping_wait{0};
    if ((0 != peer->m_ping_nonce_sent) &&
        (0 != peer->m_ping_start.load().count())) {
        ping_wait =
            GetTime<std::chrono::microseconds>() - peer->m_ping_start.load();
    }

    stats.m_ping_wait = ping_wait;
    stats.m_addr_processed = peer->m_addr_processed.load();
    stats.m_addr_rate_limited = peer->m_addr_rate_limited.load();
    stats.m_addr_relay_enabled = peer->m_addr_relay_enabled.load();

    return true;
}

void PeerManagerImpl::AddToCompactExtraTransactions(const CTransactionRef &tx) {
    size_t max_extra_txn = gArgs.GetIntArg(
        "-blockreconstructionextratxn", DEFAULT_BLOCK_RECONSTRUCTION_EXTRA_TXN);
    if (max_extra_txn <= 0) {
        return;
    }

    if (!vExtraTxnForCompact.size()) {
        vExtraTxnForCompact.resize(max_extra_txn);
    }

    vExtraTxnForCompact[vExtraTxnForCompactIt] =
        std::make_pair(tx->GetHash(), tx);
    vExtraTxnForCompactIt = (vExtraTxnForCompactIt + 1) % max_extra_txn;
}

void PeerManagerImpl::Misbehaving(const NodeId pnode, const int howmuch,
                                  const std::string &message) {
    assert(howmuch > 0);

    PeerRef peer = GetPeerRef(pnode);
    if (peer == nullptr) {
        return;
    }

    LOCK(peer->m_misbehavior_mutex);
    const int score_before{peer->m_misbehavior_score};
    peer->m_misbehavior_score += howmuch;
    const int score_now{peer->m_misbehavior_score};

    const std::string message_prefixed =
        message.empty() ? "" : (": " + message);
    std::string warning;

    if (score_now >= DISCOURAGEMENT_THRESHOLD &&
        score_before < DISCOURAGEMENT_THRESHOLD) {
        warning = " DISCOURAGE THRESHOLD EXCEEDED";
        peer->m_should_discourage = true;
    }

    LogPrint(BCLog::NET, "Misbehaving: peer=%d (%d -> %d)%s%s\n", pnode,
             score_before, score_now, warning, message_prefixed);
}

bool PeerManagerImpl::MaybePunishNodeForBlock(NodeId nodeid,
                                              const BlockValidationState &state,
                                              bool via_compact_block,
                                              const std::string &message) {
    switch (state.GetResult()) {
        case BlockValidationResult::BLOCK_RESULT_UNSET:
            break;
        // The node is providing invalid data:
        case BlockValidationResult::BLOCK_CONSENSUS:
        case BlockValidationResult::BLOCK_MUTATED:
            if (!via_compact_block) {
                Misbehaving(nodeid, 100, message);
                return true;
            }
            break;
        case BlockValidationResult::BLOCK_CACHED_INVALID: {
            LOCK(cs_main);
            CNodeState *node_state = State(nodeid);
            if (node_state == nullptr) {
                break;
            }

            // Ban outbound (but not inbound) peers if on an invalid chain.
            // Exempt HB compact block peers. Manual connections are always
            // protected from discouragement.
            if (!via_compact_block && !node_state->m_is_inbound) {
                Misbehaving(nodeid, 100, message);
                return true;
            }
            break;
        }
        case BlockValidationResult::BLOCK_INVALID_HEADER:
        case BlockValidationResult::BLOCK_CHECKPOINT:
        case BlockValidationResult::BLOCK_INVALID_PREV:
            Misbehaving(nodeid, 100, message);
            return true;
        case BlockValidationResult::BLOCK_FINALIZATION:
            // TODO: Use the state object to report this is probably not the
            // best idea. This is effectively unreachable, unless there is a bug
            // somewhere.
            Misbehaving(nodeid, 20, message);
            return true;
        // Conflicting (but not necessarily invalid) data or different policy:
        case BlockValidationResult::BLOCK_MISSING_PREV:
            // TODO: Handle this much more gracefully (10 DoS points is super
            // arbitrary)
            Misbehaving(nodeid, 10, message);
            return true;
        case BlockValidationResult::BLOCK_RECENT_CONSENSUS_CHANGE:
        case BlockValidationResult::BLOCK_TIME_FUTURE:
            break;
    }
    if (message != "") {
        LogPrint(BCLog::NET, "peer=%d: %s\n", nodeid, message);
    }
    return false;
}

bool PeerManagerImpl::MaybePunishNodeForTx(NodeId nodeid,
                                           const TxValidationState &state,
                                           const std::string &message) {
    switch (state.GetResult()) {
        case TxValidationResult::TX_RESULT_UNSET:
            break;
        // The node is providing invalid data:
        case TxValidationResult::TX_CONSENSUS:
            Misbehaving(nodeid, 100, message);
            return true;
        // Conflicting (but not necessarily invalid) data or different policy:
        case TxValidationResult::TX_RECENT_CONSENSUS_CHANGE:
        case TxValidationResult::TX_INPUTS_NOT_STANDARD:
        case TxValidationResult::TX_NOT_STANDARD:
        case TxValidationResult::TX_MISSING_INPUTS:
        case TxValidationResult::TX_PREMATURE_SPEND:
        case TxValidationResult::TX_CONFLICT:
        case TxValidationResult::TX_MEMPOOL_POLICY:
        case TxValidationResult::TX_NO_MEMPOOL:
            break;
    }
    if (message != "") {
        LogPrint(BCLog::NET, "peer=%d: %s\n", nodeid, message);
    }
    return false;
}

bool PeerManagerImpl::BlockRequestAllowed(const CBlockIndex *pindex) {
    AssertLockHeld(cs_main);
    if (m_chainman.ActiveChain().Contains(pindex)) {
        return true;
    }
    return pindex->IsValid(BlockValidity::SCRIPTS) &&
           (m_chainman.m_best_header != nullptr) &&
           (m_chainman.m_best_header->GetBlockTime() - pindex->GetBlockTime() <
            STALE_RELAY_AGE_LIMIT) &&
           (GetBlockProofEquivalentTime(
                *m_chainman.m_best_header, *pindex, *m_chainman.m_best_header,
                m_chainparams.GetConsensus()) < STALE_RELAY_AGE_LIMIT);
}

std::optional<std::string>
PeerManagerImpl::FetchBlock(const Config &config, NodeId peer_id,
                            const CBlockIndex &block_index) {
    if (fImporting) {
        return "Importing...";
    }
    if (fReindex) {
        return "Reindexing...";
    }

    LOCK(cs_main);
    // Ensure this peer exists and hasn't been disconnected
    CNodeState *state = State(peer_id);
    if (state == nullptr) {
        return "Peer does not exist";
    }
    // Mark block as in-flight unless it already is (for this peer).
    // If a block was already in-flight for a different peer, its BLOCKTXN
    // response will be dropped.
    if (!BlockRequested(config, peer_id, block_index)) {
        return "Already requested from this peer";
    }

    // Construct message to request the block
    const BlockHash &hash{block_index.GetBlockHash()};
    const std::vector<CInv> invs{CInv(MSG_BLOCK, hash)};

    // Send block request message to the peer
    if (!m_connman.ForNode(peer_id, [this, &invs](CNode *node) {
            const CNetMsgMaker msgMaker(node->GetCommonVersion());
            this->m_connman.PushMessage(
                node, msgMaker.Make(NetMsgType::GETDATA, invs));
            return true;
        })) {
        return "Node not fully connected";
    }

    LogPrint(BCLog::NET, "Requesting block %s from peer=%d\n", hash.ToString(),
             peer_id);
    return std::nullopt;
}

std::unique_ptr<PeerManager>
PeerManager::make(const CChainParams &chainparams, CConnman &connman,
                  AddrMan &addrman, BanMan *banman, ChainstateManager &chainman,
                  CTxMemPool &pool, bool ignore_incoming_txs) {
    return std::make_unique<PeerManagerImpl>(chainparams, connman, addrman,
                                             banman, chainman, pool,
                                             ignore_incoming_txs);
}

PeerManagerImpl::PeerManagerImpl(const CChainParams &chainparams,
                                 CConnman &connman, AddrMan &addrman,
                                 BanMan *banman, ChainstateManager &chainman,
                                 CTxMemPool &pool, bool ignore_incoming_txs)
    : m_chainparams(chainparams), m_connman(connman), m_addrman(addrman),
      m_banman(banman), m_chainman(chainman), m_mempool(pool),
      m_ignore_incoming_txs(ignore_incoming_txs) {
    // Initialize global variables that cannot be constructed at startup.
    recentRejects.reset(new CRollingBloomFilter(120000, 0.000001));

    {
        LOCK(cs_invalidProofs);
        invalidProofs = std::make_unique<CRollingBloomFilter>(100000, 0.000001);
    }

    // Blocks don't typically have more than 4000 transactions, so this should
    // be at least six blocks (~1 hr) worth of transactions that we can store.
    // If the number of transactions appearing in a block goes up, or if we are
    // seeing getdata requests more than an hour after initial announcement, we
    // can increase this number.
    // The false positive rate of 1/1M should come out to less than 1
    // transaction per day that would be inadvertently ignored (which is the
    // same probability that we have in the reject filter).
    m_recent_confirmed_transactions.reset(
        new CRollingBloomFilter(24000, 0.000001));
}

void PeerManagerImpl::StartScheduledTasks(CScheduler &scheduler) {
    // Stale tip checking and peer eviction are on two different timers, but we
    // don't want them to get out of sync due to drift in the scheduler, so we
    // combine them in one function and schedule at the quicker (peer-eviction)
    // timer.
    static_assert(
        EXTRA_PEER_CHECK_INTERVAL < STALE_CHECK_INTERVAL,
        "peer eviction timer should be less than stale tip check timer");
    scheduler.scheduleEvery(
        [this]() {
            this->CheckForStaleTipAndEvictPeers();
            return true;
        },
        std::chrono::seconds{EXTRA_PEER_CHECK_INTERVAL});

    // schedule next run for 10-15 minutes in the future
    const auto reattemptBroadcastInterval = 10min + GetRandMillis(5min);
    scheduler.scheduleFromNow([&] { ReattemptInitialBroadcast(scheduler); },
                              reattemptBroadcastInterval);

    // Update the avalanche statistics on a schedule
    scheduler.scheduleEvery(
        [this]() {
            UpdateAvalancheStatistics();
            return true;
        },
        AVALANCHE_STATISTICS_REFRESH_PERIOD);

    // schedule next run for 2-5 minutes in the future
    const auto avalanchePeriodicNetworkingInterval = 2min + GetRandMillis(3min);
    scheduler.scheduleFromNow([&] { AvalanchePeriodicNetworking(scheduler); },
                              avalanchePeriodicNetworkingInterval);
}

/**
 * Evict orphan txn pool entries based on a newly connected
 * block, remember the recently confirmed transactions, and delete tracked
 * announcements for them. Also save the time of the last tip update.
 */
void PeerManagerImpl::BlockConnected(
    const std::shared_ptr<const CBlock> &pblock, const CBlockIndex *pindex) {
    m_orphanage.EraseForBlock(*pblock);
    m_last_tip_update = GetTime<std::chrono::seconds>();

    {
        LOCK(m_recent_confirmed_transactions_mutex);
        for (const CTransactionRef &ptx : pblock->vtx) {
            m_recent_confirmed_transactions->insert(ptx->GetId());
        }
    }
    {
        LOCK(cs_main);
        for (const auto &ptx : pblock->vtx) {
            m_txrequest.ForgetInvId(ptx->GetId());
        }
    }
}

void PeerManagerImpl::BlockDisconnected(
    const std::shared_ptr<const CBlock> &block, const CBlockIndex *pindex) {
    // To avoid relay problems with transactions that were previously
    // confirmed, clear our filter of recently confirmed transactions whenever
    // there's a reorg.
    // This means that in a 1-block reorg (where 1 block is disconnected and
    // then another block reconnected), our filter will drop to having only one
    // block's worth of transactions in it, but that should be fine, since
    // presumably the most common case of relaying a confirmed transaction
    // should be just after a new block containing it is found.
    LOCK(m_recent_confirmed_transactions_mutex);
    m_recent_confirmed_transactions->reset();
}

// All of the following cache a recent block, and are protected by
// cs_most_recent_block
static RecursiveMutex cs_most_recent_block;
static std::shared_ptr<const CBlock>
    most_recent_block GUARDED_BY(cs_most_recent_block);
static std::shared_ptr<const CBlockHeaderAndShortTxIDs>
    most_recent_compact_block GUARDED_BY(cs_most_recent_block);
static uint256 most_recent_block_hash GUARDED_BY(cs_most_recent_block);

/**
 * Maintain state about the best-seen block and fast-announce a compact block
 * to compatible peers.
 */
void PeerManagerImpl::NewPoWValidBlock(
    const CBlockIndex *pindex, const std::shared_ptr<const CBlock> &pblock) {
    std::shared_ptr<const CBlockHeaderAndShortTxIDs> pcmpctblock =
        std::make_shared<const CBlockHeaderAndShortTxIDs>(*pblock);
    const CNetMsgMaker msgMaker(PROTOCOL_VERSION);

    LOCK(cs_main);

    static int nHighestFastAnnounce = 0;
    if (pindex->nHeight <= nHighestFastAnnounce) {
        return;
    }
    nHighestFastAnnounce = pindex->nHeight;

    uint256 hashBlock(pblock->GetHash());

    {
        LOCK(cs_most_recent_block);
        most_recent_block_hash = hashBlock;
        most_recent_block = pblock;
        most_recent_compact_block = pcmpctblock;
    }

    m_connman.ForEachNode(
        [this, &pcmpctblock, pindex, &msgMaker,
         &hashBlock](CNode *pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            AssertLockHeld(::cs_main);

            // TODO: Avoid the repeated-serialization here
            if (pnode->GetCommonVersion() < INVALID_CB_NO_BAN_VERSION ||
                pnode->fDisconnect) {
                return;
            }
            ProcessBlockAvailability(pnode->GetId());
            CNodeState &state = *State(pnode->GetId());
            // If the peer has, or we announced to them the previous block
            // already, but we don't think they have this one, go ahead and
            // announce it.
            if (state.fPreferHeaderAndIDs && !PeerHasHeader(&state, pindex) &&
                PeerHasHeader(&state, pindex->pprev)) {
                LogPrint(BCLog::NET,
                         "%s sending header-and-ids %s to peer=%d\n",
                         "PeerManager::NewPoWValidBlock", hashBlock.ToString(),
                         pnode->GetId());
                m_connman.PushMessage(
                    pnode, msgMaker.Make(NetMsgType::CMPCTBLOCK, *pcmpctblock));
                state.pindexBestHeaderSent = pindex;
            }
        });
}

/**
 * Update our best height and announce any block hashes which weren't previously
 * in m_chainman.ActiveChain() to our peers.
 */
void PeerManagerImpl::UpdatedBlockTip(const CBlockIndex *pindexNew,
                                      const CBlockIndex *pindexFork,
                                      bool fInitialDownload) {
    SetBestHeight(pindexNew->nHeight);
    SetServiceFlagsIBDCache(!fInitialDownload);

    // Don't relay inventory during initial block download.
    if (fInitialDownload) {
        return;
    }

    // Find the hashes of all blocks that weren't previously in the best chain.
    std::vector<BlockHash> vHashes;
    const CBlockIndex *pindexToAnnounce = pindexNew;
    while (pindexToAnnounce != pindexFork) {
        vHashes.push_back(pindexToAnnounce->GetBlockHash());
        pindexToAnnounce = pindexToAnnounce->pprev;
        if (vHashes.size() == MAX_BLOCKS_TO_ANNOUNCE) {
            // Limit announcements in case of a huge reorganization. Rely on the
            // peer's synchronization mechanism in that case.
            break;
        }
    }

    {
        LOCK(m_peer_mutex);
        for (auto &it : m_peer_map) {
            Peer &peer = *it.second;
            LOCK(peer.m_block_inv_mutex);
            for (const BlockHash &hash : reverse_iterate(vHashes)) {
                peer.m_blocks_for_headers_relay.push_back(hash);
            }
        }
    }

    m_connman.WakeMessageHandler();
}

/**
 * Handle invalid block rejection and consequent peer banning, maintain which
 * peers announce compact blocks.
 */
void PeerManagerImpl::BlockChecked(const CBlock &block,
                                   const BlockValidationState &state) {
    LOCK(cs_main);

    const BlockHash hash = block.GetHash();
    std::map<BlockHash, std::pair<NodeId, bool>>::iterator it =
        mapBlockSource.find(hash);

    // If the block failed validation, we know where it came from and we're
    // still connected to that peer, maybe punish.
    if (state.IsInvalid() && it != mapBlockSource.end() &&
        State(it->second.first)) {
        MaybePunishNodeForBlock(/*nodeid=*/it->second.first, state,
                                /*via_compact_block=*/!it->second.second);
    }
    // Check that:
    // 1. The block is valid
    // 2. We're not in initial block download
    // 3. This is currently the best block we're aware of. We haven't updated
    //    the tip yet so we have no way to check this directly here. Instead we
    //    just check that there are currently no other blocks in flight.
    else if (state.IsValid() &&
             !m_chainman.ActiveChainstate().IsInitialBlockDownload() &&
             mapBlocksInFlight.count(hash) == mapBlocksInFlight.size()) {
        if (it != mapBlockSource.end()) {
            MaybeSetPeerAsAnnouncingHeaderAndIDs(it->second.first);
        }
    }

    if (it != mapBlockSource.end()) {
        mapBlockSource.erase(it);
    }
}

//////////////////////////////////////////////////////////////////////////////
//
// Messages
//

bool PeerManagerImpl::AlreadyHaveTx(const TxId &txid) {
    assert(recentRejects);
    if (m_chainman.ActiveChain().Tip()->GetBlockHash() !=
        hashRecentRejectsChainTip) {
        // If the chain tip has changed previously rejected transactions
        // might be now valid, e.g. due to a nLockTime'd tx becoming
        // valid, or a double-spend. Reset the rejects filter and give
        // those txs a second chance.
        hashRecentRejectsChainTip =
            m_chainman.ActiveChain().Tip()->GetBlockHash();
        recentRejects->reset();
    }

    if (m_orphanage.HaveTx(txid)) {
        return true;
    }

    {
        LOCK(m_recent_confirmed_transactions_mutex);
        if (m_recent_confirmed_transactions->contains(txid)) {
            return true;
        }
    }

    return recentRejects->contains(txid) || m_mempool.exists(txid);
}

bool PeerManagerImpl::AlreadyHaveBlock(const BlockHash &block_hash) {
    return m_chainman.m_blockman.LookupBlockIndex(block_hash) != nullptr;
}

bool PeerManagerImpl::AlreadyHaveProof(const avalanche::ProofId &proofid) {
    assert(g_avalanche);

    auto localProof = g_avalanche->getLocalProof();
    if (localProof && localProof->getId() == proofid) {
        return true;
    }

    const bool hasProof = g_avalanche->withPeerManager(
        [&proofid](avalanche::PeerManager &pm) { return pm.exists(proofid); });

    LOCK(cs_invalidProofs);
    return hasProof || invalidProofs->contains(proofid);
}

void PeerManagerImpl::SendPings() {
    LOCK(m_peer_mutex);
    for (auto &it : m_peer_map) {
        it.second->m_ping_queued = true;
    }
}

void PeerManagerImpl::RelayTransaction(const TxId &txid) {
    m_connman.ForEachNode(
        [&txid](CNode *pnode) { pnode->PushTxInventory(txid); });
}

void PeerManagerImpl::RelayProof(const avalanche::ProofId &proofid) {
    m_connman.ForEachNode(
        [&proofid](CNode *pnode) { pnode->PushProofInventory(proofid); });
}

void PeerManagerImpl::RelayAddress(NodeId originator, const CAddress &addr,
                                   bool fReachable) {
    // We choose the same nodes within a given 24h window (if the list of
    // connected nodes does not change) and we don't relay to nodes that already
    // know an address. So within 24h we will likely relay a given address once.
    // This is to prevent a peer from unjustly giving their address better
    // propagation by sending it to us repeatedly.

    if (!fReachable && !addr.IsRelayable()) {
        return;
    }

    // Relay to a limited number of other nodes
    // Use deterministic randomness to send to the same nodes for 24 hours
    // at a time so the m_addr_knowns of the chosen nodes prevent repeats
    const uint64_t hash_addr{CServiceHash(0, 0)(addr)};
    const CSipHasher hasher{
        m_connman.GetDeterministicRandomizer(RANDOMIZER_ID_ADDRESS_RELAY)
            .Write(hash_addr)
            .Write((GetTime() + hash_addr) / (24 * 60 * 60))};
    FastRandomContext insecure_rand;

    // Relay reachable addresses to 2 peers. Unreachable addresses are relayed
    // randomly to 1 or 2 peers.
    unsigned int nRelayNodes = (fReachable || (hasher.Finalize() & 1)) ? 2 : 1;
    std::array<std::pair<uint64_t, Peer *>, 2> best{
        {{0, nullptr}, {0, nullptr}}};
    assert(nRelayNodes <= best.size());

    LOCK(m_peer_mutex);

    for (auto &[id, peer] : m_peer_map) {
        if (peer->m_addr_relay_enabled && id != originator &&
            IsAddrCompatible(*peer, addr)) {
            uint64_t hashKey = CSipHasher(hasher).Write(id).Finalize();
            for (unsigned int i = 0; i < nRelayNodes; i++) {
                if (hashKey > best[i].first) {
                    std::copy(best.begin() + i, best.begin() + nRelayNodes - 1,
                              best.begin() + i + 1);
                    best[i] = std::make_pair(hashKey, peer.get());
                    break;
                }
            }
        }
    };

    for (unsigned int i = 0; i < nRelayNodes && best[i].first != 0; i++) {
        PushAddress(*best[i].second, addr, insecure_rand);
    }
}

void PeerManagerImpl::ProcessGetBlockData(const Config &config, CNode &pfrom,
                                          Peer &peer, const CInv &inv) {
    const BlockHash hash(inv.hash);

    std::shared_ptr<const CBlock> a_recent_block;
    std::shared_ptr<const CBlockHeaderAndShortTxIDs> a_recent_compact_block;
    {
        LOCK(cs_most_recent_block);
        a_recent_block = most_recent_block;
        a_recent_compact_block = most_recent_compact_block;
    }

    bool need_activate_chain = false;
    {
        LOCK(cs_main);
        const CBlockIndex *pindex =
            m_chainman.m_blockman.LookupBlockIndex(hash);
        if (pindex) {
            if (pindex->HaveTxsDownloaded() &&
                !pindex->IsValid(BlockValidity::SCRIPTS) &&
                pindex->IsValid(BlockValidity::TREE)) {
                // If we have the block and all of its parents, but have not yet
                // validated it, we might be in the middle of connecting it (ie
                // in the unlock of cs_main before ActivateBestChain but after
                // AcceptBlock). In this case, we need to run ActivateBestChain
                // prior to checking the relay conditions below.
                need_activate_chain = true;
            }
        }
    } // release cs_main before calling ActivateBestChain
    if (need_activate_chain) {
        BlockValidationState state;
        if (!m_chainman.ActiveChainstate().ActivateBestChain(config, state,
                                                             a_recent_block)) {
            LogPrint(BCLog::NET, "failed to activate chain (%s)\n",
                     state.ToString());
        }
    }

    LOCK(cs_main);
    const CBlockIndex *pindex = m_chainman.m_blockman.LookupBlockIndex(hash);
    if (!pindex) {
        return;
    }
    if (!BlockRequestAllowed(pindex)) {
        LogPrint(BCLog::NET,
                 "%s: ignoring request from peer=%i for old "
                 "block that isn't in the main chain\n",
                 __func__, pfrom.GetId());
        return;
    }
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());
    // Disconnect node in case we have reached the outbound limit for serving
    // historical blocks.
    if (m_connman.OutboundTargetReached(true) &&
        (((m_chainman.m_best_header != nullptr) &&
          (m_chainman.m_best_header->GetBlockTime() - pindex->GetBlockTime() >
           HISTORICAL_BLOCK_AGE)) ||
         inv.IsMsgFilteredBlk()) &&
        // nodes with the download permission may exceed target
        !pfrom.HasPermission(NetPermissionFlags::Download)) {
        LogPrint(BCLog::NET,
                 "historical block serving limit reached, disconnect peer=%d\n",
                 pfrom.GetId());
        pfrom.fDisconnect = true;
        return;
    }
    // Avoid leaking prune-height by never sending blocks below the
    // NODE_NETWORK_LIMITED threshold.
    // Add two blocks buffer extension for possible races
    if (!pfrom.HasPermission(NetPermissionFlags::NoBan) &&
        ((((pfrom.GetLocalServices() & NODE_NETWORK_LIMITED) ==
           NODE_NETWORK_LIMITED) &&
          ((pfrom.GetLocalServices() & NODE_NETWORK) != NODE_NETWORK) &&
          (m_chainman.ActiveChain().Tip()->nHeight - pindex->nHeight >
           (int)NODE_NETWORK_LIMITED_MIN_BLOCKS + 2)))) {
        LogPrint(BCLog::NET,
                 "Ignore block request below NODE_NETWORK_LIMITED "
                 "threshold, disconnect peer=%d\n",
                 pfrom.GetId());

        // disconnect node and prevent it from stalling (would otherwise wait
        // for the missing block)
        pfrom.fDisconnect = true;
        return;
    }
    // Pruned nodes may have deleted the block, so check whether it's available
    // before trying to send.
    if (!pindex->nStatus.hasData()) {
        return;
    }
    std::shared_ptr<const CBlock> pblock;
    if (a_recent_block && a_recent_block->GetHash() == pindex->GetBlockHash()) {
        pblock = a_recent_block;
    } else {
        // Send block from disk
        std::shared_ptr<CBlock> pblockRead = std::make_shared<CBlock>();
        if (!ReadBlockFromDisk(*pblockRead, pindex,
                               m_chainparams.GetConsensus())) {
            assert(!"cannot load block from disk");
        }
        pblock = pblockRead;
    }
    if (inv.IsMsgBlk()) {
        m_connman.PushMessage(&pfrom,
                              msgMaker.Make(NetMsgType::BLOCK, *pblock));
    } else if (inv.IsMsgFilteredBlk()) {
        bool sendMerkleBlock = false;
        CMerkleBlock merkleBlock;
        if (pfrom.m_tx_relay != nullptr) {
            LOCK(pfrom.m_tx_relay->cs_filter);
            if (pfrom.m_tx_relay->pfilter) {
                sendMerkleBlock = true;
                merkleBlock = CMerkleBlock(*pblock, *pfrom.m_tx_relay->pfilter);
            }
        }
        if (sendMerkleBlock) {
            m_connman.PushMessage(
                &pfrom, msgMaker.Make(NetMsgType::MERKLEBLOCK, merkleBlock));
            // CMerkleBlock just contains hashes, so also push any
            // transactions in the block the client did not see. This avoids
            // hurting performance by pointlessly requiring a round-trip.
            // Note that there is currently no way for a node to request any
            // single transactions we didn't send here - they must either
            // disconnect and retry or request the full block. Thus, the
            // protocol spec specified allows for us to provide duplicate
            // txn here, however we MUST always provide at least what the
            // remote peer needs.
            typedef std::pair<size_t, uint256> PairType;
            for (PairType &pair : merkleBlock.vMatchedTxn) {
                m_connman.PushMessage(
                    &pfrom,
                    msgMaker.Make(NetMsgType::TX, *pblock->vtx[pair.first]));
            }
        }
        // else
        // no response
    } else if (inv.IsMsgCmpctBlk()) {
        // If a peer is asking for old blocks, we're almost guaranteed they
        // won't have a useful mempool to match against a compact block, and
        // we don't feel like constructing the object for them, so instead
        // we respond with the full, non-compact block.
        int nSendFlags = 0;
        if (CanDirectFetch() &&
            pindex->nHeight >=
                m_chainman.ActiveChain().Height() - MAX_CMPCTBLOCK_DEPTH) {
            CBlockHeaderAndShortTxIDs cmpctblock(*pblock);
            m_connman.PushMessage(
                &pfrom,
                msgMaker.Make(nSendFlags, NetMsgType::CMPCTBLOCK, cmpctblock));
        } else {
            m_connman.PushMessage(
                &pfrom, msgMaker.Make(nSendFlags, NetMsgType::BLOCK, *pblock));
        }
    }

    {
        LOCK(peer.m_block_inv_mutex);
        // Trigger the peer node to send a getblocks request for the next
        // batch of inventory.
        if (hash == peer.m_continuation_block) {
            // Send immediately. This must send even if redundant, and
            // we want it right after the last block so they don't wait for
            // other stuff first.
            std::vector<CInv> vInv;
            vInv.push_back(CInv(
                MSG_BLOCK, m_chainman.ActiveChain().Tip()->GetBlockHash()));
            m_connman.PushMessage(&pfrom, msgMaker.Make(NetMsgType::INV, vInv));
            peer.m_continuation_block = BlockHash();
        }
    }
}

CTransactionRef
PeerManagerImpl::FindTxForGetData(const CNode &peer, const TxId &txid,
                                  const std::chrono::seconds mempool_req,
                                  const std::chrono::seconds now) {
    auto txinfo = m_mempool.info(txid);
    if (txinfo.tx) {
        // If a TX could have been INVed in reply to a MEMPOOL request,
        // or is older than UNCONDITIONAL_RELAY_DELAY, permit the request
        // unconditionally.
        if ((mempool_req.count() && txinfo.m_time <= mempool_req) ||
            txinfo.m_time <= now - UNCONDITIONAL_RELAY_DELAY) {
            return std::move(txinfo.tx);
        }
    }

    {
        LOCK(cs_main);

        // Otherwise, the transaction must have been announced recently.
        if (State(peer.GetId())->m_recently_announced_invs.contains(txid)) {
            // If it was, it can be relayed from either the mempool...
            if (txinfo.tx) {
                return std::move(txinfo.tx);
            }
            // ... or the relay pool.
            auto mi = mapRelay.find(txid);
            if (mi != mapRelay.end()) {
                return mi->second;
            }
        }
    }

    return {};
}

//! Determine whether or not a peer can request a proof, and return it (or
//! nullptr if not found or not allowed).
static avalanche::ProofRef
FindProofForGetData(const CNode &peer, const avalanche::ProofId &proofid,
                    const std::chrono::seconds now) {
    avalanche::ProofRef proof;

    bool send_unconditionally =
        g_avalanche->withPeerManager([&](const avalanche::PeerManager &pm) {
            return pm.forPeer(proofid, [&](const avalanche::Peer &peer) {
                proof = peer.proof;

                // If we know that proof for long enough, allow for requesting
                // it.
                return peer.registration_time <=
                       now - UNCONDITIONAL_RELAY_DELAY;
            });
        });

    if (!proof) {
        // Always send our local proof if it gets requested, assuming it's
        // valid. This will make it easier to bind with peers upon startup where
        // the status of our proof is unknown pending for a block. Note that it
        // still needs to have been announced first (presumably via an avahello
        // message).
        proof = g_avalanche->getLocalProof();
    }

    // We don't have this proof
    if (!proof) {
        return avalanche::ProofRef();
    }

    if (send_unconditionally) {
        return proof;
    }

    // Otherwise, the proofs must have been announced recently.
    LOCK(cs_main);
    if (State(peer.GetId())->m_recently_announced_proofs.contains(proofid)) {
        return proof;
    }

    return avalanche::ProofRef();
}

void PeerManagerImpl::ProcessGetData(
    const Config &config, CNode &pfrom, Peer &peer,
    const std::atomic<bool> &interruptMsgProc) {
    AssertLockNotHeld(cs_main);

    std::deque<CInv>::iterator it = peer.m_getdata_requests.begin();
    std::vector<CInv> vNotFound;
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());

    const std::chrono::seconds now = GetTime<std::chrono::seconds>();
    // Get last mempool request time
    const std::chrono::seconds mempool_req =
        pfrom.m_tx_relay != nullptr
            ? pfrom.m_tx_relay->m_last_mempool_req.load()
            : std::chrono::seconds::min();

    // Process as many TX or AVA_PROOF items from the front of the getdata
    // queue as possible, since they're common and it's efficient to batch
    // process them.
    while (it != peer.m_getdata_requests.end()) {
        if (interruptMsgProc) {
            return;
        }
        // The send buffer provides backpressure. If there's no space in
        // the buffer, pause processing until the next call.
        if (pfrom.fPauseSend) {
            break;
        }

        const CInv &inv = *it;

        if (it->IsMsgProof()) {
            const avalanche::ProofId proofid(inv.hash);
            auto proof = FindProofForGetData(pfrom, proofid, now);
            if (proof) {
                m_connman.PushMessage(
                    &pfrom, msgMaker.Make(NetMsgType::AVAPROOF, *proof));
                g_avalanche->withPeerManager([&](avalanche::PeerManager &pm) {
                    pm.removeUnbroadcastProof(proofid);
                });
            } else {
                vNotFound.push_back(inv);
            }

            ++it;
            continue;
        }

        if (it->IsMsgTx()) {
            if (pfrom.m_tx_relay == nullptr) {
                // Ignore GETDATA requests for transactions from blocks-only
                // peers.
                continue;
            }

            const TxId txid(inv.hash);
            CTransactionRef tx =
                FindTxForGetData(pfrom, txid, mempool_req, now);
            if (tx) {
                int nSendFlags = 0;
                m_connman.PushMessage(
                    &pfrom, msgMaker.Make(nSendFlags, NetMsgType::TX, *tx));
                m_mempool.RemoveUnbroadcastTx(txid);
                // As we're going to send tx, make sure its unconfirmed parents
                // are made requestable.
                std::vector<TxId> parent_ids_to_add;
                {
                    LOCK(m_mempool.cs);
                    auto txiter = m_mempool.GetIter(tx->GetId());
                    if (txiter) {
                        const CTxMemPoolEntry::Parents &parents =
                            (*txiter)->GetMemPoolParentsConst();
                        parent_ids_to_add.reserve(parents.size());
                        for (const CTxMemPoolEntry &parent : parents) {
                            if (parent.GetTime() >
                                now - UNCONDITIONAL_RELAY_DELAY) {
                                parent_ids_to_add.push_back(
                                    parent.GetTx().GetId());
                            }
                        }
                    }
                }
                for (const TxId &parent_txid : parent_ids_to_add) {
                    // Relaying a transaction with a recent but unconfirmed
                    // parent.
                    if (WITH_LOCK(pfrom.m_tx_relay->cs_tx_inventory,
                                  return !pfrom.m_tx_relay->filterInventoryKnown
                                              .contains(parent_txid))) {
                        LOCK(cs_main);
                        State(pfrom.GetId())
                            ->m_recently_announced_invs.insert(parent_txid);
                    }
                }
            } else {
                vNotFound.push_back(inv);
            }

            ++it;
            continue;
        }

        // It's neither a proof nor a transaction
        break;
    }

    // Only process one BLOCK item per call, since they're uncommon and can be
    // expensive to process.
    if (it != peer.m_getdata_requests.end() && !pfrom.fPauseSend) {
        const CInv &inv = *it++;
        if (inv.IsGenBlkMsg()) {
            ProcessGetBlockData(config, pfrom, peer, inv);
        }
        // else: If the first item on the queue is an unknown type, we erase it
        // and continue processing the queue on the next call.
    }

    peer.m_getdata_requests.erase(peer.m_getdata_requests.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it
        // doesn't have to wait around forever. SPV clients care about this
        // message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want
        // to do that because they want to know about (and store and rebroadcast
        // and risk analyze) the dependencies of transactions relevant to them,
        // without having to download the entire memory pool. Also, other nodes
        // can use these messages to automatically request a transaction from
        // some other peer that annnounced it, and stop waiting for us to
        // respond. In normal operation, we often send NOTFOUND messages for
        // parents of transactions that we relay; if a peer is missing a parent,
        // they may assume we have them and request the parents from us.
        m_connman.PushMessage(&pfrom,
                              msgMaker.Make(NetMsgType::NOTFOUND, vNotFound));
    }
}

void PeerManagerImpl::SendBlockTransactions(
    CNode &pfrom, const CBlock &block, const BlockTransactionsRequest &req) {
    BlockTransactions resp(req);
    for (size_t i = 0; i < req.indices.size(); i++) {
        if (req.indices[i] >= block.vtx.size()) {
            Misbehaving(pfrom, 100,
                        "getblocktxn with out-of-bounds tx indices");
            return;
        }
        resp.txn[i] = block.vtx[req.indices[i]];
    }
    LOCK(cs_main);
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());
    int nSendFlags = 0;
    m_connman.PushMessage(
        &pfrom, msgMaker.Make(nSendFlags, NetMsgType::BLOCKTXN, resp));
}

void PeerManagerImpl::ProcessHeadersMessage(
    const Config &config, CNode &pfrom, const Peer &peer,
    const std::vector<CBlockHeader> &headers, bool via_compact_block) {
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());
    size_t nCount = headers.size();

    if (nCount == 0) {
        // Nothing interesting. Stop asking this peers for more headers.
        return;
    }

    bool received_new_header = false;
    const CBlockIndex *pindexLast = nullptr;
    {
        LOCK(cs_main);
        CNodeState *nodestate = State(pfrom.GetId());

        // If this looks like it could be a block announcement (nCount <
        // MAX_BLOCKS_TO_ANNOUNCE), use special logic for handling headers that
        // don't connect:
        // - Send a getheaders message in response to try to connect the chain.
        // - The peer can send up to MAX_UNCONNECTING_HEADERS in a row that
        // don't connect before giving DoS points
        // - Once a headers message is received that is valid and does connect,
        // nUnconnectingHeaders gets reset back to 0.
        if (!m_chainman.m_blockman.LookupBlockIndex(headers[0].hashPrevBlock) &&
            nCount < MAX_BLOCKS_TO_ANNOUNCE) {
            nodestate->nUnconnectingHeaders++;
            m_connman.PushMessage(
                &pfrom, msgMaker.Make(NetMsgType::GETHEADERS,
                                      m_chainman.ActiveChain().GetLocator(
                                          m_chainman.m_best_header),
                                      uint256()));
            LogPrint(
                BCLog::NET,
                "received header %s: missing prev block %s, sending getheaders "
                "(%d) to end (peer=%d, nUnconnectingHeaders=%d)\n",
                headers[0].GetHash().ToString(),
                headers[0].hashPrevBlock.ToString(),
                m_chainman.m_best_header->nHeight, pfrom.GetId(),
                nodestate->nUnconnectingHeaders);
            // Set hashLastUnknownBlock for this peer, so that if we eventually
            // get the headers - even from a different peer - we can use this
            // peer to download.
            UpdateBlockAvailability(pfrom.GetId(), headers.back().GetHash());

            if (nodestate->nUnconnectingHeaders % MAX_UNCONNECTING_HEADERS ==
                0) {
                // The peer is sending us many headers we can't connect.
                Misbehaving(pfrom, 20,
                            strprintf("%d non-connecting headers",
                                      nodestate->nUnconnectingHeaders));
            }
            return;
        }

        BlockHash hashLastBlock;
        for (const CBlockHeader &header : headers) {
            if (!hashLastBlock.IsNull() &&
                header.hashPrevBlock != hashLastBlock) {
                Misbehaving(pfrom, 20, "non-continuous headers sequence");
                return;
            }
            hashLastBlock = header.GetHash();
        }

        // If we don't have the last header, then they'll have given us
        // something new (if these headers are valid).
        if (!m_chainman.m_blockman.LookupBlockIndex(hashLastBlock)) {
            received_new_header = true;
        }
    }

    BlockValidationState state;
    if (!m_chainman.ProcessNewBlockHeaders(config, headers, state,
                                           &pindexLast)) {
        if (state.IsInvalid()) {
            MaybePunishNodeForBlock(pfrom.GetId(), state, via_compact_block,
                                    "invalid header received");
            return;
        }
    }

    {
        LOCK(cs_main);
        CNodeState *nodestate = State(pfrom.GetId());
        if (nodestate->nUnconnectingHeaders > 0) {
            LogPrint(BCLog::NET,
                     "peer=%d: resetting nUnconnectingHeaders (%d -> 0)\n",
                     pfrom.GetId(), nodestate->nUnconnectingHeaders);
        }
        nodestate->nUnconnectingHeaders = 0;

        assert(pindexLast);
        UpdateBlockAvailability(pfrom.GetId(), pindexLast->GetBlockHash());

        // From here, pindexBestKnownBlock should be guaranteed to be non-null,
        // because it is set in UpdateBlockAvailability. Some nullptr checks are
        // still present, however, as belt-and-suspenders.

        if (received_new_header &&
            pindexLast->nChainWork >
                m_chainman.ActiveChain().Tip()->nChainWork) {
            nodestate->m_last_block_announcement = GetTime();
        }

        if (nCount == MAX_HEADERS_RESULTS) {
            // Headers message had its maximum size; the peer may have more
            // headers.
            // TODO: optimize: if pindexLast is an ancestor of
            // m_chainman.ActiveChain().Tip or m_chainman.m_best_header,
            // continue from there instead.
            LogPrint(
                BCLog::NET,
                "more getheaders (%d) to end to peer=%d (startheight:%d)\n",
                pindexLast->nHeight, pfrom.GetId(), peer.m_starting_height);
            m_connman.PushMessage(
                &pfrom,
                msgMaker.Make(NetMsgType::GETHEADERS,
                              m_chainman.ActiveChain().GetLocator(pindexLast),
                              uint256()));
        }

        // If this set of headers is valid and ends in a block with at least as
        // much work as our tip, download as much as possible.
        if (CanDirectFetch() && pindexLast->IsValid(BlockValidity::TREE) &&
            m_chainman.ActiveChain().Tip()->nChainWork <=
                pindexLast->nChainWork) {
            std::vector<const CBlockIndex *> vToFetch;
            const CBlockIndex *pindexWalk = pindexLast;
            // Calculate all the blocks we'd need to switch to pindexLast, up to
            // a limit.
            while (pindexWalk &&
                   !m_chainman.ActiveChain().Contains(pindexWalk) &&
                   vToFetch.size() <= MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                if (!pindexWalk->nStatus.hasData() &&
                    !IsBlockRequested(pindexWalk->GetBlockHash())) {
                    // We don't have this block, and it's not yet in flight.
                    vToFetch.push_back(pindexWalk);
                }
                pindexWalk = pindexWalk->pprev;
            }
            // If pindexWalk still isn't on our main chain, we're looking at a
            // very large reorg at a time we think we're close to caught up to
            // the main chain -- this shouldn't really happen. Bail out on the
            // direct fetch and rely on parallel download instead.
            if (!m_chainman.ActiveChain().Contains(pindexWalk)) {
                LogPrint(
                    BCLog::NET, "Large reorg, won't direct fetch to %s (%d)\n",
                    pindexLast->GetBlockHash().ToString(), pindexLast->nHeight);
            } else {
                std::vector<CInv> vGetData;
                // Download as much as possible, from earliest to latest.
                for (const CBlockIndex *pindex : reverse_iterate(vToFetch)) {
                    if (nodestate->nBlocksInFlight >=
                        MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
                        // Can't download any more from this peer
                        break;
                    }
                    vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                    BlockRequested(config, pfrom.GetId(), *pindex);
                    LogPrint(BCLog::NET, "Requesting block %s from  peer=%d\n",
                             pindex->GetBlockHash().ToString(), pfrom.GetId());
                }
                if (vGetData.size() > 1) {
                    LogPrint(BCLog::NET,
                             "Downloading blocks toward %s (%d) via headers "
                             "direct fetch\n",
                             pindexLast->GetBlockHash().ToString(),
                             pindexLast->nHeight);
                }
                if (vGetData.size() > 0) {
                    if (nodestate->fSupportsDesiredCmpctVersion &&
                        vGetData.size() == 1 && mapBlocksInFlight.size() == 1 &&
                        pindexLast->pprev->IsValid(BlockValidity::CHAIN)) {
                        // In any case, we want to download using a compact
                        // block, not a regular one.
                        vGetData[0] = CInv(MSG_CMPCT_BLOCK, vGetData[0].hash);
                    }
                    m_connman.PushMessage(
                        &pfrom, msgMaker.Make(NetMsgType::GETDATA, vGetData));
                }
            }
        }
        // If we're in IBD, we want outbound peers that will serve us a useful
        // chain. Disconnect peers that are on chains with insufficient work.
        if (m_chainman.ActiveChainstate().IsInitialBlockDownload() &&
            nCount != MAX_HEADERS_RESULTS) {
            // When nCount < MAX_HEADERS_RESULTS, we know we have no more
            // headers to fetch from this peer.
            if (nodestate->pindexBestKnownBlock &&
                nodestate->pindexBestKnownBlock->nChainWork <
                    nMinimumChainWork) {
                // This peer has too little work on their headers chain to help
                // us sync -- disconnect if it is an outbound disconnection
                // candidate.
                // Note: We compare their tip to nMinimumChainWork (rather than
                // m_chainman.ActiveChain().Tip()) because we won't start block
                // download until we have a headers chain that has at least
                // nMinimumChainWork, even if a peer has a chain past our tip,
                // as an anti-DoS measure.
                if (pfrom.IsOutboundOrBlockRelayConn()) {
                    LogPrintf("Disconnecting outbound peer %d -- headers "
                              "chain has insufficient work\n",
                              pfrom.GetId());
                    pfrom.fDisconnect = true;
                }
            }
        }

        // If this is an outbound full-relay peer, check to see if we should
        // protect it from the bad/lagging chain logic.
        // Note that outbound block-relay peers are excluded from this
        // protection, and thus always subject to eviction under the bad/lagging
        // chain logic.
        // See ChainSyncTimeoutState.
        if (!pfrom.fDisconnect && pfrom.IsFullOutboundConn() &&
            nodestate->pindexBestKnownBlock != nullptr) {
            if (m_outbound_peers_with_protect_from_disconnect <
                    MAX_OUTBOUND_PEERS_TO_PROTECT_FROM_DISCONNECT &&
                nodestate->pindexBestKnownBlock->nChainWork >=
                    m_chainman.ActiveChain().Tip()->nChainWork &&
                !nodestate->m_chain_sync.m_protect) {
                LogPrint(BCLog::NET,
                         "Protecting outbound peer=%d from eviction\n",
                         pfrom.GetId());
                nodestate->m_chain_sync.m_protect = true;
                ++m_outbound_peers_with_protect_from_disconnect;
            }
        }
    }
}

/**
 * Reconsider orphan transactions after a parent has been accepted to the
 * mempool.
 *
 * @param[in,out]  orphan_work_set  The set of orphan transactions to
 *    reconsider. Generally only one orphan will be reconsidered on each call of
 *    this function. This set may be added to if accepting an orphan causes its
 *    children to be reconsidered.
 */
void PeerManagerImpl::ProcessOrphanTx(const Config &config,
                                      std::set<TxId> &orphan_work_set) {
    AssertLockHeld(cs_main);
    AssertLockHeld(g_cs_orphans);
    while (!orphan_work_set.empty()) {
        const TxId orphanTxId = *orphan_work_set.begin();
        orphan_work_set.erase(orphan_work_set.begin());

        const auto [porphanTx, from_peer] = m_orphanage.GetTx(orphanTxId);
        if (porphanTx == nullptr) {
            continue;
        }

        const MempoolAcceptResult result =
            m_chainman.ProcessTransaction(porphanTx);
        const TxValidationState &state = result.m_state;
        if (result.m_result_type == MempoolAcceptResult::ResultType::VALID) {
            LogPrint(BCLog::MEMPOOL, "   accepted orphan tx %s\n",
                     orphanTxId.ToString());
            RelayTransaction(orphanTxId);
            m_orphanage.AddChildrenToWorkSet(*porphanTx, orphan_work_set);
            m_orphanage.EraseTx(orphanTxId);
            break;
        } else if (state.GetResult() != TxValidationResult::TX_MISSING_INPUTS) {
            if (state.IsInvalid()) {
                LogPrint(BCLog::MEMPOOL,
                         "   invalid orphan tx %s from peer=%d. %s\n",
                         orphanTxId.ToString(), from_peer, state.ToString());
                // Punish peer that gave us an invalid orphan tx
                MaybePunishNodeForTx(from_peer, state);
            }
            // Has inputs but not accepted to mempool
            // Probably non-standard or insufficient fee
            LogPrint(BCLog::MEMPOOL, "   removed orphan tx %s\n",
                     orphanTxId.ToString());

            assert(recentRejects);
            recentRejects->insert(orphanTxId);

            m_orphanage.EraseTx(orphanTxId);
            break;
        }
    }
}

bool PeerManagerImpl::PrepareBlockFilterRequest(
    CNode &peer, BlockFilterType filter_type, uint32_t start_height,
    const BlockHash &stop_hash, uint32_t max_height_diff,
    const CBlockIndex *&stop_index, BlockFilterIndex *&filter_index) {
    const bool supported_filter_type =
        (filter_type == BlockFilterType::BASIC &&
         (peer.GetLocalServices() & NODE_COMPACT_FILTERS));
    if (!supported_filter_type) {
        LogPrint(BCLog::NET,
                 "peer %d requested unsupported block filter type: %d\n",
                 peer.GetId(), static_cast<uint8_t>(filter_type));
        peer.fDisconnect = true;
        return false;
    }

    {
        LOCK(cs_main);
        stop_index = m_chainman.m_blockman.LookupBlockIndex(stop_hash);

        // Check that the stop block exists and the peer would be allowed to
        // fetch it.
        if (!stop_index || !BlockRequestAllowed(stop_index)) {
            LogPrint(BCLog::NET, "peer %d requested invalid block hash: %s\n",
                     peer.GetId(), stop_hash.ToString());
            peer.fDisconnect = true;
            return false;
        }
    }

    uint32_t stop_height = stop_index->nHeight;
    if (start_height > stop_height) {
        LogPrint(
            BCLog::NET,
            "peer %d sent invalid getcfilters/getcfheaders with " /* Continued
                                                                   */
            "start height %d and stop height %d\n",
            peer.GetId(), start_height, stop_height);
        peer.fDisconnect = true;
        return false;
    }
    if (stop_height - start_height >= max_height_diff) {
        LogPrint(BCLog::NET,
                 "peer %d requested too many cfilters/cfheaders: %d / %d\n",
                 peer.GetId(), stop_height - start_height + 1, max_height_diff);
        peer.fDisconnect = true;
        return false;
    }

    filter_index = GetBlockFilterIndex(filter_type);
    if (!filter_index) {
        LogPrint(BCLog::NET, "Filter index for supported type %s not found\n",
                 BlockFilterTypeName(filter_type));
        return false;
    }

    return true;
}

void PeerManagerImpl::ProcessGetCFilters(CNode &peer, CDataStream &vRecv) {
    uint8_t filter_type_ser;
    uint32_t start_height;
    BlockHash stop_hash;

    vRecv >> filter_type_ser >> start_height >> stop_hash;

    const BlockFilterType filter_type =
        static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex *stop_index;
    BlockFilterIndex *filter_index;
    if (!PrepareBlockFilterRequest(peer, filter_type, start_height, stop_hash,
                                   MAX_GETCFILTERS_SIZE, stop_index,
                                   filter_index)) {
        return;
    }

    std::vector<BlockFilter> filters;
    if (!filter_index->LookupFilterRange(start_height, stop_index, filters)) {
        LogPrint(BCLog::NET,
                 "Failed to find block filter in index: filter_type=%s, "
                 "start_height=%d, stop_hash=%s\n",
                 BlockFilterTypeName(filter_type), start_height,
                 stop_hash.ToString());
        return;
    }

    for (const auto &filter : filters) {
        CSerializedNetMsg msg = CNetMsgMaker(peer.GetCommonVersion())
                                    .Make(NetMsgType::CFILTER, filter);
        m_connman.PushMessage(&peer, std::move(msg));
    }
}

void PeerManagerImpl::ProcessGetCFHeaders(CNode &peer, CDataStream &vRecv) {
    uint8_t filter_type_ser;
    uint32_t start_height;
    BlockHash stop_hash;

    vRecv >> filter_type_ser >> start_height >> stop_hash;

    const BlockFilterType filter_type =
        static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex *stop_index;
    BlockFilterIndex *filter_index;
    if (!PrepareBlockFilterRequest(peer, filter_type, start_height, stop_hash,
                                   MAX_GETCFHEADERS_SIZE, stop_index,
                                   filter_index)) {
        return;
    }

    uint256 prev_header;
    if (start_height > 0) {
        const CBlockIndex *const prev_block =
            stop_index->GetAncestor(static_cast<int>(start_height - 1));
        if (!filter_index->LookupFilterHeader(prev_block, prev_header)) {
            LogPrint(BCLog::NET,
                     "Failed to find block filter header in index: "
                     "filter_type=%s, block_hash=%s\n",
                     BlockFilterTypeName(filter_type),
                     prev_block->GetBlockHash().ToString());
            return;
        }
    }

    std::vector<uint256> filter_hashes;
    if (!filter_index->LookupFilterHashRange(start_height, stop_index,
                                             filter_hashes)) {
        LogPrint(BCLog::NET,
                 "Failed to find block filter hashes in index: filter_type=%s, "
                 "start_height=%d, stop_hash=%s\n",
                 BlockFilterTypeName(filter_type), start_height,
                 stop_hash.ToString());
        return;
    }

    CSerializedNetMsg msg =
        CNetMsgMaker(peer.GetCommonVersion())
            .Make(NetMsgType::CFHEADERS, filter_type_ser,
                  stop_index->GetBlockHash(), prev_header, filter_hashes);
    m_connman.PushMessage(&peer, std::move(msg));
}

void PeerManagerImpl::ProcessGetCFCheckPt(CNode &peer, CDataStream &vRecv) {
    uint8_t filter_type_ser;
    BlockHash stop_hash;

    vRecv >> filter_type_ser >> stop_hash;

    const BlockFilterType filter_type =
        static_cast<BlockFilterType>(filter_type_ser);

    const CBlockIndex *stop_index;
    BlockFilterIndex *filter_index;
    if (!PrepareBlockFilterRequest(
            peer, filter_type, /*start_height=*/0, stop_hash,
            /*max_height_diff=*/std::numeric_limits<uint32_t>::max(),
            stop_index, filter_index)) {
        return;
    }

    std::vector<uint256> headers(stop_index->nHeight / CFCHECKPT_INTERVAL);

    // Populate headers.
    const CBlockIndex *block_index = stop_index;
    for (int i = headers.size() - 1; i >= 0; i--) {
        int height = (i + 1) * CFCHECKPT_INTERVAL;
        block_index = block_index->GetAncestor(height);

        if (!filter_index->LookupFilterHeader(block_index, headers[i])) {
            LogPrint(BCLog::NET,
                     "Failed to find block filter header in index: "
                     "filter_type=%s, block_hash=%s\n",
                     BlockFilterTypeName(filter_type),
                     block_index->GetBlockHash().ToString());
            return;
        }
    }

    CSerializedNetMsg msg = CNetMsgMaker(peer.GetCommonVersion())
                                .Make(NetMsgType::CFCHECKPT, filter_type_ser,
                                      stop_index->GetBlockHash(), headers);
    m_connman.PushMessage(&peer, std::move(msg));
}

bool IsAvalancheMessageType(const std::string &msg_type) {
    return msg_type == NetMsgType::AVAHELLO ||
           msg_type == NetMsgType::AVAPOLL ||
           msg_type == NetMsgType::AVARESPONSE ||
           msg_type == NetMsgType::AVAPROOF ||
           msg_type == NetMsgType::GETAVAADDR ||
           msg_type == NetMsgType::GETAVAPROOFS ||
           msg_type == NetMsgType::AVAPROOFS ||
           msg_type == NetMsgType::AVAPROOFSREQ;
}

uint32_t
PeerManagerImpl::GetAvalancheVoteForBlock(const BlockHash &hash) const {
    AssertLockHeld(cs_main);

    const CBlockIndex *pindex = m_chainman.m_blockman.LookupBlockIndex(hash);

    // Unknown block.
    if (!pindex) {
        return -1;
    }

    // Invalid block
    if (pindex->nStatus.isInvalid()) {
        return 1;
    }

    // Parked block
    if (pindex->nStatus.isOnParkedChain()) {
        return 2;
    }

    const CBlockIndex *pindexTip = m_chainman.ActiveChain().Tip();
    const CBlockIndex *pindexFork = LastCommonAncestor(pindex, pindexTip);

    // Active block.
    if (pindex == pindexFork) {
        return 0;
    }

    // Fork block.
    if (pindexFork != pindexTip) {
        return 3;
    }

    // Missing block data.
    if (!pindex->nStatus.hasData()) {
        return -2;
    }

    // This block is built on top of the tip, we have the data, it
    // is pending connection or rejection.
    return -3;
};

uint32_t PeerManagerImpl::GetAvalancheVoteForTx(const TxId &id) const {
    // Accepted in mempool, or in a recent block
    if (m_mempool.exists(id) ||
        WITH_LOCK(m_recent_confirmed_transactions_mutex,
                  return m_recent_confirmed_transactions->contains(id))) {
        return 0;
    }

    // Invalid tx
    assert(recentRejects);
    if (recentRejects->contains(id)) {
        return 1;
    }

    // Orphan tx
    if (m_orphanage.HaveTx(id)) {
        return 2;
    }

    // Unknown tx
    return -1;
};

/**
 * Decide a response for an Avalanche poll about the given proof.
 *
 * @param[in] id   The id of the proof being polled for
 * @return         Our current vote for the proof
 */
static uint32_t getAvalancheVoteForProof(const avalanche::ProofId &id) {
    assert(g_avalanche);

    // Rejected proof
    if (WITH_LOCK(cs_invalidProofs, return invalidProofs->contains(id))) {
        return 1;
    }

    return g_avalanche->withPeerManager([&id](avalanche::PeerManager &pm) {
        // The proof is actively bound to a peer
        if (pm.isBoundToPeer(id)) {
            return 0;
        }

        // Unknown proof
        if (!pm.exists(id)) {
            return -1;
        }

        // Immature proof
        if (pm.isImmature(id)) {
            return 2;
        }

        // Not immature, but in conflict with an actively bound proof
        if (pm.isInConflictingPool(id)) {
            return 3;
        }

        // The proof is known, not rejected, not immature, not a conflict, but
        // for some reason unbound. This should not happen if the above pools
        // are managed correctly, but added for robustness.
        return -2;
    });
};

void PeerManagerImpl::ProcessBlock(const Config &config, CNode &node,
                                   const std::shared_ptr<const CBlock> &block,
                                   bool force_processing) {
    bool new_block{false};
    m_chainman.ProcessNewBlock(config, block, force_processing, &new_block);
    if (new_block) {
        node.m_last_block_time = GetTime<std::chrono::seconds>();
    } else {
        LOCK(cs_main);
        mapBlockSource.erase(block->GetHash());
    }
}

void PeerManagerImpl::ProcessMessage(
    const Config &config, CNode &pfrom, const std::string &msg_type,
    CDataStream &vRecv, const std::chrono::microseconds time_received,
    const std::atomic<bool> &interruptMsgProc) {
    LogPrint(BCLog::NET, "received: %s (%u bytes) peer=%d\n",
             SanitizeString(msg_type), vRecv.size(), pfrom.GetId());

    PeerRef peer = GetPeerRef(pfrom.GetId());
    if (peer == nullptr) {
        return;
    }

    if (IsAvalancheMessageType(msg_type)) {
        if (!g_avalanche) {
            LogPrint(BCLog::AVALANCHE,
                     "Avalanche is not initialized, ignoring %s message\n",
                     msg_type);
            return;
        }

        if (!isAvalancheEnabled(gArgs)) {
            // If avalanche is not enabled, ignore avalanche messages
            return;
        }
    }

    if (msg_type == NetMsgType::VERSION) {
        // Each connection can only send one version message
        if (pfrom.nVersion != 0) {
            Misbehaving(pfrom, 1, "redundant version message");
            return;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        uint64_t nServiceInt;
        ServiceFlags nServices;
        int nVersion;
        std::string cleanSubVer;
        int starting_height = -1;
        bool fRelay = true;
        uint64_t nExtraEntropy = 1;

        vRecv >> nVersion >> nServiceInt >> nTime >> addrMe;
        if (nTime < 0) {
            nTime = 0;
        }
        nServices = ServiceFlags(nServiceInt);
        if (!pfrom.IsInboundConn()) {
            m_addrman.SetServices(pfrom.addr, nServices);
        }
        if (pfrom.ExpectServicesFromConn() &&
            !HasAllDesirableServiceFlags(nServices)) {
            LogPrint(BCLog::NET,
                     "peer=%d does not offer the expected services "
                     "(%08x offered, %08x expected); disconnecting\n",
                     pfrom.GetId(), nServices,
                     GetDesirableServiceFlags(nServices));
            pfrom.fDisconnect = true;
            return;
        }

        if (pfrom.IsAvalancheOutboundConnection() &&
            !(nServices & NODE_AVALANCHE)) {
            LogPrint(
                BCLog::AVALANCHE,
                "peer=%d does not offer the avalanche service; disconnecting\n",
                pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }

        if (nVersion < MIN_PEER_PROTO_VERSION) {
            // disconnect from peers older than this proto version
            LogPrint(BCLog::NET,
                     "peer=%d using obsolete version %i; disconnecting\n",
                     pfrom.GetId(), nVersion);
            pfrom.fDisconnect = true;
            return;
        }

        if (!vRecv.empty()) {
            vRecv >> addrFrom >> nNonce;
        }
        if (!vRecv.empty()) {
            std::string strSubVer;
            vRecv >> LIMITED_STRING(strSubVer, MAX_SUBVERSION_LENGTH);
            cleanSubVer = SanitizeString(strSubVer);
        }
        if (!vRecv.empty()) {
            vRecv >> starting_height;
        }
        if (!vRecv.empty()) {
            vRecv >> fRelay;
        }
        if (!vRecv.empty()) {
            vRecv >> nExtraEntropy;
        }
        // Disconnect if we connected to ourself
        if (pfrom.IsInboundConn() && !m_connman.CheckIncomingNonce(nNonce)) {
            LogPrintf("connected to self at %s, disconnecting\n",
                      pfrom.addr.ToString());
            pfrom.fDisconnect = true;
            return;
        }

        if (pfrom.IsInboundConn() && addrMe.IsRoutable()) {
            SeenLocal(addrMe);
        }

        // Inbound peers send us their version message when they connect.
        // We send our version message in response.
        if (pfrom.IsInboundConn()) {
            PushNodeVersion(config, pfrom);
        }

        // Change version
        const int greatest_common_version =
            std::min(nVersion, PROTOCOL_VERSION);
        pfrom.SetCommonVersion(greatest_common_version);
        pfrom.nVersion = nVersion;

        const CNetMsgMaker msg_maker(greatest_common_version);

        m_connman.PushMessage(&pfrom, msg_maker.Make(NetMsgType::VERACK));

        // Signal ADDRv2 support (BIP155).
        m_connman.PushMessage(&pfrom, msg_maker.Make(NetMsgType::SENDADDRV2));

        pfrom.nServices = nServices;
        pfrom.SetAddrLocal(addrMe);
        {
            LOCK(pfrom.cs_SubVer);
            pfrom.cleanSubVer = cleanSubVer;
        }
        peer->m_starting_height = starting_height;

        // set nodes not relaying blocks and tx and not serving (parts) of the
        // historical blockchain as "clients"
        pfrom.fClient = (!(nServices & NODE_NETWORK) &&
                         !(nServices & NODE_NETWORK_LIMITED));

        // set nodes not capable of serving the complete blockchain history as
        // "limited nodes"
        pfrom.m_limited_node =
            (!(nServices & NODE_NETWORK) && (nServices & NODE_NETWORK_LIMITED));

        if (pfrom.m_tx_relay != nullptr) {
            LOCK(pfrom.m_tx_relay->cs_filter);
            // set to true after we get the first filter* message
            pfrom.m_tx_relay->fRelayTxes = fRelay;
        }

        pfrom.nRemoteHostNonce = nNonce;
        pfrom.nRemoteExtraEntropy = nExtraEntropy;

        // Potentially mark this peer as a preferred download peer.
        {
            LOCK(cs_main);
            UpdatePreferredDownload(pfrom, State(pfrom.GetId()));
        }

        // Self advertisement & GETADDR logic
        if (!pfrom.IsInboundConn() && SetupAddressRelay(pfrom, *peer)) {
            // For outbound peers, we try to relay our address (so that other
            // nodes can try to find us more quickly, as we have no guarantee
            // that an outbound peer is even aware of how to reach us) and do a
            // one-time address fetch (to help populate/update our addrman). If
            // we're starting up for the first time, our addrman may be pretty
            // empty and no one will know who we are, so these mechanisms are
            // important to help us connect to the network.
            //
            // We skip this for block-relay-only peers. We want to avoid
            // potentially leaking addr information and we do not want to
            // indicate to the peer that we will participate in addr relay.
            if (fListen &&
                !m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
                CAddress addr =
                    GetLocalAddress(&pfrom.addr, pfrom.GetLocalServices());
                FastRandomContext insecure_rand;
                if (addr.IsRoutable()) {
                    LogPrint(BCLog::NET,
                             "ProcessMessages: advertising address %s\n",
                             addr.ToString());
                    PushAddress(*peer, addr, insecure_rand);
                } else if (IsPeerAddrLocalGood(&pfrom)) {
                    addr.SetIP(addrMe);
                    LogPrint(BCLog::NET,
                             "ProcessMessages: advertising address %s\n",
                             addr.ToString());
                    PushAddress(*peer, addr, insecure_rand);
                }
            }

            // Get recent addresses
            m_connman.PushMessage(&pfrom, CNetMsgMaker(greatest_common_version)
                                              .Make(NetMsgType::GETADDR));
            peer->m_getaddr_sent = true;
            // When requesting a getaddr, accept an additional MAX_ADDR_TO_SEND
            // addresses in response (bypassing the
            // MAX_ADDR_PROCESSING_TOKEN_BUCKET limit).
            WITH_LOCK(peer->m_addr_token_bucket_mutex,
                      peer->m_addr_token_bucket += GetMaxAddrToSend());
        }

        if (!pfrom.IsInboundConn()) {
            // For non-inbound connections, we update the addrman to record
            // connection success so that addrman will have an up-to-date
            // notion of which peers are online and available.
            //
            // While we strive to not leak information about block-relay-only
            // connections via the addrman, not moving an address to the tried
            // table is also potentially detrimental because new-table entries
            // are subject to eviction in the event of addrman collisions.  We
            // mitigate the information-leak by never calling
            // AddrMan::Connected() on block-relay-only peers; see
            // FinalizeNode().
            //
            // This moves an address from New to Tried table in Addrman,
            // resolves tried-table collisions, etc.
            m_addrman.Good(pfrom.addr);
        }

        std::string remoteAddr;
        if (fLogIPs) {
            remoteAddr = ", peeraddr=" + pfrom.addr.ToString();
        }

        LogPrint(BCLog::NET,
                 "receive version message: [%s] %s: version %d, blocks=%d, "
                 "us=%s, txrelay=%d, peer=%d%s\n",
                 pfrom.addr.ToString(), cleanSubVer, pfrom.nVersion,
                 peer->m_starting_height, addrMe.ToString(), fRelay,
                 pfrom.GetId(), remoteAddr);

        int64_t currentTime = GetTime();
        int64_t nTimeOffset = nTime - currentTime;
        pfrom.nTimeOffset = nTimeOffset;
        if (nTime < int64_t(m_chainparams.GenesisBlock().nTime)) {
            // Ignore time offsets that are improbable (before the Genesis
            // block) and may underflow our adjusted time.
            Misbehaving(pfrom, 20,
                        "Ignoring invalid timestamp in version message");
        } else if (!pfrom.IsInboundConn()) {
            // Don't use timedata samples from inbound peers to make it
            // harder for others to tamper with our adjusted time.
            AddTimeData(pfrom.addr, nTimeOffset);
        }

        // Feeler connections exist only to verify if address is online.
        if (pfrom.IsFeelerConn()) {
            LogPrint(BCLog::NET,
                     "feeler connection completed peer=%d; disconnecting\n",
                     pfrom.GetId());
            pfrom.fDisconnect = true;
        }
        return;
    }

    if (pfrom.nVersion == 0) {
        // Must have a version message before anything else
        Misbehaving(pfrom, 10, "non-version message before version handshake");
        return;
    }

    // At this point, the outgoing message serialization version can't change.
    const CNetMsgMaker msgMaker(pfrom.GetCommonVersion());

    if (msg_type == NetMsgType::VERACK) {
        if (pfrom.fSuccessfullyConnected) {
            LogPrint(BCLog::NET,
                     "ignoring redundant verack message from peer=%d\n",
                     pfrom.GetId());
            return;
        }

        if (!pfrom.IsInboundConn()) {
            LogPrintf(
                "New outbound peer connected: version: %d, blocks=%d, "
                "peer=%d%s (%s)\n",
                pfrom.nVersion.load(), peer->m_starting_height, pfrom.GetId(),
                (fLogIPs ? strprintf(", peeraddr=%s", pfrom.addr.ToString())
                         : ""),
                pfrom.ConnectionTypeAsString());
        }

        if (pfrom.GetCommonVersion() >= SENDHEADERS_VERSION) {
            // Tell our peer we prefer to receive headers rather than inv's
            // We send this to non-NODE NETWORK peers as well, because even
            // non-NODE NETWORK peers can announce blocks (such as pruning
            // nodes)
            m_connman.PushMessage(&pfrom,
                                  msgMaker.Make(NetMsgType::SENDHEADERS));
        }

        if (pfrom.GetCommonVersion() >= SHORT_IDS_BLOCKS_VERSION) {
            // Tell our peer we are willing to provide version 1 or 2
            // cmpctblocks. However, we do not request new block announcements
            // using cmpctblock messages. We send this to non-NODE NETWORK peers
            // as well, because they may wish to request compact blocks from us.
            bool fAnnounceUsingCMPCTBLOCK = false;
            uint64_t nCMPCTBLOCKVersion = 1;
            m_connman.PushMessage(&pfrom,
                                  msgMaker.Make(NetMsgType::SENDCMPCT,
                                                fAnnounceUsingCMPCTBLOCK,
                                                nCMPCTBLOCKVersion));
        }

        if (g_avalanche && isAvalancheEnabled(gArgs)) {
            if (g_avalanche->sendHello(&pfrom)) {
                LogPrint(BCLog::AVALANCHE, "Send avahello to peer %d\n",
                         pfrom.GetId());

                auto localProof = g_avalanche->getLocalProof();

                if (localProof) {
                    // Add our proof id to the list or the recently announced
                    // proof INVs to this peer. This is used for filtering which
                    // INV can be requested for download.
                    LOCK(cs_main);
                    State(pfrom.GetId())
                        ->m_recently_announced_proofs.insert(
                            localProof->getId());
                }
            }
        }

        pfrom.fSuccessfullyConnected = true;
        return;
    }

    if (!pfrom.fSuccessfullyConnected) {
        // Must have a verack message before anything else
        Misbehaving(pfrom, 10, "non-verack message before version handshake");
        return;
    }

    if (msg_type == NetMsgType::ADDR || msg_type == NetMsgType::ADDRV2) {
        int stream_version = vRecv.GetVersion();
        if (msg_type == NetMsgType::ADDRV2) {
            // Add ADDRV2_FORMAT to the version so that the CNetAddr and
            // CAddress unserialize methods know that an address in v2 format is
            // coming.
            stream_version |= ADDRV2_FORMAT;
        }

        OverrideStream<CDataStream> s(&vRecv, vRecv.GetType(), stream_version);
        std::vector<CAddress> vAddr;

        s >> vAddr;

        if (!SetupAddressRelay(pfrom, *peer)) {
            LogPrint(BCLog::NET, "ignoring %s message from %s peer=%d\n",
                     msg_type, pfrom.ConnectionTypeAsString(), pfrom.GetId());
            return;
        }

        if (vAddr.size() > GetMaxAddrToSend()) {
            Misbehaving(
                pfrom, 20,
                strprintf("%s message size = %u", msg_type, vAddr.size()));
            return;
        }

        // Store the new addresses
        std::vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;

        // Update/increment addr rate limiting bucket.
        const auto current_time = GetTime<std::chrono::microseconds>();
        {
            LOCK(peer->m_addr_token_bucket_mutex);
            if (peer->m_addr_token_bucket < MAX_ADDR_PROCESSING_TOKEN_BUCKET) {
                // Don't increment bucket if it's already full
                const auto time_diff =
                    std::max(current_time - peer->m_addr_token_timestamp, 0us);
                const double increment =
                    CountSecondsDouble(time_diff) * MAX_ADDR_RATE_PER_SECOND;
                peer->m_addr_token_bucket =
                    std::min<double>(peer->m_addr_token_bucket + increment,
                                     MAX_ADDR_PROCESSING_TOKEN_BUCKET);
            }
        }
        peer->m_addr_token_timestamp = current_time;

        const bool rate_limited =
            !pfrom.HasPermission(NetPermissionFlags::Addr);
        uint64_t num_proc = 0;
        uint64_t num_rate_limit = 0;
        Shuffle(vAddr.begin(), vAddr.end(), FastRandomContext());
        for (CAddress &addr : vAddr) {
            if (interruptMsgProc) {
                return;
            }

            {
                LOCK(peer->m_addr_token_bucket_mutex);
                // Apply rate limiting.
                if (peer->m_addr_token_bucket < 1.0) {
                    if (rate_limited) {
                        ++num_rate_limit;
                        continue;
                    }
                } else {
                    peer->m_addr_token_bucket -= 1.0;
                }
            }

            // We only bother storing full nodes, though this may include things
            // which we would not make an outbound connection to, in part
            // because we may make feeler connections to them.
            if (!MayHaveUsefulAddressDB(addr.nServices) &&
                !HasAllDesirableServiceFlags(addr.nServices)) {
                continue;
            }

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60) {
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            }
            AddAddressKnown(*peer, addr);
            if (m_banman &&
                (m_banman->IsDiscouraged(addr) || m_banman->IsBanned(addr))) {
                // Do not process banned/discouraged addresses beyond
                // remembering we received them
                continue;
            }
            ++num_proc;
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !peer->m_getaddr_sent &&
                vAddr.size() <= 10 && addr.IsRoutable()) {
                // Relay to a limited number of other nodes
                RelayAddress(pfrom.GetId(), addr, fReachable);
            }
            // Do not store addresses outside our network
            if (fReachable) {
                vAddrOk.push_back(addr);
            }
        }
        peer->m_addr_processed += num_proc;
        peer->m_addr_rate_limited += num_rate_limit;
        LogPrint(BCLog::NET,
                 "Received addr: %u addresses (%u processed, %u rate-limited) "
                 "from peer=%d\n",
                 vAddr.size(), num_proc, num_rate_limit, pfrom.GetId());

        m_addrman.Add(vAddrOk, pfrom.addr, 2 * 60 * 60);
        if (vAddr.size() < 1000) {
            peer->m_getaddr_sent = false;
        }

        // AddrFetch: Require multiple addresses to avoid disconnecting on
        // self-announcements
        if (pfrom.IsAddrFetchConn() && vAddr.size() > 1) {
            LogPrint(BCLog::NET,
                     "addrfetch connection completed peer=%d; disconnecting\n",
                     pfrom.GetId());
            pfrom.fDisconnect = true;
        }
        return;
    }

    if (msg_type == NetMsgType::SENDADDRV2) {
        peer->m_wants_addrv2 = true;
        return;
    }

    if (msg_type == NetMsgType::SENDHEADERS) {
        LOCK(cs_main);
        State(pfrom.GetId())->fPreferHeaders = true;
        return;
    }

    if (msg_type == NetMsgType::SENDCMPCT) {
        bool fAnnounceUsingCMPCTBLOCK = false;
        uint64_t nCMPCTBLOCKVersion = 0;
        vRecv >> fAnnounceUsingCMPCTBLOCK >> nCMPCTBLOCKVersion;
        if (nCMPCTBLOCKVersion == 1) {
            LOCK(cs_main);
            // fProvidesHeaderAndIDs is used to "lock in" version of compact
            // blocks we send.
            if (!State(pfrom.GetId())->fProvidesHeaderAndIDs) {
                State(pfrom.GetId())->fProvidesHeaderAndIDs = true;
            }

            State(pfrom.GetId())->fPreferHeaderAndIDs =
                fAnnounceUsingCMPCTBLOCK;
            // save whether peer selects us as BIP152 high-bandwidth peer
            // (receiving sendcmpct(1) signals high-bandwidth,
            // sendcmpct(0) low-bandwidth)
            pfrom.m_bip152_highbandwidth_from = fAnnounceUsingCMPCTBLOCK;
            if (!State(pfrom.GetId())->fSupportsDesiredCmpctVersion) {
                State(pfrom.GetId())->fSupportsDesiredCmpctVersion = true;
            }
        }
        return;
    }

    if (msg_type == NetMsgType::INV) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            Misbehaving(pfrom, 20,
                        strprintf("inv message size = %u", vInv.size()));
            return;
        }

        // Reject tx INVs when the -blocksonly setting is enabled, or this is a
        // block-relay-only peer
        bool reject_tx_invs{m_ignore_incoming_txs ||
                            (pfrom.m_tx_relay == nullptr)};

        // Allow peers with relay permission to send data other than blocks
        // in blocks only mode
        if (pfrom.HasPermission(NetPermissionFlags::Relay)) {
            reject_tx_invs = false;
        }

        const auto current_time = GetTime<std::chrono::microseconds>();
        std::optional<BlockHash> best_block;

        auto logInv = [&](const CInv &inv, bool fAlreadyHave) {
            LogPrint(BCLog::NET, "got inv: %s  %s peer=%d\n", inv.ToString(),
                     fAlreadyHave ? "have" : "new", pfrom.GetId());
        };

        for (CInv &inv : vInv) {
            if (interruptMsgProc) {
                return;
            }

            if (inv.IsMsgBlk()) {
                LOCK(cs_main);
                const bool fAlreadyHave = AlreadyHaveBlock(BlockHash(inv.hash));
                logInv(inv, fAlreadyHave);

                const BlockHash hash{inv.hash};
                UpdateBlockAvailability(pfrom.GetId(), hash);
                if (!fAlreadyHave && !fImporting && !fReindex &&
                    !IsBlockRequested(hash)) {
                    // Headers-first is the primary method of announcement on
                    // the network. If a node fell back to sending blocks by
                    // inv, it's probably for a re-org. The final block hash
                    // provided should be the highest, so send a getheaders and
                    // then fetch the blocks we need to catch up.
                    best_block = std::move(hash);
                }

                continue;
            }

            if (inv.IsMsgProof()) {
                const avalanche::ProofId proofid(inv.hash);
                const bool fAlreadyHave = AlreadyHaveProof(proofid);
                logInv(inv, fAlreadyHave);
                pfrom.AddKnownProof(proofid);

                if (!fAlreadyHave && g_avalanche && isAvalancheEnabled(gArgs) &&
                    !m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
                    const bool preferred = isPreferredDownloadPeer(pfrom);

                    LOCK(cs_proofrequest);
                    AddProofAnnouncement(pfrom, proofid, current_time,
                                         preferred);
                }
                continue;
            }

            if (inv.IsMsgTx()) {
                LOCK(cs_main);
                const TxId txid(inv.hash);
                const bool fAlreadyHave = AlreadyHaveTx(txid);
                logInv(inv, fAlreadyHave);

                pfrom.AddKnownTx(txid);
                if (reject_tx_invs) {
                    LogPrint(BCLog::NET,
                             "transaction (%s) inv sent in violation of "
                             "protocol, disconnecting peer=%d\n",
                             txid.ToString(), pfrom.GetId());
                    pfrom.fDisconnect = true;
                    return;
                } else if (!fAlreadyHave && !m_chainman.ActiveChainstate()
                                                 .IsInitialBlockDownload()) {
                    AddTxAnnouncement(pfrom, txid, current_time);
                }

                continue;
            }

            LogPrint(BCLog::NET,
                     "Unknown inv type \"%s\" received from peer=%d\n",
                     inv.ToString(), pfrom.GetId());
        }

        if (best_block) {
            m_connman.PushMessage(
                &pfrom, msgMaker.Make(NetMsgType::GETHEADERS,
                                      m_chainman.ActiveChain().GetLocator(
                                          m_chainman.m_best_header),
                                      *best_block));
            LogPrint(BCLog::NET, "getheaders (%d) %s to peer=%d\n",
                     m_chainman.m_best_header->nHeight, best_block->ToString(),
                     pfrom.GetId());
        }

        return;
    }

    if (msg_type == NetMsgType::GETDATA) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            Misbehaving(pfrom, 20,
                        strprintf("getdata message size = %u", vInv.size()));
            return;
        }

        LogPrint(BCLog::NET, "received getdata (%u invsz) peer=%d\n",
                 vInv.size(), pfrom.GetId());

        if (vInv.size() > 0) {
            LogPrint(BCLog::NET, "received getdata for: %s peer=%d\n",
                     vInv[0].ToString(), pfrom.GetId());
        }

        {
            LOCK(peer->m_getdata_requests_mutex);
            peer->m_getdata_requests.insert(peer->m_getdata_requests.end(),
                                            vInv.begin(), vInv.end());
            ProcessGetData(config, pfrom, *peer, interruptMsgProc);
        }

        return;
    }

    if (msg_type == NetMsgType::GETBLOCKS) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET,
                     "getblocks locator size %lld > %d, disconnect peer=%d\n",
                     locator.vHave.size(), MAX_LOCATOR_SZ, pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }

        // We might have announced the currently-being-connected tip using a
        // compact block, which resulted in the peer sending a getblocks
        // request, which we would otherwise respond to without the new block.
        // To avoid this situation we simply verify that we are on our best
        // known chain now. This is super overkill, but we handle it better
        // for getheaders requests, and there are no known nodes which support
        // compact blocks but still use getblocks to request blocks.
        {
            std::shared_ptr<const CBlock> a_recent_block;
            {
                LOCK(cs_most_recent_block);
                a_recent_block = most_recent_block;
            }
            BlockValidationState state;
            if (!m_chainman.ActiveChainstate().ActivateBestChain(
                    config, state, a_recent_block)) {
                LogPrint(BCLog::NET, "failed to activate chain (%s)\n",
                         state.ToString());
            }
        }

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        const CBlockIndex *pindex =
            m_chainman.ActiveChainstate().FindForkInGlobalIndex(locator);

        // Send the rest of the chain
        if (pindex) {
            pindex = m_chainman.ActiveChain().Next(pindex);
        }
        int nLimit = 500;
        LogPrint(BCLog::NET, "getblocks %d to %s limit %d from peer=%d\n",
                 (pindex ? pindex->nHeight : -1),
                 hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit,
                 pfrom.GetId());
        for (; pindex; pindex = m_chainman.ActiveChain().Next(pindex)) {
            if (pindex->GetBlockHash() == hashStop) {
                LogPrint(BCLog::NET, "  getblocks stopping at %d %s\n",
                         pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            // If pruning, don't inv blocks unless we have on disk and are
            // likely to still have for some reasonable time window (1 hour)
            // that block relay might require.
            const int nPrunedBlocksLikelyToHave =
                MIN_BLOCKS_TO_KEEP -
                3600 / m_chainparams.GetConsensus().nPowTargetSpacing;
            if (fPruneMode &&
                (!pindex->nStatus.hasData() ||
                 pindex->nHeight <= m_chainman.ActiveChain().Tip()->nHeight -
                                        nPrunedBlocksLikelyToHave)) {
                LogPrint(
                    BCLog::NET,
                    " getblocks stopping, pruned or too old block at %d %s\n",
                    pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            WITH_LOCK(
                peer->m_block_inv_mutex,
                peer->m_blocks_for_inv_relay.push_back(pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                // When this block is requested, we'll send an inv that'll
                // trigger the peer to getblocks the next batch of inventory.
                LogPrint(BCLog::NET, "  getblocks stopping at limit %d %s\n",
                         pindex->nHeight, pindex->GetBlockHash().ToString());
                WITH_LOCK(peer->m_block_inv_mutex, {
                    peer->m_continuation_block = pindex->GetBlockHash();
                });
                break;
            }
        }
        return;
    }

    if (msg_type == NetMsgType::GETBLOCKTXN) {
        BlockTransactionsRequest req;
        vRecv >> req;

        std::shared_ptr<const CBlock> recent_block;
        {
            LOCK(cs_most_recent_block);
            if (most_recent_block_hash == req.blockhash) {
                recent_block = most_recent_block;
            }
            // Unlock cs_most_recent_block to avoid cs_main lock inversion
        }
        if (recent_block) {
            SendBlockTransactions(pfrom, *recent_block, req);
            return;
        }

        {
            LOCK(cs_main);

            const CBlockIndex *pindex =
                m_chainman.m_blockman.LookupBlockIndex(req.blockhash);
            if (!pindex || !pindex->nStatus.hasData()) {
                LogPrint(
                    BCLog::NET,
                    "Peer %d sent us a getblocktxn for a block we don't have\n",
                    pfrom.GetId());
                return;
            }

            if (pindex->nHeight >=
                m_chainman.ActiveChain().Height() - MAX_BLOCKTXN_DEPTH) {
                CBlock block;
                bool ret = ReadBlockFromDisk(block, pindex,
                                             m_chainparams.GetConsensus());
                assert(ret);

                SendBlockTransactions(pfrom, block, req);
                return;
            }
        }

        // If an older block is requested (should never happen in practice,
        // but can happen in tests) send a block response instead of a
        // blocktxn response. Sending a full block response instead of a
        // small blocktxn response is preferable in the case where a peer
        // might maliciously send lots of getblocktxn requests to trigger
        // expensive disk reads, because it will require the peer to
        // actually receive all the data read from disk over the network.
        LogPrint(BCLog::NET,
                 "Peer %d sent us a getblocktxn for a block > %i deep\n",
                 pfrom.GetId(), MAX_BLOCKTXN_DEPTH);
        CInv inv;
        inv.type = MSG_BLOCK;
        inv.hash = req.blockhash;
        WITH_LOCK(peer->m_getdata_requests_mutex,
                  peer->m_getdata_requests.push_back(inv));
        // The message processing loop will go around again (without pausing)
        // and we'll respond then (without cs_main)
        return;
    }

    if (msg_type == NetMsgType::GETHEADERS) {
        CBlockLocator locator;
        BlockHash hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET,
                     "getheaders locator size %lld > %d, disconnect peer=%d\n",
                     locator.vHave.size(), MAX_LOCATOR_SZ, pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }

        LOCK(cs_main);
        if (m_chainman.ActiveChainstate().IsInitialBlockDownload() &&
            !pfrom.HasPermission(NetPermissionFlags::Download)) {
            LogPrint(BCLog::NET,
                     "Ignoring getheaders from peer=%d because node is in "
                     "initial block download\n",
                     pfrom.GetId());
            return;
        }

        CNodeState *nodestate = State(pfrom.GetId());
        const CBlockIndex *pindex = nullptr;
        if (locator.IsNull()) {
            // If locator is null, return the hashStop block
            pindex = m_chainman.m_blockman.LookupBlockIndex(hashStop);
            if (!pindex) {
                return;
            }

            if (!BlockRequestAllowed(pindex)) {
                LogPrint(BCLog::NET,
                         "%s: ignoring request from peer=%i for old block "
                         "header that isn't in the main chain\n",
                         __func__, pfrom.GetId());
                return;
            }
        } else {
            // Find the last block the caller has in the main chain
            pindex =
                m_chainman.ActiveChainstate().FindForkInGlobalIndex(locator);
            if (pindex) {
                pindex = m_chainman.ActiveChain().Next(pindex);
            }
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx
        // count at the end
        std::vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrint(BCLog::NET, "getheaders %d to %s from peer=%d\n",
                 (pindex ? pindex->nHeight : -1),
                 hashStop.IsNull() ? "end" : hashStop.ToString(),
                 pfrom.GetId());
        for (; pindex; pindex = m_chainman.ActiveChain().Next(pindex)) {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop) {
                break;
            }
        }
        // pindex can be nullptr either if we sent
        // m_chainman.ActiveChain().Tip() OR if our peer has
        // m_chainman.ActiveChain().Tip() (and thus we are sending an empty
        // headers message). In both cases it's safe to update
        // pindexBestHeaderSent to be our tip.
        //
        // It is important that we simply reset the BestHeaderSent value here,
        // and not max(BestHeaderSent, newHeaderSent). We might have announced
        // the currently-being-connected tip using a compact block, which
        // resulted in the peer sending a headers request, which we respond to
        // without the new block. By resetting the BestHeaderSent, we ensure we
        // will re-announce the new block via headers (or compact blocks again)
        // in the SendMessages logic.
        nodestate->pindexBestHeaderSent =
            pindex ? pindex : m_chainman.ActiveChain().Tip();
        m_connman.PushMessage(&pfrom,
                              msgMaker.Make(NetMsgType::HEADERS, vHeaders));
        return;
    }

    if (msg_type == NetMsgType::TX) {
        // Stop processing the transaction early if
        // 1) We are in blocks only mode and peer has no relay permission
        // 2) This peer is a block-relay-only peer
        if ((m_ignore_incoming_txs &&
             !pfrom.HasPermission(NetPermissionFlags::Relay)) ||
            (pfrom.m_tx_relay == nullptr)) {
            LogPrint(BCLog::NET,
                     "transaction sent in violation of protocol peer=%d\n",
                     pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }

        CTransactionRef ptx;
        vRecv >> ptx;
        const CTransaction &tx = *ptx;
        const TxId &txid = tx.GetId();
        pfrom.AddKnownTx(txid);

        LOCK2(cs_main, g_cs_orphans);

        m_txrequest.ReceivedResponse(pfrom.GetId(), txid);

        if (AlreadyHaveTx(txid)) {
            if (pfrom.HasPermission(NetPermissionFlags::ForceRelay)) {
                // Always relay transactions received from peers with
                // forcerelay permission, even if they were already in the
                // mempool, allowing the node to function as a gateway for
                // nodes hidden behind it.
                if (!m_mempool.exists(tx.GetId())) {
                    LogPrintf("Not relaying non-mempool transaction %s from "
                              "forcerelay peer=%d\n",
                              tx.GetId().ToString(), pfrom.GetId());
                } else {
                    LogPrintf("Force relaying tx %s from peer=%d\n",
                              tx.GetId().ToString(), pfrom.GetId());
                    RelayTransaction(tx.GetId());
                }
            }
            return;
        }

        const MempoolAcceptResult result = m_chainman.ProcessTransaction(ptx);
        const TxValidationState &state = result.m_state;

        if (result.m_result_type == MempoolAcceptResult::ResultType::VALID) {
            // As this version of the transaction was acceptable, we can forget
            // about any requests for it.
            m_txrequest.ForgetInvId(tx.GetId());
            RelayTransaction(tx.GetId());
            m_orphanage.AddChildrenToWorkSet(tx, peer->m_orphan_work_set);

            pfrom.m_last_tx_time = GetTime<std::chrono::seconds>();

            LogPrint(BCLog::MEMPOOL,
                     "AcceptToMemoryPool: peer=%d: accepted %s "
                     "(poolsz %u txn, %u kB)\n",
                     pfrom.GetId(), tx.GetId().ToString(), m_mempool.size(),
                     m_mempool.DynamicMemoryUsage() / 1000);

            // Recursively process any orphan transactions that depended on this
            // one
            ProcessOrphanTx(config, peer->m_orphan_work_set);
        } else if (state.GetResult() == TxValidationResult::TX_MISSING_INPUTS) {
            // It may be the case that the orphans parents have all been
            // rejected.
            bool fRejectedParents = false;

            // Deduplicate parent txids, so that we don't have to loop over
            // the same parent txid more than once down below.
            std::vector<TxId> unique_parents;
            unique_parents.reserve(tx.vin.size());
            for (const CTxIn &txin : tx.vin) {
                // We start with all parents, and then remove duplicates below.
                unique_parents.push_back(txin.prevout.GetTxId());
            }
            std::sort(unique_parents.begin(), unique_parents.end());
            unique_parents.erase(
                std::unique(unique_parents.begin(), unique_parents.end()),
                unique_parents.end());
            for (const TxId &parent_txid : unique_parents) {
                if (recentRejects->contains(parent_txid)) {
                    fRejectedParents = true;
                    break;
                }
            }
            if (!fRejectedParents) {
                const auto current_time = GetTime<std::chrono::microseconds>();

                for (const TxId &parent_txid : unique_parents) {
                    // FIXME: MSG_TX should use a TxHash, not a TxId.
                    pfrom.AddKnownTx(parent_txid);
                    if (!AlreadyHaveTx(parent_txid)) {
                        AddTxAnnouncement(pfrom, parent_txid, current_time);
                    }
                }

                if (m_orphanage.AddTx(ptx, pfrom.GetId())) {
                    AddToCompactExtraTransactions(ptx);
                }

                // Once added to the orphan pool, a tx is considered
                // AlreadyHave, and we shouldn't request it anymore.
                m_txrequest.ForgetInvId(tx.GetId());

                // DoS prevention: do not allow m_orphanage to grow
                // unbounded (see CVE-2012-3789)
                unsigned int nMaxOrphanTx = (unsigned int)std::max(
                    int64_t(0),
                    gArgs.GetIntArg("-maxorphantx",
                                    DEFAULT_MAX_ORPHAN_TRANSACTIONS));
                unsigned int nEvicted = m_orphanage.LimitOrphans(nMaxOrphanTx);
                if (nEvicted > 0) {
                    LogPrint(BCLog::MEMPOOL,
                             "orphanage overflow, removed %u tx\n", nEvicted);
                }
            } else {
                LogPrint(BCLog::MEMPOOL,
                         "not keeping orphan with rejected parents %s\n",
                         tx.GetId().ToString());
                // We will continue to reject this tx since it has rejected
                // parents so avoid re-requesting it from other peers.
                recentRejects->insert(tx.GetId());
                m_txrequest.ForgetInvId(tx.GetId());
            }
        } else {
            assert(recentRejects);
            recentRejects->insert(tx.GetId());
            m_txrequest.ForgetInvId(tx.GetId());

            if (RecursiveDynamicUsage(*ptx) < 100000) {
                AddToCompactExtraTransactions(ptx);
            }
        }

        // If a tx has been detected by recentRejects, we will have reached
        // this point and the tx will have been ignored. Because we haven't
        // submitted the tx to our mempool, we won't have computed a DoS
        // score for it or determined exactly why we consider it invalid.
        //
        // This means we won't penalize any peer subsequently relaying a DoSy
        // tx (even if we penalized the first peer who gave it to us) because
        // we have to account for recentRejects showing false positives. In
        // other words, we shouldn't penalize a peer if we aren't *sure* they
        // submitted a DoSy tx.
        //
        // Note that recentRejects doesn't just record DoSy or invalid
        // transactions, but any tx not accepted by the mempool, which may be
        // due to node policy (vs. consensus). So we can't blanket penalize a
        // peer simply for relaying a tx that our recentRejects has caught,
        // regardless of false positives.

        if (state.IsInvalid()) {
            LogPrint(BCLog::MEMPOOLREJ,
                     "%s from peer=%d was not accepted: %s\n",
                     tx.GetHash().ToString(), pfrom.GetId(), state.ToString());
            MaybePunishNodeForTx(pfrom.GetId(), state);
        }
        return;
    }

    if (msg_type == NetMsgType::CMPCTBLOCK) {
        // Ignore cmpctblock received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET,
                     "Unexpected cmpctblock message received from peer %d\n",
                     pfrom.GetId());
            return;
        }

        CBlockHeaderAndShortTxIDs cmpctblock;
        try {
            vRecv >> cmpctblock;
        } catch (std::ios_base::failure &e) {
            // This block has non contiguous or overflowing indexes
            Misbehaving(pfrom, 100, "cmpctblock-bad-indexes");
            return;
        }

        bool received_new_header = false;

        {
            LOCK(cs_main);

            if (!m_chainman.m_blockman.LookupBlockIndex(
                    cmpctblock.header.hashPrevBlock)) {
                // Doesn't connect (or is genesis), instead of DoSing in
                // AcceptBlockHeader, request deeper headers
                if (!m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
                    m_connman.PushMessage(
                        &pfrom,
                        msgMaker.Make(NetMsgType::GETHEADERS,
                                      m_chainman.ActiveChain().GetLocator(
                                          m_chainman.m_best_header),
                                      uint256()));
                }
                return;
            }

            if (!m_chainman.m_blockman.LookupBlockIndex(
                    cmpctblock.header.GetHash())) {
                received_new_header = true;
            }
        }

        const CBlockIndex *pindex = nullptr;
        BlockValidationState state;
        if (!m_chainman.ProcessNewBlockHeaders(config, {cmpctblock.header},
                                               state, &pindex)) {
            if (state.IsInvalid()) {
                MaybePunishNodeForBlock(pfrom.GetId(), state,
                                        /*via_compact_block*/ true,
                                        "invalid header via cmpctblock");
                return;
            }
        }

        // When we succeed in decoding a block's txids from a cmpctblock
        // message we typically jump to the BLOCKTXN handling code, with a
        // dummy (empty) BLOCKTXN message, to re-use the logic there in
        // completing processing of the putative block (without cs_main).
        bool fProcessBLOCKTXN = false;
        CDataStream blockTxnMsg(SER_NETWORK, PROTOCOL_VERSION);

        // If we end up treating this as a plain headers message, call that as
        // well
        // without cs_main.
        bool fRevertToHeaderProcessing = false;

        // Keep a CBlock for "optimistic" compactblock reconstructions (see
        // below)
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        bool fBlockReconstructed = false;

        {
            LOCK2(cs_main, g_cs_orphans);
            // If AcceptBlockHeader returned true, it set pindex
            assert(pindex);
            UpdateBlockAvailability(pfrom.GetId(), pindex->GetBlockHash());

            CNodeState *nodestate = State(pfrom.GetId());

            // If this was a new header with more work than our tip, update the
            // peer's last block announcement time
            if (received_new_header &&
                pindex->nChainWork >
                    m_chainman.ActiveChain().Tip()->nChainWork) {
                nodestate->m_last_block_announcement = GetTime();
            }

            std::map<BlockHash,
                     std::pair<NodeId, std::list<QueuedBlock>::iterator>>::
                iterator blockInFlightIt =
                    mapBlocksInFlight.find(pindex->GetBlockHash());
            bool fAlreadyInFlight = blockInFlightIt != mapBlocksInFlight.end();

            if (pindex->nStatus.hasData()) {
                // Nothing to do here
                return;
            }

            if (pindex->nChainWork <=
                    m_chainman.ActiveChain()
                        .Tip()
                        ->nChainWork || // We know something better
                pindex->nTx != 0) {
                // We had this block at some point, but pruned it
                if (fAlreadyInFlight) {
                    // We requested this block for some reason, but our mempool
                    // will probably be useless so we just grab the block via
                    // normal getdata.
                    std::vector<CInv> vInv(1);
                    vInv[0] = CInv(MSG_BLOCK, cmpctblock.header.GetHash());
                    m_connman.PushMessage(
                        &pfrom, msgMaker.Make(NetMsgType::GETDATA, vInv));
                }
                return;
            }

            // If we're not close to tip yet, give up and let parallel block
            // fetch work its magic.
            if (!fAlreadyInFlight && !CanDirectFetch()) {
                return;
            }

            // We want to be a bit conservative just to be extra careful about
            // DoS possibilities in compact block processing...
            if (pindex->nHeight <= m_chainman.ActiveChain().Height() + 2) {
                if ((!fAlreadyInFlight && nodestate->nBlocksInFlight <
                                              MAX_BLOCKS_IN_TRANSIT_PER_PEER) ||
                    (fAlreadyInFlight &&
                     blockInFlightIt->second.first == pfrom.GetId())) {
                    std::list<QueuedBlock>::iterator *queuedBlockIt = nullptr;
                    if (!BlockRequested(config, pfrom.GetId(), *pindex,
                                        &queuedBlockIt)) {
                        if (!(*queuedBlockIt)->partialBlock) {
                            (*queuedBlockIt)
                                ->partialBlock.reset(
                                    new PartiallyDownloadedBlock(config,
                                                                 &m_mempool));
                        } else {
                            // The block was already in flight using compact
                            // blocks from the same peer.
                            LogPrint(BCLog::NET, "Peer sent us compact block "
                                                 "we were already syncing!\n");
                            return;
                        }
                    }

                    PartiallyDownloadedBlock &partialBlock =
                        *(*queuedBlockIt)->partialBlock;
                    ReadStatus status =
                        partialBlock.InitData(cmpctblock, vExtraTxnForCompact);
                    if (status == READ_STATUS_INVALID) {
                        // Reset in-flight state in case Misbehaving does not
                        // result in a disconnect
                        RemoveBlockRequest(pindex->GetBlockHash());
                        Misbehaving(pfrom, 100, "invalid compact block");
                        return;
                    } else if (status == READ_STATUS_FAILED) {
                        // Duplicate txindices, the block is now in-flight, so
                        // just request it.
                        std::vector<CInv> vInv(1);
                        vInv[0] = CInv(MSG_BLOCK, cmpctblock.header.GetHash());
                        m_connman.PushMessage(
                            &pfrom, msgMaker.Make(NetMsgType::GETDATA, vInv));
                        return;
                    }

                    BlockTransactionsRequest req;
                    for (size_t i = 0; i < cmpctblock.BlockTxCount(); i++) {
                        if (!partialBlock.IsTxAvailable(i)) {
                            req.indices.push_back(i);
                        }
                    }
                    if (req.indices.empty()) {
                        // Dirty hack to jump to BLOCKTXN code (TODO: move
                        // message handling into their own functions)
                        BlockTransactions txn;
                        txn.blockhash = cmpctblock.header.GetHash();
                        blockTxnMsg << txn;
                        fProcessBLOCKTXN = true;
                    } else {
                        req.blockhash = pindex->GetBlockHash();
                        m_connman.PushMessage(
                            &pfrom,
                            msgMaker.Make(NetMsgType::GETBLOCKTXN, req));
                    }
                } else {
                    // This block is either already in flight from a different
                    // peer, or this peer has too many blocks outstanding to
                    // download from. Optimistically try to reconstruct anyway
                    // since we might be able to without any round trips.
                    PartiallyDownloadedBlock tempBlock(config, &m_mempool);
                    ReadStatus status =
                        tempBlock.InitData(cmpctblock, vExtraTxnForCompact);
                    if (status != READ_STATUS_OK) {
                        // TODO: don't ignore failures
                        return;
                    }
                    std::vector<CTransactionRef> dummy;
                    status = tempBlock.FillBlock(*pblock, dummy);
                    if (status == READ_STATUS_OK) {
                        fBlockReconstructed = true;
                    }
                }
            } else {
                if (fAlreadyInFlight) {
                    // We requested this block, but its far into the future, so
                    // our mempool will probably be useless - request the block
                    // normally.
                    std::vector<CInv> vInv(1);
                    vInv[0] = CInv(MSG_BLOCK, cmpctblock.header.GetHash());
                    m_connman.PushMessage(
                        &pfrom, msgMaker.Make(NetMsgType::GETDATA, vInv));
                    return;
                } else {
                    // If this was an announce-cmpctblock, we want the same
                    // treatment as a header message.
                    fRevertToHeaderProcessing = true;
                }
            }
        } // cs_main

        if (fProcessBLOCKTXN) {
            return ProcessMessage(config, pfrom, NetMsgType::BLOCKTXN,
                                  blockTxnMsg, time_received, interruptMsgProc);
        }

        if (fRevertToHeaderProcessing) {
            // Headers received from HB compact block peers are permitted to be
            // relayed before full validation (see BIP 152), so we don't want to
            // disconnect the peer if the header turns out to be for an invalid
            // block. Note that if a peer tries to build on an invalid chain,
            // that will be detected and the peer will be banned.
            return ProcessHeadersMessage(config, pfrom, *peer,
                                         {cmpctblock.header},
                                         /*via_compact_block=*/true);
        }

        if (fBlockReconstructed) {
            // If we got here, we were able to optimistically reconstruct a
            // block that is in flight from some other peer.
            {
                LOCK(cs_main);
                mapBlockSource.emplace(pblock->GetHash(),
                                       std::make_pair(pfrom.GetId(), false));
            }
            // Setting force_processing to true means that we bypass some of
            // our anti-DoS protections in AcceptBlock, which filters
            // unrequested blocks that might be trying to waste our resources
            // (eg disk space). Because we only try to reconstruct blocks when
            // we're close to caught up (via the CanDirectFetch() requirement
            // above, combined with the behavior of not requesting blocks until
            // we have a chain with at least nMinimumChainWork), and we ignore
            // compact blocks with less work than our tip, it is safe to treat
            // reconstructed compact blocks as having been requested.
            ProcessBlock(config, pfrom, pblock, /*force_processing=*/true);
            // hold cs_main for CBlockIndex::IsValid()
            LOCK(cs_main);
            if (pindex->IsValid(BlockValidity::TRANSACTIONS)) {
                // Clear download state for this block, which is in process from
                // some other peer. We do this after calling. ProcessNewBlock so
                // that a malleated cmpctblock announcement can't be used to
                // interfere with block relay.
                RemoveBlockRequest(pblock->GetHash());
            }
        }
        return;
    }

    if (msg_type == NetMsgType::BLOCKTXN) {
        // Ignore blocktxn received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET,
                     "Unexpected blocktxn message received from peer %d\n",
                     pfrom.GetId());
            return;
        }

        BlockTransactions resp;
        vRecv >> resp;

        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        bool fBlockRead = false;
        {
            LOCK(cs_main);

            std::map<BlockHash,
                     std::pair<NodeId, std::list<QueuedBlock>::iterator>>::
                iterator it = mapBlocksInFlight.find(resp.blockhash);
            if (it == mapBlocksInFlight.end() ||
                !it->second.second->partialBlock ||
                it->second.first != pfrom.GetId()) {
                LogPrint(BCLog::NET,
                         "Peer %d sent us block transactions for block "
                         "we weren't expecting\n",
                         pfrom.GetId());
                return;
            }

            PartiallyDownloadedBlock &partialBlock =
                *it->second.second->partialBlock;
            ReadStatus status = partialBlock.FillBlock(*pblock, resp.txn);
            if (status == READ_STATUS_INVALID) {
                // Reset in-flight state in case of Misbehaving does not
                // result in a disconnect.
                RemoveBlockRequest(resp.blockhash);
                Misbehaving(
                    pfrom, 100,
                    "invalid compact block/non-matching block transactions");
                return;
            } else if (status == READ_STATUS_FAILED) {
                // Might have collided, fall back to getdata now :(
                std::vector<CInv> invs;
                invs.push_back(CInv(MSG_BLOCK, resp.blockhash));
                m_connman.PushMessage(&pfrom,
                                      msgMaker.Make(NetMsgType::GETDATA, invs));
            } else {
                // Block is either okay, or possibly we received
                // READ_STATUS_CHECKBLOCK_FAILED.
                // Note that CheckBlock can only fail for one of a few reasons:
                // 1. bad-proof-of-work (impossible here, because we've already
                //    accepted the header)
                // 2. merkleroot doesn't match the transactions given (already
                //    caught in FillBlock with READ_STATUS_FAILED, so
                //    impossible here)
                // 3. the block is otherwise invalid (eg invalid coinbase,
                //    block is too big, too many sigChecks, etc).
                // So if CheckBlock failed, #3 is the only possibility.
                // Under BIP 152, we don't DoS-ban unless proof of work is
                // invalid (we don't require all the stateless checks to have
                // been run). This is handled below, so just treat this as
                // though the block was successfully read, and rely on the
                // handling in ProcessNewBlock to ensure the block index is
                // updated, etc.

                // it is now an empty pointer
                RemoveBlockRequest(resp.blockhash);
                fBlockRead = true;
                // mapBlockSource is used for potentially punishing peers and
                // updating which peers send us compact blocks, so the race
                // between here and cs_main in ProcessNewBlock is fine.
                // BIP 152 permits peers to relay compact blocks after
                // validating the header only; we should not punish peers
                // if the block turns out to be invalid.
                mapBlockSource.emplace(resp.blockhash,
                                       std::make_pair(pfrom.GetId(), false));
            }
        } // Don't hold cs_main when we call into ProcessNewBlock
        if (fBlockRead) {
            // Since we requested this block (it was in mapBlocksInFlight),
            // force it to be processed, even if it would not be a candidate for
            // new tip (missing previous block, chain not long enough, etc)
            // This bypasses some anti-DoS logic in AcceptBlock (eg to prevent
            // disk-space attacks), but this should be safe due to the
            // protections in the compact block handler -- see related comment
            // in compact block optimistic reconstruction handling.
            ProcessBlock(config, pfrom, pblock, /*force_processing=*/true);
        }
        return;
    }

    if (msg_type == NetMsgType::HEADERS) {
        // Ignore headers received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET,
                     "Unexpected headers message received from peer %d\n",
                     pfrom.GetId());
            return;
        }

        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk
        // deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            Misbehaving(pfrom, 20,
                        strprintf("too-many-headers: headers message size = %u",
                                  nCount));
            return;
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            // Ignore tx count; assume it is 0.
            ReadCompactSize(vRecv);
        }

        return ProcessHeadersMessage(config, pfrom, *peer, headers,
                                     /*via_compact_block=*/false);
    }

    if (msg_type == NetMsgType::BLOCK) {
        // Ignore block received while importing
        if (fImporting || fReindex) {
            LogPrint(BCLog::NET,
                     "Unexpected block message received from peer %d\n",
                     pfrom.GetId());
            return;
        }

        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        vRecv >> *pblock;

        LogPrint(BCLog::NET, "received block %s peer=%d\n",
                 pblock->GetHash().ToString(), pfrom.GetId());

        // Process all blocks from whitelisted peers, even if not requested,
        // unless we're still syncing with the network. Such an unrequested
        // block may still be processed, subject to the conditions in
        // AcceptBlock().
        bool forceProcessing =
            pfrom.HasPermission(NetPermissionFlags::NoBan) &&
            !m_chainman.ActiveChainstate().IsInitialBlockDownload();
        const BlockHash hash = pblock->GetHash();
        {
            LOCK(cs_main);
            // Always process the block if we requested it, since we may
            // need it even when it's not a candidate for a new best tip.
            forceProcessing = IsBlockRequested(hash);
            RemoveBlockRequest(hash);
            // mapBlockSource is only used for punishing peers and setting
            // which peers send us compact blocks, so the race between here and
            // cs_main in ProcessNewBlock is fine.
            mapBlockSource.emplace(hash, std::make_pair(pfrom.GetId(), true));
        }
        ProcessBlock(config, pfrom, pblock, forceProcessing);
        return;
    }

    if (msg_type == NetMsgType::AVAHELLO) {
        {
            LOCK(pfrom.cs_avalanche_pubkey);
            if (pfrom.m_avalanche_pubkey.has_value()) {
                LogPrint(
                    BCLog::AVALANCHE,
                    "Ignoring avahello from peer %d: already in our node set\n",
                    pfrom.GetId());
                return;
            }

            avalanche::Delegation delegation;
            vRecv >> delegation;

            // A delegation with an all zero limited id indicates that the peer
            // has no proof, so we're done.
            if (delegation.getLimitedProofId() != uint256::ZERO) {
                avalanche::DelegationState state;
                CPubKey pubkey;
                if (!delegation.verify(state, pubkey)) {
                    Misbehaving(pfrom, 100, "invalid-delegation");
                    return;
                }
                pfrom.m_avalanche_pubkey = std::move(pubkey);

                CHashWriter sighasher(SER_GETHASH, 0);
                sighasher << delegation.getId();
                sighasher << pfrom.nRemoteHostNonce;
                sighasher << pfrom.GetLocalNonce();
                sighasher << pfrom.nRemoteExtraEntropy;
                sighasher << pfrom.GetLocalExtraEntropy();

                SchnorrSig sig;
                vRecv >> sig;
                if (!(*pfrom.m_avalanche_pubkey)
                         .VerifySchnorr(sighasher.GetHash(), sig)) {
                    Misbehaving(pfrom, 100, "invalid-avahello-signature");
                    return;
                }

                // If we don't know this proof already, add it to the tracker so
                // it can be requested.
                const avalanche::ProofId proofid(delegation.getProofId());
                if (!AlreadyHaveProof(proofid)) {
                    const bool preferred = isPreferredDownloadPeer(pfrom);
                    LOCK(cs_proofrequest);
                    AddProofAnnouncement(pfrom, proofid,
                                         GetTime<std::chrono::microseconds>(),
                                         preferred);
                }

                // Don't check the return value. If it fails we probably don't
                // know about the proof yet.
                g_avalanche->withPeerManager([&](avalanche::PeerManager &pm) {
                    return pm.addNode(pfrom.GetId(), proofid);
                });
            }

            pfrom.m_avalanche_enabled = true;
        }

        // Send getavaaddr and getavaproofs to our avalanche outbound or
        // manual connections
        if (!pfrom.IsInboundConn()) {
            m_connman.PushMessage(&pfrom,
                                  msgMaker.Make(NetMsgType::GETAVAADDR));
            WITH_LOCK(peer->m_addr_token_bucket_mutex,
                      peer->m_addr_token_bucket += GetMaxAddrToSend());

            if (pfrom.m_proof_relay &&
                !m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
                m_connman.PushMessage(&pfrom,
                                      msgMaker.Make(NetMsgType::GETAVAPROOFS));
                pfrom.m_proof_relay->compactproofs_requested = true;
            }
        }

        return;
    }

    if (msg_type == NetMsgType::AVAPOLL) {
        auto now = std::chrono::steady_clock::now();
        int64_t cooldown =
            gArgs.GetIntArg("-avacooldown", AVALANCHE_DEFAULT_COOLDOWN);

        {
            LOCK(cs_main);
            auto &node_state = State(pfrom.GetId())->m_avalanche_state;

            if (now <
                node_state.last_poll + std::chrono::milliseconds(cooldown)) {
                Misbehaving(pfrom, 20, "avapool-cooldown");
            }

            node_state.last_poll = now;
        }

        const bool quorum_established =
            g_avalanche && g_avalanche->isQuorumEstablished();

        uint64_t round;
        Unserialize(vRecv, round);

        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > AVALANCHE_MAX_ELEMENT_POLL) {
            Misbehaving(
                pfrom, 20,
                strprintf("too-many-ava-poll: poll message size = %u", nCount));
            return;
        }

        std::vector<avalanche::Vote> votes;
        votes.reserve(nCount);

        for (unsigned int n = 0; n < nCount; n++) {
            CInv inv;
            vRecv >> inv;

            // Default vote for unknown inv type
            uint32_t vote = -1;

            // We don't vote definitively until we have an established quorum
            if (!quorum_established) {
                votes.emplace_back(vote, inv.hash);
                continue;
            }

            // If inv's type is known, get a vote for its hash
            switch (inv.type) {
                case MSG_TX: {
                    if (gArgs.GetBoolArg("-avalanchepreconsensus", false)) {
                        vote = WITH_LOCK(cs_main, return GetAvalancheVoteForTx(
                                                      TxId(inv.hash)));
                    }
                } break;
                case MSG_BLOCK: {
                    vote = WITH_LOCK(cs_main, return GetAvalancheVoteForBlock(
                                                  BlockHash(inv.hash)));
                } break;
                case MSG_AVA_PROOF: {
                    vote =
                        getAvalancheVoteForProof(avalanche::ProofId(inv.hash));
                } break;
                default: {
                    LogPrint(BCLog::AVALANCHE,
                             "poll inv type unknown from peer=%d\n", inv.type);
                }
            }

            votes.emplace_back(vote, inv.hash);
        }

        // Send the query to the node.
        g_avalanche->sendResponse(
            &pfrom, avalanche::Response(round, cooldown, std::move(votes)));
        return;
    }

    if (msg_type == NetMsgType::AVARESPONSE) {
        // As long as QUIC is not implemented, we need to sign response and
        // verify response's signatures in order to avoid any manipulation of
        // messages at the transport level.
        CHashVerifier<CDataStream> verifier(&vRecv);
        avalanche::Response response;
        verifier >> response;

        SchnorrSig sig;
        vRecv >> sig;

        {
            LOCK(pfrom.cs_avalanche_pubkey);
            if (!pfrom.m_avalanche_pubkey.has_value() ||
                !(*pfrom.m_avalanche_pubkey)
                     .VerifySchnorr(verifier.GetHash(), sig)) {
                Misbehaving(pfrom, 100, "invalid-ava-response-signature");
                return;
            }
        }

        std::vector<avalanche::VoteItemUpdate> updates;
        int banscore;
        std::string error;
        if (!g_avalanche->registerVotes(pfrom.GetId(), response, updates,
                                        banscore, error)) {
            Misbehaving(pfrom, banscore, error);
            return;
        }

        pfrom.invsVoted(response.GetVotes().size());

        auto logVoteUpdate = [](const auto &voteUpdate,
                                const std::string &voteItemTypeStr,
                                const auto &voteItemId) {
            std::string voteOutcome;
            switch (voteUpdate.getStatus()) {
                case avalanche::VoteStatus::Invalid:
                    voteOutcome = "invalidated";
                    break;
                case avalanche::VoteStatus::Rejected:
                    voteOutcome = "rejected";
                    break;
                case avalanche::VoteStatus::Accepted:
                    voteOutcome = "accepted";
                    break;
                case avalanche::VoteStatus::Finalized:
                    voteOutcome = "finalized";
                    break;
                case avalanche::VoteStatus::Stale:
                    voteOutcome = "stalled";
                    break;

                    // No default case, so the compiler can warn about missing
                    // cases
            }

            LogPrint(BCLog::AVALANCHE, "Avalanche %s %s %s\n", voteOutcome,
                     voteItemTypeStr, voteItemId.ToString());
        };

        bool shouldActivateBestChain = false;

        for (const auto &u : updates) {
            const avalanche::AnyVoteItem &item = u.getVoteItem();

            // Don't use a visitor here as we want to ignore unsupported item
            // types. This comes in handy when adding new types.
            if (auto pitem = std::get_if<const avalanche::ProofRef>(&item)) {
                avalanche::ProofRef proof = *pitem;
                const avalanche::ProofId &proofid = proof->getId();

                logVoteUpdate(u, "proof", proofid);

                auto rejectionMode =
                    avalanche::PeerManager::RejectionMode::DEFAULT;
                auto nextCooldownTimePoint = GetTime<std::chrono::seconds>();
                switch (u.getStatus()) {
                    case avalanche::VoteStatus::Invalid:
                        WITH_LOCK(cs_invalidProofs,
                                  invalidProofs->insert(proofid));
                        // Fallthrough
                    case avalanche::VoteStatus::Stale:
                        // Invalidate mode removes the proof from all proof
                        // pools
                        rejectionMode =
                            avalanche::PeerManager::RejectionMode::INVALIDATE;
                        // Fallthrough
                    case avalanche::VoteStatus::Rejected:
                        if (!g_avalanche->withPeerManager(
                                [&](avalanche::PeerManager &pm) {
                                    return pm.rejectProof(proofid,
                                                          rejectionMode);
                                })) {
                            LogPrint(BCLog::AVALANCHE,
                                     "ERROR: Failed to reject proof: %s\n",
                                     proofid.GetHex());
                        }
                        break;
                    case avalanche::VoteStatus::Finalized:
                        nextCooldownTimePoint +=
                            std::chrono::seconds(gArgs.GetIntArg(
                                "-avalanchepeerreplacementcooldown",
                                AVALANCHE_DEFAULT_PEER_REPLACEMENT_COOLDOWN));
                    case avalanche::VoteStatus::Accepted:
                        if (!g_avalanche->withPeerManager(
                                [&](avalanche::PeerManager &pm) {
                                    pm.registerProof(
                                        proof,
                                        avalanche::PeerManager::
                                            RegistrationMode::FORCE_ACCEPT);
                                    return pm.forPeer(
                                        proofid,
                                        [&](const avalanche::Peer &peer) {
                                            pm.updateNextPossibleConflictTime(
                                                peer.peerid,
                                                nextCooldownTimePoint);
                                            if (u.getStatus() ==
                                                avalanche::VoteStatus::
                                                    Finalized) {
                                                pm.setFinalized(peer.peerid);
                                            }
                                            // Only fail if the peer was not
                                            // created
                                            return true;
                                        });
                                })) {
                            LogPrint(BCLog::AVALANCHE,
                                     "ERROR: Failed to accept proof: %s\n",
                                     proofid.GetHex());
                        }
                        break;
                }
            }

            if (auto pitem = std::get_if<const CBlockIndex *>(&item)) {
                CBlockIndex *pindex = const_cast<CBlockIndex *>(*pitem);

                shouldActivateBestChain = true;

                logVoteUpdate(u, "block", pindex->GetBlockHash());

                switch (u.getStatus()) {
                    case avalanche::VoteStatus::Invalid:
                    case avalanche::VoteStatus::Rejected: {
                        BlockValidationState state;
                        m_chainman.ActiveChainstate().ParkBlock(config, state,
                                                                pindex);
                        if (!state.IsValid()) {
                            LogPrintf("ERROR: Database error: %s\n",
                                      state.GetRejectReason());
                            return;
                        }
                    } break;
                    case avalanche::VoteStatus::Accepted: {
                        LOCK(cs_main);
                        m_chainman.ActiveChainstate().UnparkBlock(pindex);
                    } break;
                    case avalanche::VoteStatus::Finalized: {
                        {
                            LOCK(cs_main);
                            m_chainman.ActiveChainstate().UnparkBlock(pindex);
                        }
                        m_chainman.ActiveChainstate().AvalancheFinalizeBlock(
                            pindex);
                    } break;
                    case avalanche::VoteStatus::Stale:
                        // Fall back on Nakamoto consensus in the absence of
                        // Avalanche votes for other competing or descendant
                        // blocks.
                        break;
                }
            }
        }

        if (shouldActivateBestChain) {
            BlockValidationState state;
            if (!m_chainman.ActiveChainstate().ActivateBestChain(config,
                                                                 state)) {
                LogPrintf("failed to activate chain (%s)\n", state.ToString());
            }
        }

        return;
    }

    if (msg_type == NetMsgType::AVAPROOF) {
        auto proof = RCUPtr<avalanche::Proof>::make();
        vRecv >> *proof;

        ReceivedAvalancheProof(pfrom, proof);

        return;
    }

    if (msg_type == NetMsgType::GETAVAPROOFS) {
        if (pfrom.m_proof_relay == nullptr) {
            return;
        }

        pfrom.m_proof_relay->lastSharedProofsUpdate =
            GetTime<std::chrono::seconds>();

        pfrom.m_proof_relay->sharedProofs =
            g_avalanche->withPeerManager([&](const avalanche::PeerManager &pm) {
                return pm.getShareableProofsSnapshot();
            });

        avalanche::CompactProofs compactProofs(
            pfrom.m_proof_relay->sharedProofs);
        m_connman.PushMessage(
            &pfrom, msgMaker.Make(NetMsgType::AVAPROOFS, compactProofs));

        return;
    }

    if (msg_type == NetMsgType::AVAPROOFS) {
        if (pfrom.m_proof_relay == nullptr) {
            return;
        }

        // Only process the compact proofs if we requested them
        if (!pfrom.m_proof_relay->compactproofs_requested) {
            LogPrint(BCLog::AVALANCHE, "Ignoring unsollicited avaproofs\n");
            return;
        }
        pfrom.m_proof_relay->compactproofs_requested = false;

        avalanche::CompactProofs compactProofs;
        try {
            vRecv >> compactProofs;
        } catch (std::ios_base::failure &e) {
            // This compact proofs have non contiguous or overflowing indexes
            Misbehaving(pfrom, 100, "avaproofs-bad-indexes");
            return;
        }

        // If there are prefilled proofs, process them first
        std::set<uint32_t> prefilledIndexes;
        for (const auto &prefilledProof : compactProofs.getPrefilledProofs()) {
            if (!ReceivedAvalancheProof(pfrom, prefilledProof.proof)) {
                // If we got an invalid proof, the peer is getting banned and we
                // can bail out.
                return;
            }
        }

        // If there is no shortid, avoid parsing/responding/accounting for the
        // message.
        if (compactProofs.getShortIDs().size() == 0) {
            LogPrint(BCLog::AVALANCHE,
                     "Got an avaproofs message with no shortid (peer %d)\n",
                     pfrom.GetId());
            return;
        }

        // To determine the chance that the number of entries in a bucket
        // exceeds N, we use the fact that the number of elements in a single
        // bucket is binomially distributed (with n = the number of shorttxids
        // S, and p = 1 / the number of buckets), that in the worst case the
        // number of buckets is equal to S (due to std::unordered_map having a
        // default load factor of 1.0), and that the chance for any bucket to
        // exceed N elements is at most buckets * (the chance that any given
        // bucket is above N elements). Thus:
        //   P(max_elements_per_bucket > N) <=
        //     S * (1 - cdf(binomial(n=S,p=1/S), N))
        // If we assume up to 21000000, allowing 15 elements per bucket should
        // only fail once per ~2.5 million avaproofs transfers (per peer and
        // connection).
        // TODO re-evaluate the bucket count to a more realistic value.
        // TODO: In the case of a shortid-collision, we should request all the
        // proofs which collided. For now, we only request one, which is not
        // that bad considering this event is expected to be very rare.
        auto shortIdProcessor =
            avalanche::ProofShortIdProcessor(compactProofs.getPrefilledProofs(),
                                             compactProofs.getShortIDs(), 15);

        if (shortIdProcessor.hasOutOfBoundIndex()) {
            // This should be catched by deserialization, but catch it here as
            // well as a good measure.
            Misbehaving(pfrom, 100, "avaproofs-bad-indexes");
            return;
        }
        if (!shortIdProcessor.isEvenlyDistributed()) {
            // This is suspicious, don't ban but bail out
            return;
        }

        const auto &proofs =
            g_avalanche->withPeerManager([&](const avalanche::PeerManager &pm) {
                return pm.getShareableProofsSnapshot();
            });

        size_t proofCount = 0;
        proofs.forEachLeaf([&](const avalanche::ProofRef &proof) {
            uint64_t shortid = compactProofs.getShortID(proof->getId());

            proofCount += shortIdProcessor.matchKnownItem(shortid, proof);

            // Though ideally we'd continue scanning for the
            // two-proofs-match-shortid case, the performance win of an early
            // exit here is too good to pass up and worth the extra risk.
            return proofCount != shortIdProcessor.getShortIdCount();
        });

        avalanche::ProofsRequest req;
        for (size_t i = 0; i < compactProofs.size(); i++) {
            if (shortIdProcessor.getItem(i) == nullptr) {
                req.indices.push_back(i);
            }
        }

        m_connman.PushMessage(&pfrom,
                              msgMaker.Make(NetMsgType::AVAPROOFSREQ, req));

        // We want to keep a count of how many nodes we successfully requested
        // avaproofs from as this is used to determine when we are confident our
        // quorum is close enough to the other participants.
        g_avalanche->avaproofsSent(pfrom.GetId());

        return;
    }

    if (msg_type == NetMsgType::AVAPROOFSREQ) {
        if (pfrom.m_proof_relay == nullptr) {
            return;
        }

        avalanche::ProofsRequest proofreq;
        vRecv >> proofreq;

        auto requestedIndiceIt = proofreq.indices.begin();
        uint32_t treeIndice = 0;
        pfrom.m_proof_relay->sharedProofs.forEachLeaf([&](const auto &proof) {
            if (requestedIndiceIt == proofreq.indices.end()) {
                // No more indice to process
                return false;
            }

            if (treeIndice++ == *requestedIndiceIt) {
                m_connman.PushMessage(
                    &pfrom, msgMaker.Make(NetMsgType::AVAPROOF, *proof));
                requestedIndiceIt++;
            }

            return true;
        });

        pfrom.m_proof_relay->sharedProofs = {};
        return;
    }

    if (msg_type == NetMsgType::GETADDR) {
        // This asymmetric behavior for inbound and outbound connections was
        // introduced to prevent a fingerprinting attack: an attacker can send
        // specific fake addresses to users' AddrMan and later request them by
        // sending getaddr messages. Making nodes which are behind NAT and can
        // only make outgoing connections ignore the getaddr message mitigates
        // the attack.
        if (!pfrom.IsInboundConn()) {
            LogPrint(BCLog::NET,
                     "Ignoring \"getaddr\" from %s connection. peer=%d\n",
                     pfrom.ConnectionTypeAsString(), pfrom.GetId());
            return;
        }

        // Since this must be an inbound connection, SetupAddressRelay will
        // never fail.
        Assume(SetupAddressRelay(pfrom, *peer));

        // Only send one GetAddr response per connection to reduce resource
        // waste and discourage addr stamping of INV announcements.
        if (peer->m_getaddr_recvd) {
            LogPrint(BCLog::NET, "Ignoring repeated \"getaddr\". peer=%d\n",
                     pfrom.GetId());
            return;
        }
        peer->m_getaddr_recvd = true;

        peer->m_addrs_to_send.clear();
        std::vector<CAddress> vAddr;
        const size_t maxAddrToSend = GetMaxAddrToSend();
        if (pfrom.HasPermission(NetPermissionFlags::Addr)) {
            vAddr = m_connman.GetAddresses(maxAddrToSend, MAX_PCT_ADDR_TO_SEND,
                                           /* network */ std::nullopt);
        } else {
            vAddr = m_connman.GetAddresses(pfrom, maxAddrToSend,
                                           MAX_PCT_ADDR_TO_SEND);
        }
        FastRandomContext insecure_rand;
        for (const CAddress &addr : vAddr) {
            PushAddress(*peer, addr, insecure_rand);
        }
        return;
    }

    if (msg_type == NetMsgType::GETAVAADDR) {
        auto now = GetTime<std::chrono::seconds>();
        if (now < pfrom.m_nextGetAvaAddr) {
            // Prevent a peer from exhausting our resources by spamming
            // getavaaddr messages.
            LogPrint(BCLog::AVALANCHE,
                     "Ignoring repeated getavaaddr from peer %d\n",
                     pfrom.GetId());
            return;
        }

        // Only accept a getavaaddr every GETAVAADDR_INTERVAL at most
        pfrom.m_nextGetAvaAddr = now + GETAVAADDR_INTERVAL;

        if (!SetupAddressRelay(pfrom, *peer)) {
            LogPrint(BCLog::AVALANCHE,
                     "Ignoring getavaaddr message from %s peer=%d\n",
                     pfrom.ConnectionTypeAsString(), pfrom.GetId());
            return;
        }

        auto availabilityScoreComparator = [](const CNode *lhs,
                                              const CNode *rhs) {
            double scoreLhs = lhs->getAvailabilityScore();
            double scoreRhs = rhs->getAvailabilityScore();

            if (scoreLhs != scoreRhs) {
                return scoreLhs > scoreRhs;
            }

            return lhs < rhs;
        };

        // Get up to MAX_ADDR_TO_SEND addresses of the nodes which are the
        // most active in the avalanche network. Account for 0 availability as
        // well so we can send addresses even if we did not start polling yet.
        std::set<const CNode *, decltype(availabilityScoreComparator)> avaNodes(
            availabilityScoreComparator);
        m_connman.ForEachNode([&](const CNode *pnode) {
            if (!pnode->m_avalanche_enabled ||
                pnode->getAvailabilityScore() < 0.) {
                return;
            }

            avaNodes.insert(pnode);
            if (avaNodes.size() > GetMaxAddrToSend()) {
                avaNodes.erase(std::prev(avaNodes.end()));
            }
        });

        peer->m_addrs_to_send.clear();
        FastRandomContext insecure_rand;
        for (const CNode *pnode : avaNodes) {
            PushAddress(*peer, pnode->addr, insecure_rand);
        }

        return;
    }

    if (msg_type == NetMsgType::MEMPOOL) {
        if (!(pfrom.GetLocalServices() & NODE_BLOOM) &&
            !pfrom.HasPermission(NetPermissionFlags::Mempool)) {
            if (!pfrom.HasPermission(NetPermissionFlags::NoBan)) {
                LogPrint(BCLog::NET,
                         "mempool request with bloom filters disabled, "
                         "disconnect peer=%d\n",
                         pfrom.GetId());
                pfrom.fDisconnect = true;
            }
            return;
        }

        if (m_connman.OutboundTargetReached(false) &&
            !pfrom.HasPermission(NetPermissionFlags::Mempool)) {
            if (!pfrom.HasPermission(NetPermissionFlags::NoBan)) {
                LogPrint(BCLog::NET,
                         "mempool request with bandwidth limit reached, "
                         "disconnect peer=%d\n",
                         pfrom.GetId());
                pfrom.fDisconnect = true;
            }
            return;
        }

        if (pfrom.m_tx_relay != nullptr) {
            LOCK(pfrom.m_tx_relay->cs_tx_inventory);
            pfrom.m_tx_relay->fSendMempool = true;
        }
        return;
    }

    if (msg_type == NetMsgType::PING) {
        if (pfrom.GetCommonVersion() > BIP0031_VERSION) {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful
            // features:
            //
            // 1) A remote node can quickly check if the connection is
            // operational.
            // 2) Remote nodes can measure the latency of the network thread. If
            // this node is overloaded it won't respond to pings quickly and the
            // remote node can avoid sending us more work, like chain download
            // requests.
            //
            // The nonce stops the remote getting confused between different
            // pings: without it, if the remote node sends a ping once per
            // second and this node takes 5 seconds to respond to each, the 5th
            // ping the remote sends would appear to return very quickly.
            m_connman.PushMessage(&pfrom,
                                  msgMaker.Make(NetMsgType::PONG, nonce));
        }
        return;
    }

    if (msg_type == NetMsgType::PONG) {
        const auto ping_end = time_received;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old
            // ping without nonce should never pong)
            if (peer->m_ping_nonce_sent != 0) {
                if (nonce == peer->m_ping_nonce_sent) {
                    // Matching pong received, this ping is no longer
                    // outstanding
                    bPingFinished = true;
                    const auto ping_time = ping_end - peer->m_ping_start.load();
                    if (ping_time.count() >= 0) {
                        // Let connman know about this successful ping-pong
                        pfrom.PongReceived(ping_time);
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation
                        // somewhere; cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere;
            // cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint(BCLog::NET,
                     "pong peer=%d: %s, %x expected, %x received, %u bytes\n",
                     pfrom.GetId(), sProblem, peer->m_ping_nonce_sent, nonce,
                     nAvail);
        }
        if (bPingFinished) {
            peer->m_ping_nonce_sent = 0;
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERLOAD) {
        if (!(pfrom.GetLocalServices() & NODE_BLOOM)) {
            LogPrint(BCLog::NET,
                     "filterload received despite not offering bloom services "
                     "from peer=%d; disconnecting\n",
                     pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        CBloomFilter filter;
        vRecv >> filter;

        if (!filter.IsWithinSizeConstraints()) {
            // There is no excuse for sending a too-large filter
            Misbehaving(pfrom, 100, "too-large bloom filter");
        } else if (pfrom.m_tx_relay != nullptr) {
            LOCK(pfrom.m_tx_relay->cs_filter);
            pfrom.m_tx_relay->pfilter.reset(new CBloomFilter(filter));
            pfrom.m_tx_relay->fRelayTxes = true;
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERADD) {
        if (!(pfrom.GetLocalServices() & NODE_BLOOM)) {
            LogPrint(BCLog::NET,
                     "filteradd received despite not offering bloom services "
                     "from peer=%d; disconnecting\n",
                     pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        std::vector<uint8_t> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a
        // script data object, and thus, the maximum size any matched object can
        // have) in a filteradd message.
        bool bad = false;
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            bad = true;
        } else if (pfrom.m_tx_relay != nullptr) {
            LOCK(pfrom.m_tx_relay->cs_filter);
            if (pfrom.m_tx_relay->pfilter) {
                pfrom.m_tx_relay->pfilter->insert(vData);
            } else {
                bad = true;
            }
        }
        if (bad) {
            // The structure of this code doesn't really allow for a good error
            // code. We'll go generic.
            Misbehaving(pfrom, 100, "bad filteradd message");
        }
        return;
    }

    if (msg_type == NetMsgType::FILTERCLEAR) {
        if (!(pfrom.GetLocalServices() & NODE_BLOOM)) {
            LogPrint(BCLog::NET,
                     "filterclear received despite not offering bloom services "
                     "from peer=%d; disconnecting\n",
                     pfrom.GetId());
            pfrom.fDisconnect = true;
            return;
        }
        if (pfrom.m_tx_relay == nullptr) {
            return;
        }
        LOCK(pfrom.m_tx_relay->cs_filter);
        pfrom.m_tx_relay->pfilter = nullptr;
        pfrom.m_tx_relay->fRelayTxes = true;
        return;
    }

    if (msg_type == NetMsgType::FEEFILTER) {
        Amount newFeeFilter = Amount::zero();
        vRecv >> newFeeFilter;
        if (MoneyRange(newFeeFilter)) {
            if (pfrom.m_tx_relay != nullptr) {
                pfrom.m_tx_relay->minFeeFilter = newFeeFilter;
            }
            LogPrint(BCLog::NET, "received: feefilter of %s from peer=%d\n",
                     CFeeRate(newFeeFilter).ToString(), pfrom.GetId());
        }
        return;
    }

    if (msg_type == NetMsgType::GETCFILTERS) {
        ProcessGetCFilters(pfrom, vRecv);
        return;
    }

    if (msg_type == NetMsgType::GETCFHEADERS) {
        ProcessGetCFHeaders(pfrom, vRecv);
        return;
    }

    if (msg_type == NetMsgType::GETCFCHECKPT) {
        ProcessGetCFCheckPt(pfrom, vRecv);
        return;
    }

    if (msg_type == NetMsgType::NOTFOUND) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        // A peer might send up to 1 notfound per getdata request, but no more
        if (vInv.size() <= PROOF_REQUEST_PARAMS.max_peer_announcements +
                               TX_REQUEST_PARAMS.max_peer_announcements +
                               MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            for (CInv &inv : vInv) {
                if (inv.IsMsgTx()) {
                    // If we receive a NOTFOUND message for a tx we requested,
                    // mark the announcement for it as completed in
                    // InvRequestTracker.
                    LOCK(::cs_main);
                    m_txrequest.ReceivedResponse(pfrom.GetId(), TxId(inv.hash));
                    continue;
                }
                if (inv.IsMsgProof()) {
                    LOCK(cs_proofrequest);
                    m_proofrequest.ReceivedResponse(
                        pfrom.GetId(), avalanche::ProofId(inv.hash));
                }
            }
        }
        return;
    }

    // Ignore unknown commands for extensibility
    LogPrint(BCLog::NET, "Unknown command \"%s\" from peer=%d\n",
             SanitizeString(msg_type), pfrom.GetId());
    return;
}

bool PeerManagerImpl::MaybeDiscourageAndDisconnect(CNode &pnode, Peer &peer) {
    {
        LOCK(peer.m_misbehavior_mutex);

        // There's nothing to do if the m_should_discourage flag isn't set
        if (!peer.m_should_discourage) {
            return false;
        }

        peer.m_should_discourage = false;
    } // peer.m_misbehavior_mutex

    if (pnode.HasPermission(NetPermissionFlags::NoBan)) {
        // We never disconnect or discourage peers for bad behavior if they have
        // NetPermissionFlags::NoBan permission
        LogPrintf("Warning: not punishing noban peer %d!\n", peer.m_id);
        return false;
    }

    if (pnode.IsManualConn()) {
        // We never disconnect or discourage manual peers for bad behavior
        LogPrintf("Warning: not punishing manually connected peer %d!\n",
                  peer.m_id);
        return false;
    }

    if (pnode.addr.IsLocal()) {
        // We disconnect local peers for bad behavior but don't discourage
        // (since that would discourage all peers on the same local address)
        LogPrint(BCLog::NET,
                 "Warning: disconnecting but not discouraging %s peer %d!\n",
                 pnode.m_inbound_onion ? "inbound onion" : "local", peer.m_id);
        pnode.fDisconnect = true;
        return true;
    }

    // Normal case: Disconnect the peer and discourage all nodes sharing the
    // address
    LogPrint(BCLog::NET, "Disconnecting and discouraging peer %d!\n",
             peer.m_id);
    if (m_banman) {
        m_banman->Discourage(pnode.addr);
    }
    m_connman.DisconnectNode(pnode.addr);
    return true;
}

bool PeerManagerImpl::ProcessMessages(const Config &config, CNode *pfrom,
                                      std::atomic<bool> &interruptMsgProc) {
    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fMoreWork = false;

    PeerRef peer = GetPeerRef(pfrom->GetId());
    if (peer == nullptr) {
        return false;
    }

    {
        LOCK(peer->m_getdata_requests_mutex);
        if (!peer->m_getdata_requests.empty()) {
            ProcessGetData(config, *pfrom, *peer, interruptMsgProc);
        }
    }

    {
        LOCK2(cs_main, g_cs_orphans);
        if (!peer->m_orphan_work_set.empty()) {
            ProcessOrphanTx(config, peer->m_orphan_work_set);
        }
    }

    if (pfrom->fDisconnect) {
        return false;
    }

    // this maintains the order of responses and prevents m_getdata_requests
    // from growing unbounded
    {
        LOCK(peer->m_getdata_requests_mutex);
        if (!peer->m_getdata_requests.empty()) {
            return true;
        }
    }

    {
        LOCK(g_cs_orphans);
        if (!peer->m_orphan_work_set.empty()) {
            return true;
        }
    }

    // Don't bother if send buffer is too full to respond anyway
    if (pfrom->fPauseSend) {
        return false;
    }

    std::list<CNetMessage> msgs;
    {
        LOCK(pfrom->cs_vProcessMsg);
        if (pfrom->vProcessMsg.empty()) {
            return false;
        }
        // Just take one message
        msgs.splice(msgs.begin(), pfrom->vProcessMsg,
                    pfrom->vProcessMsg.begin());
        pfrom->nProcessQueueSize -= msgs.front().m_raw_message_size;
        pfrom->fPauseRecv =
            pfrom->nProcessQueueSize > m_connman.GetReceiveFloodSize();
        fMoreWork = !pfrom->vProcessMsg.empty();
    }
    CNetMessage &msg(msgs.front());

    TRACE6(net, inbound_message, pfrom->GetId(), pfrom->m_addr_name.c_str(),
           pfrom->ConnectionTypeAsString().c_str(), msg.m_command.c_str(),
           msg.m_recv.size(), msg.m_recv.data());

    if (gArgs.GetBoolArg("-capturemessages", false)) {
        CaptureMessage(pfrom->addr, msg.m_command, MakeUCharSpan(msg.m_recv),
                       /*is_incoming=*/true);
    }

    msg.SetVersion(pfrom->GetCommonVersion());

    // Check network magic
    if (!msg.m_valid_netmagic) {
        LogPrint(BCLog::NET,
                 "PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n",
                 SanitizeString(msg.m_command), pfrom->GetId());

        // Make sure we discourage where that come from for some time.
        if (m_banman) {
            m_banman->Discourage(pfrom->addr);
        }
        m_connman.DisconnectNode(pfrom->addr);

        pfrom->fDisconnect = true;
        return false;
    }

    // Check header
    if (!msg.m_valid_header) {
        LogPrint(BCLog::NET, "PROCESSMESSAGE: ERRORS IN HEADER %s peer=%d\n",
                 SanitizeString(msg.m_command), pfrom->GetId());
        return fMoreWork;
    }
    const std::string &msg_type = msg.m_command;

    // Message size
    unsigned int nMessageSize = msg.m_message_size;

    // Checksum
    CDataStream &vRecv = msg.m_recv;
    if (!msg.m_valid_checksum) {
        LogPrint(BCLog::NET, "%s(%s, %u bytes): CHECKSUM ERROR peer=%d\n",
                 __func__, SanitizeString(msg_type), nMessageSize,
                 pfrom->GetId());
        if (m_banman) {
            m_banman->Discourage(pfrom->addr);
        }
        m_connman.DisconnectNode(pfrom->addr);
        return fMoreWork;
    }

    try {
        ProcessMessage(config, *pfrom, msg_type, vRecv, msg.m_time,
                       interruptMsgProc);
        if (interruptMsgProc) {
            return false;
        }

        {
            LOCK(peer->m_getdata_requests_mutex);
            if (!peer->m_getdata_requests.empty()) {
                fMoreWork = true;
            }
        }
    } catch (const std::exception &e) {
        LogPrint(BCLog::NET, "%s(%s, %u bytes): Exception '%s' (%s) caught\n",
                 __func__, SanitizeString(msg_type), nMessageSize, e.what(),
                 typeid(e).name());
    } catch (...) {
        LogPrint(BCLog::NET, "%s(%s, %u bytes): Unknown exception caught\n",
                 __func__, SanitizeString(msg_type), nMessageSize);
    }

    return fMoreWork;
}

void PeerManagerImpl::ConsiderEviction(CNode &pto, int64_t time_in_seconds) {
    AssertLockHeld(cs_main);

    CNodeState &state = *State(pto.GetId());
    const CNetMsgMaker msgMaker(pto.GetCommonVersion());

    if (!state.m_chain_sync.m_protect && pto.IsOutboundOrBlockRelayConn() &&
        state.fSyncStarted) {
        // This is an outbound peer subject to disconnection if they don't
        // announce a block with as much work as the current tip within
        // CHAIN_SYNC_TIMEOUT + HEADERS_RESPONSE_TIME seconds (note: if their
        // chain has more work than ours, we should sync to it, unless it's
        // invalid, in which case we should find that out and disconnect from
        // them elsewhere).
        if (state.pindexBestKnownBlock != nullptr &&
            state.pindexBestKnownBlock->nChainWork >=
                m_chainman.ActiveChain().Tip()->nChainWork) {
            if (state.m_chain_sync.m_timeout != 0) {
                state.m_chain_sync.m_timeout = 0;
                state.m_chain_sync.m_work_header = nullptr;
                state.m_chain_sync.m_sent_getheaders = false;
            }
        } else if (state.m_chain_sync.m_timeout == 0 ||
                   (state.m_chain_sync.m_work_header != nullptr &&
                    state.pindexBestKnownBlock != nullptr &&
                    state.pindexBestKnownBlock->nChainWork >=
                        state.m_chain_sync.m_work_header->nChainWork)) {
            // Our best block known by this peer is behind our tip, and we're
            // either noticing that for the first time, OR this peer was able to
            // catch up to some earlier point where we checked against our tip.
            // Either way, set a new timeout based on current tip.
            state.m_chain_sync.m_timeout = time_in_seconds + CHAIN_SYNC_TIMEOUT;
            state.m_chain_sync.m_work_header = m_chainman.ActiveChain().Tip();
            state.m_chain_sync.m_sent_getheaders = false;
        } else if (state.m_chain_sync.m_timeout > 0 &&
                   time_in_seconds > state.m_chain_sync.m_timeout) {
            // No evidence yet that our peer has synced to a chain with work
            // equal to that of our tip, when we first detected it was behind.
            // Send a single getheaders message to give the peer a chance to
            // update us.
            if (state.m_chain_sync.m_sent_getheaders) {
                // They've run out of time to catch up!
                LogPrintf(
                    "Disconnecting outbound peer %d for old chain, best known "
                    "block = %s\n",
                    pto.GetId(),
                    state.pindexBestKnownBlock != nullptr
                        ? state.pindexBestKnownBlock->GetBlockHash().ToString()
                        : "<none>");
                pto.fDisconnect = true;
            } else {
                assert(state.m_chain_sync.m_work_header);
                LogPrint(
                    BCLog::NET,
                    "sending getheaders to outbound peer=%d to verify chain "
                    "work (current best known block:%s, benchmark blockhash: "
                    "%s)\n",
                    pto.GetId(),
                    state.pindexBestKnownBlock != nullptr
                        ? state.pindexBestKnownBlock->GetBlockHash().ToString()
                        : "<none>",
                    state.m_chain_sync.m_work_header->GetBlockHash()
                        .ToString());
                m_connman.PushMessage(
                    &pto,
                    msgMaker.Make(NetMsgType::GETHEADERS,
                                  m_chainman.ActiveChain().GetLocator(
                                      state.m_chain_sync.m_work_header->pprev),
                                  uint256()));
                state.m_chain_sync.m_sent_getheaders = true;
                // 2 minutes
                constexpr int64_t HEADERS_RESPONSE_TIME = 120;
                // Bump the timeout to allow a response, which could clear the
                // timeout (if the response shows the peer has synced), reset
                // the timeout (if the peer syncs to the required work but not
                // to our tip), or result in disconnect (if we advance to the
                // timeout and pindexBestKnownBlock has not sufficiently
                // progressed)
                state.m_chain_sync.m_timeout =
                    time_in_seconds + HEADERS_RESPONSE_TIME;
            }
        }
    }
}

void PeerManagerImpl::EvictExtraOutboundPeers(std::chrono::seconds now) {
    // If we have any extra block-relay-only peers, disconnect the youngest
    // unless it's given us a block -- in which case, compare with the
    // second-youngest, and out of those two, disconnect the peer who least
    // recently gave us a block.
    // The youngest block-relay-only peer would be the extra peer we connected
    // to temporarily in order to sync our tip; see net.cpp.
    // Note that we use higher nodeid as a measure for most recent connection.
    if (m_connman.GetExtraBlockRelayCount() > 0) {
        std::pair<NodeId, std::chrono::seconds> youngest_peer{-1, 0},
            next_youngest_peer{-1, 0};

        m_connman.ForEachNode([&](CNode *pnode) {
            if (!pnode->IsBlockOnlyConn() || pnode->fDisconnect) {
                return;
            }
            if (pnode->GetId() > youngest_peer.first) {
                next_youngest_peer = youngest_peer;
                youngest_peer.first = pnode->GetId();
                youngest_peer.second = pnode->m_last_block_time;
            }
        });

        NodeId to_disconnect = youngest_peer.first;
        if (youngest_peer.second > next_youngest_peer.second) {
            // Our newest block-relay-only peer gave us a block more recently;
            // disconnect our second youngest.
            to_disconnect = next_youngest_peer.first;
        }

        m_connman.ForNode(
            to_disconnect,
            [&](CNode *pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
                AssertLockHeld(::cs_main);
                // Make sure we're not getting a block right now, and that we've
                // been connected long enough for this eviction to happen at
                // all. Note that we only request blocks from a peer if we learn
                // of a valid headers chain with at least as much work as our
                // tip.
                CNodeState *node_state = State(pnode->GetId());
                if (node_state == nullptr ||
                    (now - pnode->m_connected >= MINIMUM_CONNECT_TIME &&
                     node_state->nBlocksInFlight == 0)) {
                    pnode->fDisconnect = true;
                    LogPrint(BCLog::NET,
                             "disconnecting extra block-relay-only peer=%d "
                             "(last block received at time %d)\n",
                             pnode->GetId(),
                             count_seconds(pnode->m_last_block_time));
                    return true;
                } else {
                    LogPrint(
                        BCLog::NET,
                        "keeping block-relay-only peer=%d chosen for eviction "
                        "(connect time: %d, blocks_in_flight: %d)\n",
                        pnode->GetId(), count_seconds(pnode->m_connected),
                        node_state->nBlocksInFlight);
                }
                return false;
            });
    }

    // Check whether we have too many OUTBOUND_FULL_RELAY peers
    if (m_connman.GetExtraFullOutboundCount() <= 0) {
        return;
    }

    // If we have more OUTBOUND_FULL_RELAY peers than we target, disconnect one.
    // Pick the OUTBOUND_FULL_RELAY peer that least recently announced us a new
    // block, with ties broken by choosing the more recent connection (higher
    // node id)
    NodeId worst_peer = -1;
    int64_t oldest_block_announcement = std::numeric_limits<int64_t>::max();

    m_connman.ForEachNode([&](CNode *pnode) EXCLUSIVE_LOCKS_REQUIRED(
                              ::cs_main) {
        AssertLockHeld(::cs_main);

        // Only consider OUTBOUND_FULL_RELAY peers that are not already marked
        // for disconnection
        if (!pnode->IsFullOutboundConn() || pnode->fDisconnect) {
            return;
        }
        CNodeState *state = State(pnode->GetId());
        if (state == nullptr) {
            // shouldn't be possible, but just in case
            return;
        }
        // Don't evict our protected peers
        if (state->m_chain_sync.m_protect) {
            return;
        }
        if (state->m_last_block_announcement < oldest_block_announcement ||
            (state->m_last_block_announcement == oldest_block_announcement &&
             pnode->GetId() > worst_peer)) {
            worst_peer = pnode->GetId();
            oldest_block_announcement = state->m_last_block_announcement;
        }
    });

    if (worst_peer == -1) {
        return;
    }

    bool disconnected = m_connman.ForNode(
        worst_peer, [&](CNode *pnode) EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
            AssertLockHeld(::cs_main);

            // Only disconnect a peer that has been connected to us for some
            // reasonable fraction of our check-frequency, to give it time for
            // new information to have arrived. Also don't disconnect any peer
            // we're trying to download a block from.
            CNodeState &state = *State(pnode->GetId());
            if (now - pnode->m_connected > MINIMUM_CONNECT_TIME &&
                state.nBlocksInFlight == 0) {
                LogPrint(BCLog::NET,
                         "disconnecting extra outbound peer=%d (last block "
                         "announcement received at time %d)\n",
                         pnode->GetId(), oldest_block_announcement);
                pnode->fDisconnect = true;
                return true;
            } else {
                LogPrint(BCLog::NET,
                         "keeping outbound peer=%d chosen for eviction "
                         "(connect time: %d, blocks_in_flight: %d)\n",
                         pnode->GetId(), count_seconds(pnode->m_connected),
                         state.nBlocksInFlight);
                return false;
            }
        });

    if (disconnected) {
        // If we disconnected an extra peer, that means we successfully
        // connected to at least one peer after the last time we detected a
        // stale tip. Don't try any more extra peers until we next detect a
        // stale tip, to limit the load we put on the network from these extra
        // connections.
        m_connman.SetTryNewOutboundPeer(false);
    }
}

void PeerManagerImpl::CheckForStaleTipAndEvictPeers() {
    LOCK(cs_main);

    auto now{GetTime<std::chrono::seconds>()};

    EvictExtraOutboundPeers(now);

    if (now > m_stale_tip_check_time) {
        // Check whether our tip is stale, and if so, allow using an extra
        // outbound peer.
        if (!fImporting && !fReindex && m_connman.GetNetworkActive() &&
            m_connman.GetUseAddrmanOutgoing() && TipMayBeStale()) {
            LogPrintf("Potential stale tip detected, will try using extra "
                      "outbound peer (last tip update: %d seconds ago)\n",
                      count_seconds(now) - count_seconds(m_last_tip_update));
            m_connman.SetTryNewOutboundPeer(true);
        } else if (m_connman.GetTryNewOutboundPeer()) {
            m_connman.SetTryNewOutboundPeer(false);
        }
        m_stale_tip_check_time = now + STALE_CHECK_INTERVAL;
    }

    if (!m_initial_sync_finished && CanDirectFetch()) {
        m_connman.StartExtraBlockRelayPeers();
        m_initial_sync_finished = true;
    }
}

void PeerManagerImpl::MaybeSendPing(CNode &node_to, Peer &peer,
                                    std::chrono::microseconds now) {
    if (m_connman.ShouldRunInactivityChecks(
            node_to, std::chrono::duration_cast<std::chrono::seconds>(now)) &&
        peer.m_ping_nonce_sent &&
        now > peer.m_ping_start.load() + TIMEOUT_INTERVAL) {
        // The ping timeout is using mocktime. To disable the check during
        // testing, increase -peertimeout.
        LogPrint(BCLog::NET, "ping timeout: %fs peer=%d\n",
                 0.000001 * count_microseconds(now - peer.m_ping_start.load()),
                 peer.m_id);
        node_to.fDisconnect = true;
        return;
    }

    const CNetMsgMaker msgMaker(node_to.GetCommonVersion());
    bool pingSend = false;

    if (peer.m_ping_queued) {
        // RPC ping request by user
        pingSend = true;
    }

    if (peer.m_ping_nonce_sent == 0 &&
        now > peer.m_ping_start.load() + PING_INTERVAL) {
        // Ping automatically sent as a latency probe & keepalive.
        pingSend = true;
    }

    if (pingSend) {
        uint64_t nonce = 0;
        while (nonce == 0) {
            GetRandBytes((uint8_t *)&nonce, sizeof(nonce));
        }
        peer.m_ping_queued = false;
        peer.m_ping_start = now;
        if (node_to.GetCommonVersion() > BIP0031_VERSION) {
            peer.m_ping_nonce_sent = nonce;
            m_connman.PushMessage(&node_to,
                                  msgMaker.Make(NetMsgType::PING, nonce));
        } else {
            // Peer is too old to support ping command with nonce, pong will
            // never arrive.
            peer.m_ping_nonce_sent = 0;
            m_connman.PushMessage(&node_to, msgMaker.Make(NetMsgType::PING));
        }
    }
}

void PeerManagerImpl::MaybeSendAddr(CNode &node, Peer &peer,
                                    std::chrono::microseconds current_time) {
    // Nothing to do for non-address-relay peers
    if (!peer.m_addr_relay_enabled) {
        return;
    }

    LOCK(peer.m_addr_send_times_mutex);
    if (fListen && !m_chainman.ActiveChainstate().IsInitialBlockDownload() &&
        peer.m_next_local_addr_send < current_time) {
        // If we've sent before, clear the bloom filter for the peer, so
        // that our self-announcement will actually go out. This might
        // be unnecessary if the bloom filter has already rolled over
        // since our last self-announcement, but there is only a small
        // bandwidth cost that we can incur by doing this (which happens
        // once a day on average).
        if (peer.m_next_local_addr_send != 0us) {
            peer.m_addr_known->reset();
        }
        if (std::optional<CAddress> local_addr = GetLocalAddrForPeer(&node)) {
            FastRandomContext insecure_rand;
            PushAddress(peer, *local_addr, insecure_rand);
        }
        peer.m_next_local_addr_send =
            PoissonNextSend(current_time, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
    }

    // We sent an `addr` message to this peer recently. Nothing more to do.
    if (current_time <= peer.m_next_addr_send) {
        return;
    }

    peer.m_next_addr_send =
        PoissonNextSend(current_time, AVG_ADDRESS_BROADCAST_INTERVAL);

    const size_t max_addr_to_send = GetMaxAddrToSend();
    if (!Assume(peer.m_addrs_to_send.size() <= max_addr_to_send)) {
        // Should be impossible since we always check size before adding to
        // m_addrs_to_send. Recover by trimming the vector.
        peer.m_addrs_to_send.resize(max_addr_to_send);
    }

    // Remove addr records that the peer already knows about, and add new
    // addrs to the m_addr_known filter on the same pass.
    auto addr_already_known = [&peer](const CAddress &addr) {
        bool ret = peer.m_addr_known->contains(addr.GetKey());
        if (!ret) {
            peer.m_addr_known->insert(addr.GetKey());
        }
        return ret;
    };
    peer.m_addrs_to_send.erase(std::remove_if(peer.m_addrs_to_send.begin(),
                                              peer.m_addrs_to_send.end(),
                                              addr_already_known),
                               peer.m_addrs_to_send.end());

    // No addr messages to send
    if (peer.m_addrs_to_send.empty()) {
        return;
    }

    const char *msg_type;
    int make_flags;
    if (peer.m_wants_addrv2) {
        msg_type = NetMsgType::ADDRV2;
        make_flags = ADDRV2_FORMAT;
    } else {
        msg_type = NetMsgType::ADDR;
        make_flags = 0;
    }
    m_connman.PushMessage(
        &node, CNetMsgMaker(node.GetCommonVersion())
                   .Make(make_flags, msg_type, peer.m_addrs_to_send));
    peer.m_addrs_to_send.clear();

    // we only send the big addr message once
    if (peer.m_addrs_to_send.capacity() > 40) {
        peer.m_addrs_to_send.shrink_to_fit();
    }
}

void PeerManagerImpl::MaybeSendFeefilter(
    CNode &pto, std::chrono::microseconds current_time) {
    if (m_ignore_incoming_txs) {
        return;
    }
    if (!pto.m_tx_relay) {
        return;
    }
    if (pto.GetCommonVersion() < FEEFILTER_VERSION) {
        return;
    }
    // peers with the forcerelay permission should not filter txs to us
    if (pto.HasPermission(NetPermissionFlags::ForceRelay)) {
        return;
    }

    Amount currentFilter =
        m_mempool
            .GetMinFee(
                gArgs.GetIntArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) *
                1000000)
            .GetFeePerK();
    static FeeFilterRounder g_filter_rounder{
        CFeeRate{DEFAULT_MIN_RELAY_TX_FEE_PER_KB}};

    if (m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
        // Received tx-inv messages are discarded when the active
        // chainstate is in IBD, so tell the peer to not send them.
        currentFilter = MAX_MONEY;
    } else {
        static const Amount MAX_FILTER{g_filter_rounder.round(MAX_MONEY)};
        if (pto.m_tx_relay->lastSentFeeFilter == MAX_FILTER) {
            // Send the current filter if we sent MAX_FILTER previously
            // and made it out of IBD.
            pto.m_tx_relay->m_next_send_feefilter = 0us;
        }
    }
    if (current_time > pto.m_tx_relay->m_next_send_feefilter) {
        Amount filterToSend = g_filter_rounder.round(currentFilter);
        // We always have a fee filter of at least minRelayTxFee
        filterToSend = std::max(filterToSend, ::minRelayTxFee.GetFeePerK());
        if (filterToSend != pto.m_tx_relay->lastSentFeeFilter) {
            m_connman.PushMessage(
                &pto, CNetMsgMaker(pto.GetCommonVersion())
                          .Make(NetMsgType::FEEFILTER, filterToSend));
            pto.m_tx_relay->lastSentFeeFilter = filterToSend;
        }
        pto.m_tx_relay->m_next_send_feefilter =
            PoissonNextSend(current_time, AVG_FEEFILTER_BROADCAST_INTERVAL);
    }
    // If the fee filter has changed substantially and it's still more than
    // MAX_FEEFILTER_CHANGE_DELAY until scheduled broadcast, then move the
    // broadcast to within MAX_FEEFILTER_CHANGE_DELAY.
    else if (current_time + MAX_FEEFILTER_CHANGE_DELAY <
                 pto.m_tx_relay->m_next_send_feefilter &&
             (currentFilter < 3 * pto.m_tx_relay->lastSentFeeFilter / 4 ||
              currentFilter > 4 * pto.m_tx_relay->lastSentFeeFilter / 3)) {
        pto.m_tx_relay->m_next_send_feefilter =
            current_time + GetRandomDuration<std::chrono::microseconds>(
                               MAX_FEEFILTER_CHANGE_DELAY);
    }
}

namespace {
class CompareInvMempoolOrder {
    CTxMemPool *mp;

public:
    explicit CompareInvMempoolOrder(CTxMemPool *_mempool) : mp(_mempool) {}

    bool operator()(std::set<TxId>::iterator a, std::set<TxId>::iterator b) {
        /**
         * As std::make_heap produces a max-heap, we want the entries which
         * are topologically earlier to sort later.
         */
        return mp->CompareTopologically(*b, *a);
    }
};
} // namespace

bool PeerManagerImpl::SetupAddressRelay(const CNode &node, Peer &peer) {
    // We don't participate in addr relay with outbound block-relay-only
    // connections to prevent providing adversaries with the additional
    // information of addr traffic to infer the link.
    if (node.IsBlockOnlyConn()) {
        return false;
    }

    if (!peer.m_addr_relay_enabled.exchange(true)) {
        // First addr message we have received from the peer, initialize
        // m_addr_known
        peer.m_addr_known = std::make_unique<CRollingBloomFilter>(5000, 0.001);
    }

    return true;
}

bool PeerManagerImpl::SendMessages(const Config &config, CNode *pto) {
    PeerRef peer = GetPeerRef(pto->GetId());
    if (!peer) {
        return false;
    }
    const Consensus::Params &consensusParams = m_chainparams.GetConsensus();

    // We must call MaybeDiscourageAndDisconnect first, to ensure that we'll
    // disconnect misbehaving peers even before the version handshake is
    // complete.
    if (MaybeDiscourageAndDisconnect(*pto, *peer)) {
        return true;
    }

    // Don't send anything until the version handshake is complete
    if (!pto->fSuccessfullyConnected || pto->fDisconnect) {
        return true;
    }

    // If we get here, the outgoing message serialization version is set and
    // can't change.
    const CNetMsgMaker msgMaker(pto->GetCommonVersion());

    const auto current_time = GetTime<std::chrono::microseconds>();

    if (pto->IsAddrFetchConn() &&
        current_time - pto->m_connected > 10 * AVG_ADDRESS_BROADCAST_INTERVAL) {
        LogPrint(BCLog::NET,
                 "addrfetch connection timeout; disconnecting peer=%d\n",
                 pto->GetId());
        pto->fDisconnect = true;
        return true;
    }

    MaybeSendPing(*pto, *peer, current_time);

    // MaybeSendPing may have marked peer for disconnection
    if (pto->fDisconnect) {
        return true;
    }

    bool fFetch;

    MaybeSendAddr(*pto, *peer, current_time);

    {
        LOCK(cs_main);

        CNodeState &state = *State(pto->GetId());

        // Start block sync
        if (m_chainman.m_best_header == nullptr) {
            m_chainman.m_best_header = m_chainman.ActiveChain().Tip();
        }

        // Download if this is a nice peer, or we have no nice peers and this
        // one might do.
        fFetch = state.fPreferredDownload ||
                 (nPreferredDownload == 0 && !pto->fClient &&
                  !pto->IsAddrFetchConn());

        if (!state.fSyncStarted && !pto->fClient && !fImporting && !fReindex) {
            // Only actively request headers from a single peer, unless we're
            // close to today.
            if ((nSyncStarted == 0 && fFetch) ||
                m_chainman.m_best_header->GetBlockTime() >
                    GetAdjustedTime() - 24 * 60 * 60) {
                state.fSyncStarted = true;
                state.m_headers_sync_timeout =
                    current_time + HEADERS_DOWNLOAD_TIMEOUT_BASE +
                    (
                        // Convert HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER to
                        // microseconds before scaling to maintain precision
                        std::chrono::microseconds{
                            HEADERS_DOWNLOAD_TIMEOUT_PER_HEADER} *
                        (GetAdjustedTime() -
                         m_chainman.m_best_header->GetBlockTime()) /
                        consensusParams.nPowTargetSpacing);
                nSyncStarted++;
                const CBlockIndex *pindexStart = m_chainman.m_best_header;
                /**
                 * If possible, start at the block preceding the currently best
                 * known header. This ensures that we always get a non-empty
                 * list of headers back as long as the peer is up-to-date. With
                 * a non-empty response, we can initialise the peer's known best
                 * block. This wouldn't be possible if we requested starting at
                 * m_best_header and got back an empty response.
                 */
                if (pindexStart->pprev) {
                    pindexStart = pindexStart->pprev;
                }

                LogPrint(
                    BCLog::NET,
                    "initial getheaders (%d) to peer=%d (startheight:%d)\n",
                    pindexStart->nHeight, pto->GetId(),
                    peer->m_starting_height);
                m_connman.PushMessage(
                    pto, msgMaker.Make(
                             NetMsgType::GETHEADERS,
                             m_chainman.ActiveChain().GetLocator(pindexStart),
                             uint256()));
            }
        }

        //
        // Try sending block announcements via headers
        //
        {
            // If we have less than MAX_BLOCKS_TO_ANNOUNCE in our list of block
            // hashes we're relaying, and our peer wants headers announcements,
            // then find the first header not yet known to our peer but would
            // connect, and send. If no header would connect, or if we have too
            // many blocks, or if the peer doesn't want headers, just add all to
            // the inv queue.
            LOCK(peer->m_block_inv_mutex);
            std::vector<CBlock> vHeaders;
            bool fRevertToInv =
                ((!state.fPreferHeaders &&
                  (!state.fPreferHeaderAndIDs ||
                   peer->m_blocks_for_headers_relay.size() > 1)) ||
                 peer->m_blocks_for_headers_relay.size() >
                     MAX_BLOCKS_TO_ANNOUNCE);
            // last header queued for delivery
            const CBlockIndex *pBestIndex = nullptr;
            // ensure pindexBestKnownBlock is up-to-date
            ProcessBlockAvailability(pto->GetId());

            if (!fRevertToInv) {
                bool fFoundStartingHeader = false;
                // Try to find first header that our peer doesn't have, and then
                // send all headers past that one. If we come across an headers
                // that aren't on m_chainman.ActiveChain(), give up.
                for (const BlockHash &hash : peer->m_blocks_for_headers_relay) {
                    const CBlockIndex *pindex =
                        m_chainman.m_blockman.LookupBlockIndex(hash);
                    assert(pindex);
                    if (m_chainman.ActiveChain()[pindex->nHeight] != pindex) {
                        // Bail out if we reorged away from this block
                        fRevertToInv = true;
                        break;
                    }
                    if (pBestIndex != nullptr && pindex->pprev != pBestIndex) {
                        // This means that the list of blocks to announce don't
                        // connect to each other. This shouldn't really be
                        // possible to hit during regular operation (because
                        // reorgs should take us to a chain that has some block
                        // not on the prior chain, which should be caught by the
                        // prior check), but one way this could happen is by
                        // using invalidateblock / reconsiderblock repeatedly on
                        // the tip, causing it to be added multiple times to
                        // m_blocks_for_headers_relay. Robustly deal with this
                        // rare situation by reverting to an inv.
                        fRevertToInv = true;
                        break;
                    }
                    pBestIndex = pindex;
                    if (fFoundStartingHeader) {
                        // add this to the headers message
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else if (PeerHasHeader(&state, pindex)) {
                        // Keep looking for the first new block.
                        continue;
                    } else if (pindex->pprev == nullptr ||
                               PeerHasHeader(&state, pindex->pprev)) {
                        // Peer doesn't have this header but they do have the
                        // prior one. Start sending headers.
                        fFoundStartingHeader = true;
                        vHeaders.push_back(pindex->GetBlockHeader());
                    } else {
                        // Peer doesn't have this header or the prior one --
                        // nothing will connect, so bail out.
                        fRevertToInv = true;
                        break;
                    }
                }
            }
            if (!fRevertToInv && !vHeaders.empty()) {
                if (vHeaders.size() == 1 && state.fPreferHeaderAndIDs) {
                    // We only send up to 1 block as header-and-ids, as
                    // otherwise probably means we're doing an initial-ish-sync
                    // or they're slow.
                    LogPrint(BCLog::NET,
                             "%s sending header-and-ids %s to peer=%d\n",
                             __func__, vHeaders.front().GetHash().ToString(),
                             pto->GetId());

                    int nSendFlags = 0;

                    bool fGotBlockFromCache = false;
                    {
                        LOCK(cs_most_recent_block);
                        if (most_recent_block_hash ==
                            pBestIndex->GetBlockHash()) {
                            CBlockHeaderAndShortTxIDs cmpctblock(
                                *most_recent_block);
                            m_connman.PushMessage(
                                pto, msgMaker.Make(nSendFlags,
                                                   NetMsgType::CMPCTBLOCK,
                                                   cmpctblock));
                            fGotBlockFromCache = true;
                        }
                    }
                    if (!fGotBlockFromCache) {
                        CBlock block;
                        bool ret = ReadBlockFromDisk(block, pBestIndex,
                                                     consensusParams);
                        assert(ret);
                        CBlockHeaderAndShortTxIDs cmpctblock(block);
                        m_connman.PushMessage(
                            pto,
                            msgMaker.Make(nSendFlags, NetMsgType::CMPCTBLOCK,
                                          cmpctblock));
                    }
                    state.pindexBestHeaderSent = pBestIndex;
                } else if (state.fPreferHeaders) {
                    if (vHeaders.size() > 1) {
                        LogPrint(BCLog::NET,
                                 "%s: %u headers, range (%s, %s), to peer=%d\n",
                                 __func__, vHeaders.size(),
                                 vHeaders.front().GetHash().ToString(),
                                 vHeaders.back().GetHash().ToString(),
                                 pto->GetId());
                    } else {
                        LogPrint(BCLog::NET,
                                 "%s: sending header %s to peer=%d\n", __func__,
                                 vHeaders.front().GetHash().ToString(),
                                 pto->GetId());
                    }
                    m_connman.PushMessage(
                        pto, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
                    state.pindexBestHeaderSent = pBestIndex;
                } else {
                    fRevertToInv = true;
                }
            }
            if (fRevertToInv) {
                // If falling back to using an inv, just try to inv the tip. The
                // last entry in m_blocks_for_headers_relay was our tip at some
                // point in the past.
                if (!peer->m_blocks_for_headers_relay.empty()) {
                    const BlockHash &hashToAnnounce =
                        peer->m_blocks_for_headers_relay.back();
                    const CBlockIndex *pindex =
                        m_chainman.m_blockman.LookupBlockIndex(hashToAnnounce);
                    assert(pindex);

                    // Warn if we're announcing a block that is not on the main
                    // chain. This should be very rare and could be optimized
                    // out. Just log for now.
                    if (m_chainman.ActiveChain()[pindex->nHeight] != pindex) {
                        LogPrint(
                            BCLog::NET,
                            "Announcing block %s not on main chain (tip=%s)\n",
                            hashToAnnounce.ToString(),
                            m_chainman.ActiveChain()
                                .Tip()
                                ->GetBlockHash()
                                .ToString());
                    }

                    // If the peer's chain has this block, don't inv it back.
                    if (!PeerHasHeader(&state, pindex)) {
                        peer->m_blocks_for_inv_relay.push_back(hashToAnnounce);
                        LogPrint(BCLog::NET,
                                 "%s: sending inv peer=%d hash=%s\n", __func__,
                                 pto->GetId(), hashToAnnounce.ToString());
                    }
                }
            }
            peer->m_blocks_for_headers_relay.clear();
        }
    } // release cs_main

    //
    // Message: inventory
    //
    std::vector<CInv> vInv;
    auto addInvAndMaybeFlush = [&](uint32_t type, const uint256 &hash) {
        vInv.emplace_back(type, hash);
        if (vInv.size() == MAX_INV_SZ) {
            m_connman.PushMessage(
                pto, msgMaker.Make(NetMsgType::INV, std::move(vInv)));
            vInv.clear();
        }
    };

    {
        LOCK(cs_main);

        {
            LOCK(peer->m_block_inv_mutex);

            vInv.reserve(std::max<size_t>(peer->m_blocks_for_inv_relay.size(),
                                          INVENTORY_BROADCAST_MAX_PER_MB *
                                              config.GetMaxBlockSize() /
                                              1000000));

            // Add blocks
            for (const BlockHash &hash : peer->m_blocks_for_inv_relay) {
                addInvAndMaybeFlush(MSG_BLOCK, hash);
            }
            peer->m_blocks_for_inv_relay.clear();
        }

        auto computeNextInvSendTime =
            [&](std::chrono::microseconds &next) -> bool {
            bool fSendTrickle = pto->HasPermission(NetPermissionFlags::NoBan);

            if (next < current_time) {
                fSendTrickle = true;
                if (pto->IsInboundConn()) {
                    next = m_connman.PoissonNextSendInbound(
                        current_time, INBOUND_INVENTORY_BROADCAST_INTERVAL);
                } else {
                    // Skip delay for outbound peers, as there is less privacy
                    // concern for them.
                    next = current_time;
                }
            }

            return fSendTrickle;
        };

        // Add proofs to inventory
        if (pto->m_proof_relay != nullptr) {
            LOCK(pto->m_proof_relay->cs_proof_inventory);

            if (computeNextInvSendTime(pto->m_proof_relay->nextInvSend)) {
                auto it = pto->m_proof_relay->setInventoryProofToSend.begin();
                while (it !=
                       pto->m_proof_relay->setInventoryProofToSend.end()) {
                    const avalanche::ProofId proofid = *it;

                    it = pto->m_proof_relay->setInventoryProofToSend.erase(it);

                    if (pto->m_proof_relay->filterProofKnown.contains(
                            proofid)) {
                        continue;
                    }

                    pto->m_proof_relay->filterProofKnown.insert(proofid);
                    addInvAndMaybeFlush(MSG_AVA_PROOF, proofid);
                    State(pto->GetId())
                        ->m_recently_announced_proofs.insert(proofid);
                }
            }
        }

        if (pto->m_tx_relay != nullptr) {
            LOCK(pto->m_tx_relay->cs_tx_inventory);
            // Check whether periodic sends should happen
            const bool fSendTrickle =
                computeNextInvSendTime(pto->m_tx_relay->nNextInvSend);

            // Time to send but the peer has requested we not relay
            // transactions.
            if (fSendTrickle) {
                LOCK(pto->m_tx_relay->cs_filter);
                if (!pto->m_tx_relay->fRelayTxes) {
                    pto->m_tx_relay->setInventoryTxToSend.clear();
                }
            }

            // Respond to BIP35 mempool requests
            if (fSendTrickle && pto->m_tx_relay->fSendMempool) {
                auto vtxinfo = m_mempool.infoAll();
                pto->m_tx_relay->fSendMempool = false;
                const CFeeRate filterrate{pto->m_tx_relay->minFeeFilter.load()};

                LOCK(pto->m_tx_relay->cs_filter);

                for (const auto &txinfo : vtxinfo) {
                    const TxId &txid = txinfo.tx->GetId();
                    pto->m_tx_relay->setInventoryTxToSend.erase(txid);
                    // Don't send transactions that peers will not put into
                    // their mempool
                    if (txinfo.fee < filterrate.GetFee(txinfo.vsize)) {
                        continue;
                    }
                    if (pto->m_tx_relay->pfilter &&
                        !pto->m_tx_relay->pfilter->IsRelevantAndUpdate(
                            *txinfo.tx)) {
                        continue;
                    }
                    pto->m_tx_relay->filterInventoryKnown.insert(txid);
                    // Responses to MEMPOOL requests bypass the
                    // m_recently_announced_invs filter.
                    addInvAndMaybeFlush(MSG_TX, txid);
                }
                pto->m_tx_relay->m_last_mempool_req =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        current_time);
            }

            // Determine transactions to relay
            if (fSendTrickle) {
                // Produce a vector with all candidates for sending
                std::vector<std::set<TxId>::iterator> vInvTx;
                vInvTx.reserve(pto->m_tx_relay->setInventoryTxToSend.size());
                for (std::set<TxId>::iterator it =
                         pto->m_tx_relay->setInventoryTxToSend.begin();
                     it != pto->m_tx_relay->setInventoryTxToSend.end(); it++) {
                    vInvTx.push_back(it);
                }
                const CFeeRate filterrate{pto->m_tx_relay->minFeeFilter.load()};
                // Send out the inventory in the order of admission to our
                // mempool, which is guaranteed to be a topological sort order.
                // A heap is used so that not all items need sorting if only a
                // few are being sent.
                CompareInvMempoolOrder compareInvMempoolOrder(&m_mempool);
                std::make_heap(vInvTx.begin(), vInvTx.end(),
                               compareInvMempoolOrder);
                // No reason to drain out at many times the network's
                // capacity, especially since we have many peers and some
                // will draw much shorter delays.
                unsigned int nRelayedTransactions = 0;
                LOCK(pto->m_tx_relay->cs_filter);
                while (!vInvTx.empty() &&
                       nRelayedTransactions < INVENTORY_BROADCAST_MAX_PER_MB *
                                                  config.GetMaxBlockSize() /
                                                  1000000) {
                    // Fetch the top element from the heap
                    std::pop_heap(vInvTx.begin(), vInvTx.end(),
                                  compareInvMempoolOrder);
                    std::set<TxId>::iterator it = vInvTx.back();
                    vInvTx.pop_back();
                    const TxId txid = *it;
                    // Remove it from the to-be-sent set
                    pto->m_tx_relay->setInventoryTxToSend.erase(it);
                    // Check if not in the filter already
                    if (pto->m_tx_relay->filterInventoryKnown.contains(txid)) {
                        continue;
                    }
                    // Not in the mempool anymore? don't bother sending it.
                    auto txinfo = m_mempool.info(txid);
                    if (!txinfo.tx) {
                        continue;
                    }
                    // Peer told you to not send transactions at that
                    // feerate? Don't bother sending it.
                    if (txinfo.fee < filterrate.GetFee(txinfo.vsize)) {
                        continue;
                    }
                    if (pto->m_tx_relay->pfilter &&
                        !pto->m_tx_relay->pfilter->IsRelevantAndUpdate(
                            *txinfo.tx)) {
                        continue;
                    }
                    // Send
                    State(pto->GetId())->m_recently_announced_invs.insert(txid);
                    addInvAndMaybeFlush(MSG_TX, txid);
                    nRelayedTransactions++;
                    {
                        // Expire old relay messages
                        while (!g_relay_expiration.empty() &&
                               g_relay_expiration.front().first <
                                   current_time) {
                            mapRelay.erase(g_relay_expiration.front().second);
                            g_relay_expiration.pop_front();
                        }

                        auto ret = mapRelay.insert(
                            std::make_pair(txid, std::move(txinfo.tx)));
                        if (ret.second) {
                            g_relay_expiration.push_back(std::make_pair(
                                current_time + RELAY_TX_CACHE_TIME, ret.first));
                        }
                    }
                    pto->m_tx_relay->filterInventoryKnown.insert(txid);
                }
            }
        }
    } // release cs_main

    if (!vInv.empty()) {
        m_connman.PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
    }

    {
        LOCK(cs_main);

        CNodeState &state = *State(pto->GetId());

        // Detect whether we're stalling
        if (state.m_stalling_since.count() &&
            state.m_stalling_since < current_time - BLOCK_STALLING_TIMEOUT) {
            // Stalling only triggers when the block download window cannot
            // move. During normal steady state, the download window should be
            // much larger than the to-be-downloaded set of blocks, so
            // disconnection should only happen during initial block download.
            LogPrintf("Peer=%d is stalling block download, disconnecting\n",
                      pto->GetId());
            pto->fDisconnect = true;
            return true;
        }
        // In case there is a block that has been in flight from this peer for
        // block_interval * (1 + 0.5 * N) (with N the number of peers from which
        // we're downloading validated blocks), disconnect due to timeout.
        // We compensate for other peers to prevent killing off peers due to our
        // own downstream link being saturated. We only count validated
        // in-flight blocks so peers can't advertise non-existing block hashes
        // to unreasonably increase our timeout.
        if (state.vBlocksInFlight.size() > 0) {
            QueuedBlock &queuedBlock = state.vBlocksInFlight.front();
            int nOtherPeersWithValidatedDownloads =
                m_peers_downloading_from - 1;
            if (current_time >
                state.m_downloading_since +
                    std::chrono::seconds{consensusParams.nPowTargetSpacing} *
                        (BLOCK_DOWNLOAD_TIMEOUT_BASE +
                         BLOCK_DOWNLOAD_TIMEOUT_PER_PEER *
                             nOtherPeersWithValidatedDownloads)) {
                LogPrintf("Timeout downloading block %s from peer=%d, "
                          "disconnecting\n",
                          queuedBlock.pindex->GetBlockHash().ToString(),
                          pto->GetId());
                pto->fDisconnect = true;
                return true;
            }
        }

        // Check for headers sync timeouts
        if (state.fSyncStarted &&
            state.m_headers_sync_timeout < std::chrono::microseconds::max()) {
            // Detect whether this is a stalling initial-headers-sync peer
            if (m_chainman.m_best_header->GetBlockTime() <=
                GetAdjustedTime() - 24 * 60 * 60) {
                if (current_time > state.m_headers_sync_timeout &&
                    nSyncStarted == 1 &&
                    (nPreferredDownload - state.fPreferredDownload >= 1)) {
                    // Disconnect a peer (without NetPermissionFlags::NoBan
                    // permission) if it is our only sync peer, and we have
                    // others we could be using instead. Note: If all our peers
                    // are inbound, then we won't disconnect our sync peer for
                    // stalling; we have bigger problems if we can't get any
                    // outbound peers.
                    if (!pto->HasPermission(NetPermissionFlags::NoBan)) {
                        LogPrintf("Timeout downloading headers from peer=%d, "
                                  "disconnecting\n",
                                  pto->GetId());
                        pto->fDisconnect = true;
                        return true;
                    } else {
                        LogPrintf("Timeout downloading headers from noban "
                                  "peer=%d, not disconnecting\n",
                                  pto->GetId());
                        // Reset the headers sync state so that we have a chance
                        // to try downloading from a different peer. Note: this
                        // will also result in at least one more getheaders
                        // message to be sent to this peer (eventually).
                        state.fSyncStarted = false;
                        nSyncStarted--;
                        state.m_headers_sync_timeout = 0us;
                    }
                }
            } else {
                // After we've caught up once, reset the timeout so we can't
                // trigger disconnect later.
                state.m_headers_sync_timeout = std::chrono::microseconds::max();
            }
        }

        // Check that outbound peers have reasonable chains GetTime() is used by
        // this anti-DoS logic so we can test this using mocktime.
        ConsiderEviction(*pto, GetTime());
    } // release cs_main

    std::vector<CInv> vGetData;

    //
    // Message: getdata (blocks)
    //
    {
        LOCK(cs_main);

        CNodeState &state = *State(pto->GetId());

        if (!pto->fClient &&
            ((fFetch && !pto->m_limited_node) ||
             !m_chainman.ActiveChainstate().IsInitialBlockDownload()) &&
            state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            std::vector<const CBlockIndex *> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(),
                                     MAX_BLOCKS_IN_TRANSIT_PER_PEER -
                                         state.nBlocksInFlight,
                                     vToDownload, staller);
            for (const CBlockIndex *pindex : vToDownload) {
                vGetData.push_back(CInv(MSG_BLOCK, pindex->GetBlockHash()));
                BlockRequested(config, pto->GetId(), *pindex);
                LogPrint(BCLog::NET, "Requesting block %s (%d) peer=%d\n",
                         pindex->GetBlockHash().ToString(), pindex->nHeight,
                         pto->GetId());
            }
            if (state.nBlocksInFlight == 0 && staller != -1) {
                if (State(staller)->m_stalling_since == 0us) {
                    State(staller)->m_stalling_since = current_time;
                    LogPrint(BCLog::NET, "Stall started peer=%d\n", staller);
                }
            }
        }
    } // release cs_main

    auto addGetDataAndMaybeFlush = [&](uint32_t type, const uint256 &hash) {
        CInv inv(type, hash);
        LogPrint(BCLog::NET, "Requesting %s from peer=%d\n", inv.ToString(),
                 pto->GetId());
        vGetData.push_back(std::move(inv));
        if (vGetData.size() >= MAX_GETDATA_SZ) {
            m_connman.PushMessage(
                pto, msgMaker.Make(NetMsgType::GETDATA, std::move(vGetData)));
            vGetData.clear();
        }
    };

    //
    // Message: getdata (proof)
    //
    {
        LOCK(cs_proofrequest);
        std::vector<std::pair<NodeId, avalanche::ProofId>> expired;
        auto requestable =
            m_proofrequest.GetRequestable(pto->GetId(), current_time, &expired);
        for (const auto &entry : expired) {
            LogPrint(BCLog::AVALANCHE,
                     "timeout of inflight proof %s from peer=%d\n",
                     entry.second.ToString(), entry.first);
        }
        for (const auto &proofid : requestable) {
            if (!AlreadyHaveProof(proofid)) {
                addGetDataAndMaybeFlush(MSG_AVA_PROOF, proofid);
                m_proofrequest.RequestedData(
                    pto->GetId(), proofid,
                    current_time + PROOF_REQUEST_PARAMS.getdata_interval);
            } else {
                // We have already seen this proof, no need to download.
                // This is just a belt-and-suspenders, as this should
                // already be called whenever a proof becomes
                // AlreadyHaveProof().
                m_proofrequest.ForgetInvId(proofid);
            }
        }
    } // release cs_proofrequest

    //
    // Message: getdata (transactions)
    //
    {
        LOCK(cs_main);
        std::vector<std::pair<NodeId, TxId>> expired;
        auto requestable =
            m_txrequest.GetRequestable(pto->GetId(), current_time, &expired);
        for (const auto &entry : expired) {
            LogPrint(BCLog::NET, "timeout of inflight tx %s from peer=%d\n",
                     entry.second.ToString(), entry.first);
        }
        for (const TxId &txid : requestable) {
            if (!AlreadyHaveTx(txid)) {
                addGetDataAndMaybeFlush(MSG_TX, txid);
                m_txrequest.RequestedData(
                    pto->GetId(), txid,
                    current_time + TX_REQUEST_PARAMS.getdata_interval);
            } else {
                // We have already seen this transaction, no need to download.
                // This is just a belt-and-suspenders, as this should already be
                // called whenever a transaction becomes AlreadyHaveTx().
                m_txrequest.ForgetInvId(txid);
            }
        }

        if (!vGetData.empty()) {
            m_connman.PushMessage(pto,
                                  msgMaker.Make(NetMsgType::GETDATA, vGetData));
        }

    } // release cs_main
    MaybeSendFeefilter(*pto, current_time);
    return true;
}

bool PeerManagerImpl::ReceivedAvalancheProof(CNode &peer,
                                             const avalanche::ProofRef &proof) {
    assert(proof != nullptr);

    const avalanche::ProofId &proofid = proof->getId();

    peer.AddKnownProof(proofid);

    if (m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
        // We cannot reliably verify proofs during IBD, so bail out early and
        // keep the inventory as pending so it can be requested when the node
        // has synced.
        return true;
    }

    const NodeId nodeid = peer.GetId();

    {
        LOCK(cs_proofrequest);
        m_proofrequest.ReceivedResponse(nodeid, proofid);

        if (AlreadyHaveProof(proofid)) {
            m_proofrequest.ForgetInvId(proofid);
            return true;
        }
    }

    // registerProof should not be called while cs_proofrequest because it
    // holds cs_main and that creates a potential deadlock during shutdown

    avalanche::ProofRegistrationState state;
    if (g_avalanche->withPeerManager([&](avalanche::PeerManager &pm) {
            return pm.registerProof(proof, state);
        })) {
        WITH_LOCK(cs_proofrequest, m_proofrequest.ForgetInvId(proofid));
        RelayProof(proofid);

        peer.m_last_proof_time = GetTime<std::chrono::seconds>();

        LogPrint(BCLog::NET, "New avalanche proof: peer=%d, proofid %s\n",
                 nodeid, proofid.ToString());
    }

    if (state.GetResult() == avalanche::ProofRegistrationResult::INVALID) {
        WITH_LOCK(cs_invalidProofs, invalidProofs->insert(proofid));
        Misbehaving(nodeid, 100, state.GetRejectReason());
        return false;
    }

    if (state.GetResult() == avalanche::ProofRegistrationResult::MISSING_UTXO) {
        // This is possible that a proof contains a utxo we don't know yet, so
        // don't ban for this.
        return false;
    }

    if (!g_avalanche->addToReconcile(proof)) {
        LogPrint(BCLog::AVALANCHE,
                 "Not polling the avalanche proof (%s): peer=%d, proofid %s\n",
                 state.IsValid() ? "not-worth-polling"
                                 : state.GetRejectReason(),
                 nodeid, proofid.ToString());
    }

    return true;
}
